/* desktop: Vexa's graphical desktop (the compositor).
 *
 * It takes the screen, the keyboard and the mouse, and draws programs'
 * windows (buffers they share with it) with a title bar you can drag them
 * by. Clicking a window raises it and gives it the keyboard. Ctrl+Alt+T opens
 * a terminal, Ctrl+Alt+Q goes back to the text console.
 *
 * Programs talk to it over the local socket /run/desktop (see
 * <vexa/desktop.h>; <vexa/gui.h> has the easy way).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/desktop.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/syscall.h>

#define MAX_WINDOWS 32
#define MAX_CLIENTS 32
#define MAX_CHILDREN 32
#define TITLE_HEIGHT 22
#define BORDER 1

#define COLOR_BACKGROUND 0x0b0613
#define COLOR_BACKGROUND_TEXT 0x6e6485
#define COLOR_TITLE 0x2c1d4a
#define COLOR_TITLE_FOCUSED 0x5b3a96
#define COLOR_TITLE_TEXT 0xe4dcf2
#define COLOR_BORDER 0x3a2a5c
#define COLOR_CLOSE 0xff6b81

struct rect {
    int x, y, width, height;
};

struct client {
    int handle;
};

struct window {
    int id;
    int client; /* Index in clients[]. */
    char title[64];
    int x, y; /* The content's top-left corner on the screen. */
    struct vx_surface content;
    size_t mapped_size;
    int buffer_handle;
};

static struct vx_display_info display;
static uint32_t *frame;             /* The display's memory. */
static struct vx_surface screen;    /* Composed here, then copied to `frame`. */
static struct window *stack[MAX_WINDOWS]; /* Bottom to top. */
static int window_count, next_window_id = 1;
static struct window *focused;
static struct client clients[MAX_CLIENTS];
static int listener, keyboard = -1, mouse = -1;
static int children[MAX_CHILDREN];

static int pointer_x, pointer_y, buttons;
static struct window *dragging;
static int drag_dx, drag_dy;
static struct rect damage; /* What must be drawn again (width 0: nothing). */
static bool quit;

static bool shift, ctrl, alt, caps_lock;

/* ---- Rectangles and damage ---- */

static struct rect frame_rect(const struct window *w) {
    return (struct rect){w->x - BORDER, w->y - TITLE_HEIGHT - BORDER,
                         w->content.width + 2 * BORDER,
                         w->content.height + TITLE_HEIGHT + 2 * BORDER};
}

static bool inside(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

static void add_damage(struct rect r) {
    if (r.width <= 0 || r.height <= 0) {
        return;
    }
    if (damage.width == 0) {
        damage = r;
        return;
    }
    int x0 = r.x < damage.x ? r.x : damage.x;
    int y0 = r.y < damage.y ? r.y : damage.y;
    int x1 = r.x + r.width > damage.x + damage.width ? r.x + r.width : damage.x + damage.width;
    int y1 = r.y + r.height > damage.y + damage.height ? r.y + r.height
                                                        : damage.y + damage.height;
    damage = (struct rect){x0, y0, x1 - x0, y1 - y0};
}

/* ---- Drawing ---- */

#define CURSOR_WIDTH 12
#define CURSOR_HEIGHT 19

/* An arrow: '#' outline, '.' fill. */
static const char *const cursor_shape[CURSOR_HEIGHT] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
    "#.#  #..#   ", "##    #..#  ", "#     #..#  ", "       ##   ",
};

static struct rect cursor_rect(void) {
    return (struct rect){pointer_x, pointer_y, CURSOR_WIDTH, CURSOR_HEIGHT};
}

/* Draws everything that overlaps `area` into the screen buffer. */
static void compose(struct rect area) {
    /* A view of the screen buffer clipped to the area; coordinates shift. */
    if (area.x < 0) {
        area.width += area.x, area.x = 0;
    }
    if (area.y < 0) {
        area.height += area.y, area.y = 0;
    }
    if (area.x + area.width > screen.width) {
        area.width = screen.width - area.x;
    }
    if (area.y + area.height > screen.height) {
        area.height = screen.height - area.y;
    }
    if (area.width <= 0 || area.height <= 0) {
        return;
    }
    struct vx_surface view = {screen.pixels + (long)area.y * screen.stride + area.x, area.width,
                              area.height, screen.stride};
    int ox = -area.x, oy = -area.y;
    vx_fill(&view, 0, 0, area.width, area.height, COLOR_BACKGROUND);
    const char *hint = "Vexa    Ctrl+Alt+T: terminal    Ctrl+Alt+Q: back to the console";
    vx_draw_text(&view, ox + 16, oy + screen.height - FONT_HEIGHT - 12, hint,
                 COLOR_BACKGROUND_TEXT, VX_TRANSPARENT);
    for (int i = 0; i < window_count; i++) {
        struct window *w = stack[i];
        struct rect f = frame_rect(w);
        vx_fill(&view, ox + f.x, oy + f.y, f.width, f.height, COLOR_BORDER);
        vx_fill(&view, ox + w->x, oy + w->y - TITLE_HEIGHT, w->content.width, TITLE_HEIGHT,
                w == focused ? COLOR_TITLE_FOCUSED : COLOR_TITLE);
        vx_draw_text(&view, ox + w->x + 8, oy + w->y - TITLE_HEIGHT + 3, w->title,
                     COLOR_TITLE_TEXT, VX_TRANSPARENT);
        /* The close box: an x at the right end of the title bar. */
        vx_draw_char(&view, ox + w->x + w->content.width - 16, oy + w->y - TITLE_HEIGHT + 3,
                     'x', COLOR_CLOSE, VX_TRANSPARENT);
        vx_blit(&view, ox + w->x, oy + w->y, &w->content, 0, 0, w->content.width,
                w->content.height);
    }
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            char c = cursor_shape[row][col];
            int x = pointer_x + col + ox, y = pointer_y + row + oy;
            if (c != ' ' && x >= 0 && y >= 0 && x < view.width && y < view.height) {
                view.pixels[(long)y * view.stride + x] = c == '#' ? 0x000000 : 0xffffff;
            }
        }
    }
}

/* Copies the composed area to the display, converting the pixel format if
 * the display isn't 0xRRGGBB. */
static void show(struct rect area) {
    if (area.x < 0) {
        area.width += area.x, area.x = 0;
    }
    if (area.y < 0) {
        area.height += area.y, area.y = 0;
    }
    if (area.x + area.width > screen.width) {
        area.width = screen.width - area.x;
    }
    if (area.y + area.height > screen.height) {
        area.height = screen.height - area.y;
    }
    bool native = display.red_shift == 16 && display.green_shift == 8 && display.blue_shift == 0;
    for (int row = area.y; row < area.y + area.height; row++) {
        uint32_t *from = screen.pixels + (long)row * screen.stride + area.x;
        uint32_t *to = (uint32_t *)((char *)frame + (long)row * display.pitch) + area.x;
        if (native) {
            memcpy(to, from, (size_t)area.width * 4);
            continue;
        }
        for (int i = 0; i < area.width; i++) {
            uint32_t p = from[i];
            to[i] = ((p >> 16) & 0xff) << display.red_shift |
                    ((p >> 8) & 0xff) << display.green_shift | (p & 0xff) << display.blue_shift;
        }
    }
}

static void redraw_damage(void) {
    if (damage.width > 0) {
        compose(damage);
        show(damage);
        damage.width = 0;
    }
}

/* ---- Windows ---- */

static void send_to(int client, struct desktop_message *m) {
    if (client >= 0 && clients[client].handle >= 0) {
        struct vx_message message = {m, sizeof(*m), VX_MSG_DONTWAIT | VX_MSG_NOSIGNAL, 0, NULL};
        vx_send(clients[client].handle, &message); /* A client that doesn't keep up loses events. */
    }
}

static void set_focus(struct window *w) {
    if (focused == w) {
        return;
    }
    if (focused) {
        struct desktop_message m = {.type = DESKTOP_FOCUS, .window = (uint32_t)focused->id};
        send_to(focused->client, &m);
        add_damage(frame_rect(focused));
    }
    focused = w;
    if (w) {
        struct desktop_message m = {.type = DESKTOP_FOCUS, .window = (uint32_t)w->id, .a = 1};
        send_to(w->client, &m);
        add_damage(frame_rect(w));
    }
}

static void raise_window(struct window *w) {
    int at = 0;
    while (at < window_count && stack[at] != w) {
        at++;
    }
    if (at == window_count || at == window_count - 1) {
        return;
    }
    memmove(stack + at, stack + at + 1, (size_t)(window_count - at - 1) * sizeof(stack[0]));
    stack[window_count - 1] = w;
    add_damage(frame_rect(w));
}

static struct window *find_window(int client, uint32_t id) {
    for (int i = 0; i < window_count; i++) {
        if (stack[i]->id == (int)id && stack[i]->client == client) {
            return stack[i];
        }
    }
    return NULL;
}

static struct window *window_at(int x, int y) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (inside(frame_rect(stack[i]), x, y)) {
            return stack[i];
        }
    }
    return NULL;
}

static void destroy_window(struct window *w) {
    add_damage(frame_rect(w));
    int at = 0;
    while (stack[at] != w) {
        at++;
    }
    memmove(stack + at, stack + at + 1, (size_t)(window_count - at - 1) * sizeof(stack[0]));
    window_count--;
    if (dragging == w) {
        dragging = NULL;
    }
    if (focused == w) {
        focused = NULL;
        set_focus(window_count ? stack[window_count - 1] : NULL);
    }
    vx_unmap(w->content.pixels, w->mapped_size);
    vx_close(w->buffer_handle);
    printf("desktop: closed window %d \"%s\"\n", w->id, w->title);
    free(w);
}

static void create_window(int client, struct desktop_message *m) {
    struct desktop_message reply = {.type = DESKTOP_CREATED};
    m->text[sizeof(m->text) - 1] = '\0';
    const char *path = m->text;
    size_t path_length = strlen(path);
    const char *title = path_length + 1 < sizeof(m->text) ? path + path_length + 1 : "";
    int width = m->a, height = m->b;
    struct window *w = NULL;
    if (window_count < MAX_WINDOWS && width > 0 && height > 0 && width <= 4096 &&
        height <= 4096 && strncmp(path, "/run/shm/", 9) == 0) {
        w = calloc(1, sizeof(*w));
    }
    if (w) {
        w->mapped_size = ((size_t)width * height * 4 + 4095) & ~(size_t)4095;
        w->buffer_handle = vx_open(path, VX_OPEN_READ);
        void *pixels = w->buffer_handle >= 0
                           ? vx_map_file(w->buffer_handle, 0, w->mapped_size, 0)
                           : NULL;
        if (!pixels) {
            if (w->buffer_handle >= 0) {
                vx_close(w->buffer_handle);
            }
            free(w);
            w = NULL;
        } else {
            w->content = (struct vx_surface){pixels, width, height, width};
        }
    }
    if (w) {
        w->id = next_window_id++;
        w->client = client;
        strncpy(w->title, title, sizeof(w->title) - 1);
        /* Cascade new windows from the top left. */
        int n = (w->id - 1) % 8;
        w->x = 80 + 32 * n;
        w->y = 60 + TITLE_HEIGHT + 28 * n;
        if (w->x + width > screen.width) {
            w->x = screen.width > width ? (screen.width - width) / 2 : BORDER;
        }
        if (w->y + height > screen.height) {
            w->y = TITLE_HEIGHT + BORDER;
        }
        stack[window_count++] = w;
        add_damage(frame_rect(w));
        set_focus(w);
        reply.window = (uint32_t)w->id;
        printf("desktop: window %d \"%s\" (%dx%d) at %d,%d\n", w->id, w->title, width, height,
               w->x, w->y);
    }
    send_to(client, &reply);
}

static void drop_client(int client) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (stack[i]->client == client) {
            destroy_window(stack[i]);
        }
    }
    vx_close(clients[client].handle);
    clients[client].handle = -1;
}

static void client_message(int client) {
    struct desktop_message m;
    long n = vx_read(clients[client].handle, &m, sizeof(m));
    if (n != sizeof(m)) {
        if (n == -VX_EAGAIN) {
            return;
        }
        drop_client(client);
        return;
    }
    struct window *w = find_window(client, m.window);
    switch (m.type) {
    case DESKTOP_CREATE:
        create_window(client, &m);
        break;
    case DESKTOP_PRESENT:
        if (w) {
            add_damage((struct rect){w->x + m.a, w->y + m.b, m.c, m.d});
        }
        break;
    case DESKTOP_TITLE:
        if (w) {
            m.text[sizeof(m.text) - 1] = '\0';
            strncpy(w->title, m.text, sizeof(w->title) - 1);
            add_damage(frame_rect(w));
        }
        break;
    case DESKTOP_DESTROY:
        if (w) {
            destroy_window(w);
        }
        break;
    }
}

/* ---- Programs ---- */

static void launch(const char *path) {
    const char *argv[] = {path};
    struct vx_spawn spawn = {
        .argv = argv, .argc = 1, .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
    };
    int process = vx_spawn(path, &spawn);
    if (process < 0) {
        printf("desktop: can't start %s: %s\n", path, vx_strerror(process));
        return;
    }
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (children[i] < 0) {
            children[i] = process;
            return;
        }
    }
    vx_close(process);
}

static void reap_children(void) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (children[i] >= 0 && vx_wait(children[i], VX_WAIT_NO_HANG) != -VX_EAGAIN) {
            vx_close(children[i]);
            children[i] = -1;
        }
    }
}

/* ---- Input ---- */

/* What a key types on a US keyboard, by key code (Linux's, 0 to 57). */
static const char keymap[58] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char keymap_shift[58] =
    "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static int character_of(int key) {
    if (key < 0 || key >= (int)sizeof(keymap)) {
        return 0;
    }
    char c = shift ? keymap_shift[key] : keymap[key];
    if (caps_lock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c ^= 0x20;
    }
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c &= 0x1f;
    }
    return (unsigned char)c;
}

static void key_event(int key, int value) {
    bool down = value != 0;
    switch (key) {
    case VX_KEY_LEFTSHIFT:
    case VX_KEY_RIGHTSHIFT: shift = down; return;
    case VX_KEY_LEFTCTRL:
    case VX_KEY_RIGHTCTRL: ctrl = down; return;
    case VX_KEY_LEFTALT:
    case VX_KEY_RIGHTALT: alt = down; return;
    case VX_KEY_CAPSLOCK:
        if (value == 1) {
            caps_lock = !caps_lock;
        }
        return;
    }
    if (ctrl && alt && value == 1) {
        if (key == 20) { /* T */
            launch("/bin/term");
            return;
        }
        if (key == 16) { /* Q */
            quit = true;
            return;
        }
    }
    if (focused) {
        struct desktop_message m = {.type = DESKTOP_KEY, .window = (uint32_t)focused->id,
                                    .a = key, .b = value, .c = down ? character_of(key) : 0};
        send_to(focused->client, &m);
    }
}

static void send_pointer(struct window *w, int wheel) {
    struct desktop_message m = {.type = DESKTOP_POINTER, .window = (uint32_t)w->id,
                                .a = pointer_x - w->x, .b = pointer_y - w->y, .c = buttons,
                                .d = wheel};
    send_to(w->client, &m);
}

static void button_event(int bit, bool down) {
    int before = buttons;
    buttons = down ? buttons | bit : buttons & ~bit;
    struct window *w = window_at(pointer_x, pointer_y);
    if (bit == 1 && down && !(before & 1)) {
        printf("desktop: left button at %d,%d\n", pointer_x, pointer_y);
    }
    if (bit == 1 && down && !(before & 1) && w) {
        raise_window(w);
        set_focus(w);
        bool title = pointer_y < w->y;
        bool close = title && pointer_x >= w->x + w->content.width - 20;
        if (close) {
            struct desktop_message m = {.type = DESKTOP_CLOSE, .window = (uint32_t)w->id};
            send_to(w->client, &m);
            printf("desktop: asked window %d to close\n", w->id);
            return;
        }
        if (title) {
            dragging = w;
            drag_dx = pointer_x - w->x;
            drag_dy = pointer_y - w->y;
            return;
        }
    }
    if (bit == 1 && !down && dragging) {
        printf("desktop: moved window %d to %d,%d\n", dragging->id, dragging->x, dragging->y);
        dragging = NULL;
        return;
    }
    if (w && pointer_y >= w->y) {
        send_pointer(w, 0);
    }
}

static void pointer_moved(int dx, int dy, int wheel) {
    if (dx || dy) {
        add_damage(cursor_rect());
        pointer_x += dx;
        pointer_y += dy;
        pointer_x = pointer_x < 0 ? 0 : pointer_x >= screen.width ? screen.width - 1 : pointer_x;
        pointer_y = pointer_y < 0 ? 0 : pointer_y >= screen.height ? screen.height - 1 : pointer_y;
        add_damage(cursor_rect());
        if (dragging) {
            add_damage(frame_rect(dragging));
            dragging->x = pointer_x - drag_dx;
            dragging->y = pointer_y - drag_dy;
            if (dragging->y < TITLE_HEIGHT + BORDER) {
                dragging->y = TITLE_HEIGHT + BORDER; /* Keep the title bar on the screen. */
            }
            add_damage(frame_rect(dragging));
            return;
        }
    }
    struct window *w = window_at(pointer_x, pointer_y);
    if (w && pointer_y >= w->y) {
        send_pointer(w, wheel);
    }
}

static void read_input(int handle) {
    struct vx_input_event events[64];
    long n = vx_read(handle, events, sizeof(events));
    int dx = 0, dy = 0, wheel = 0;
    for (long i = 0; i < n / (long)sizeof(events[0]); i++) {
        struct vx_input_event *e = &events[i];
        if (e->type == VX_EV_KEY && e->code >= VX_BTN_LEFT && e->code <= VX_BTN_MIDDLE) {
            pointer_moved(dx, dy, wheel);
            dx = dy = wheel = 0;
            int bit = e->code == VX_BTN_LEFT ? 1 : e->code == VX_BTN_RIGHT ? 2 : 4;
            button_event(bit, e->value != 0);
        } else if (e->type == VX_EV_KEY) {
            key_event(e->code, e->value);
        } else if (e->type == VX_EV_REL) {
            if (e->code == VX_REL_X) {
                dx += e->value;
            } else if (e->code == VX_REL_Y) {
                dy += e->value;
            } else if (e->code == VX_REL_WHEEL) {
                wheel += e->value;
            }
        } else if (e->type == VX_EV_SYN && (dx || dy || wheel)) {
            pointer_moved(dx, dy, wheel);
            dx = dy = wheel = 0;
        }
    }
}

/* ---- Setup ---- */

static int open_input(unsigned capability) {
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int handle = vx_open(path, VX_OPEN_READ);
        if (handle < 0) {
            return -1;
        }
        struct vx_input_info info;
        int grab = 1;
        if (vx_control(handle, VX_INPUT_INFO, &info, sizeof(info)) == 0 &&
            (info.capabilities & capability) &&
            vx_control(handle, VX_INPUT_GRAB, &grab, sizeof(grab)) == 0) {
            return handle;
        }
        vx_close(handle);
    }
    return -1;
}

static bool setup(void) {
    int handle = vx_open("/dev/display0", VX_OPEN_READ | VX_OPEN_WRITE);
    if (handle < 0) {
        fprintf(stderr, "desktop: no display: %s\n", vx_strerror(handle));
        return false;
    }
    long error = vx_control(handle, VX_DISPLAY_INFO, &display, sizeof(display));
    if (!error && display.bits_per_pixel != 32) {
        fprintf(stderr, "desktop: the display isn't 32 bits per pixel\n");
        return false;
    }
    if (!error) {
        error = vx_control(handle, VX_DISPLAY_ACQUIRE, NULL, 0);
    }
    if (error) {
        fprintf(stderr, "desktop: can't have the display: %s\n", vx_strerror(error));
        return false;
    }
    frame = vx_map_file(handle, 0, display.size, VX_MAP_WRITE);
    screen.width = (int)display.width;
    screen.height = (int)display.height;
    screen.stride = screen.width;
    screen.pixels = vx_map((size_t)screen.width * screen.height * 4, VX_MAP_WRITE);
    if (!frame || !screen.pixels) {
        fprintf(stderr, "desktop: out of memory\n");
        return false;
    }
    keyboard = open_input(VX_INPUT_KEYS);
    mouse = open_input(VX_INPUT_POINTER);

    listener = vx_socket(VX_AF_UNIX, VX_SOCK_SEQPACKET | VX_SOCK_NONBLOCK, 0);
    struct vx_socket_address address;
    memset(&address, 0, sizeof(address));
    address.local.family = VX_AF_UNIX;
    strcpy(address.local.path, DESKTOP_SOCKET);
    vx_remove(DESKTOP_SOCKET); /* Left over from an earlier desktop. */
    if (listener < 0 || vx_bind(listener, &address, sizeof(address.local)) ||
        vx_listen(listener, 8)) {
        fprintf(stderr, "desktop: can't listen on %s\n", DESKTOP_SOCKET);
        return false;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].handle = -1;
    }
    for (int i = 0; i < MAX_CHILDREN; i++) {
        children[i] = -1;
    }
    pointer_x = screen.width / 2;
    pointer_y = screen.height / 2;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    return true;
}

int main(int argc, char **argv) {
    if (!setup()) {
        return 1;
    }
    printf("desktop: started on a %dx%d screen%s%s\n", screen.width, screen.height,
           keyboard >= 0 ? ", keyboard" : "", mouse >= 0 ? ", mouse" : "");
    fflush(stdout);
    /* The first program: a terminal, unless told otherwise. */
    launch(argc > 1 ? argv[1] : "/bin/term");

    while (!quit) {
        redraw_damage();
        fflush(stdout);
        struct vx_poll polls[3 + MAX_CLIENTS];
        int indexes[3 + MAX_CLIENTS];
        int count = 0;
        polls[count] = (struct vx_poll){listener, VX_POLL_READ, 0};
        indexes[count++] = -1;
        if (keyboard >= 0) {
            polls[count] = (struct vx_poll){keyboard, VX_POLL_READ, 0};
            indexes[count++] = -2;
        }
        if (mouse >= 0) {
            polls[count] = (struct vx_poll){mouse, VX_POLL_READ, 0};
            indexes[count++] = -3;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].handle >= 0) {
                polls[count] = (struct vx_poll){clients[i].handle, VX_POLL_READ, 0};
                indexes[count++] = i;
            }
        }
        if (vx_poll(polls, (size_t)count, 500) <= 0) {
            reap_children();
            continue;
        }
        for (int i = 0; i < count; i++) {
            if (!polls[i].ready) {
                continue;
            }
            if (indexes[i] == -1) {
                int handle = vx_accept(listener, NULL, 0);
                int slot = 0;
                while (handle >= 0 && slot < MAX_CLIENTS && clients[slot].handle >= 0) {
                    slot++;
                }
                if (handle >= 0 && slot < MAX_CLIENTS) {
                    clients[slot].handle = handle;
                } else if (handle >= 0) {
                    vx_close(handle);
                }
            } else if (indexes[i] < -1) {
                read_input(polls[i].handle);
            } else if (clients[indexes[i]].handle >= 0) {
                client_message(indexes[i]);
            }
        }
        reap_children();
    }
    printf("desktop: back to the console\n");
    return 0; /* Closing the display brings the console back. */
}
