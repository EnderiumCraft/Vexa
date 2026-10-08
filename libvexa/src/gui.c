#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/desktop.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/syscall.h>

/* Drawing on surfaces, and the program side of the desktop protocol. */

/* ---- Drawing ---- */

static bool clip(const struct vx_surface *s, int *x, int *y, int *width, int *height) {
    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*x + *width > s->width) {
        *width = s->width - *x;
    }
    if (*y + *height > s->height) {
        *height = s->height - *y;
    }
    return *width > 0 && *height > 0;
}

void vx_fill(struct vx_surface *s, int x, int y, int width, int height, uint32_t color) {
    if (!clip(s, &x, &y, &width, &height)) {
        return;
    }
    for (int row = y; row < y + height; row++) {
        uint32_t *p = s->pixels + (long)row * s->stride + x;
        for (int i = 0; i < width; i++) {
            p[i] = color;
        }
    }
}

void vx_blit(struct vx_surface *to, int tx, int ty, const struct vx_surface *from, int fx, int fy,
             int width, int height) {
    /* Clip against the source, then the destination. */
    if (fx < 0) {
        tx -= fx, width += fx, fx = 0;
    }
    if (fy < 0) {
        ty -= fy, height += fy, fy = 0;
    }
    if (fx + width > from->width) {
        width = from->width - fx;
    }
    if (fy + height > from->height) {
        height = from->height - fy;
    }
    int x = tx, y = ty;
    if (!clip(to, &x, &y, &width, &height)) {
        return;
    }
    fx += x - tx;
    fy += y - ty;
    for (int row = 0; row < height; row++) {
        memcpy(to->pixels + (long)(y + row) * to->stride + x,
               from->pixels + (long)(fy + row) * from->stride + fx, (size_t)width * 4);
    }
}

/* ---- The desktop connection ---- */

#define QUEUE_MAX 64

static int connection = -1;
static struct desktop_message queued[QUEUE_MAX]; /* Events read while waiting for a reply. */
static int queued_count;
static int buffers_made;

static int connect_desktop(void) {
    if (connection >= 0) {
        return connection;
    }
    int handle = vx_socket(VX_AF_UNIX, VX_SOCK_SEQPACKET, 0);
    if (handle < 0) {
        return handle;
    }
    struct vx_socket_address address;
    memset(&address, 0, sizeof(address));
    address.local.family = VX_AF_UNIX;
    strcpy(address.local.path, DESKTOP_SOCKET);
    long error = vx_connect(handle, &address, sizeof(address.local));
    if (error) {
        vx_close(handle);
        return (int)error;
    }
    connection = handle;
    vx_theme_load(); /* The look the user chose, for the windows to come. */
    return handle;
}

int vx_gui_handle(void) {
    return connection;
}

static unsigned long buffer_size(int width, int height) {
    return ((unsigned long)width * height * 4 + 4095) & ~4095UL;
}

static long send_message(struct desktop_message *m) {
    return vx_write(connection, m, sizeof(*m)) == sizeof(*m) ? 0 : -VX_EPIPE;
}

static long receive_message(struct desktop_message *m) {
    long n = vx_read(connection, m, sizeof(*m));
    return n == sizeof(*m) ? 0 : n < 0 ? n : -VX_EPIPE;
}

/* Makes a pixel buffer: a file in /run/shm, mapped. Returns its handle (and
 * fills in path and pixels), or a negative error. */
static int make_buffer(int width, int height, char *path, size_t path_size, void **pixels) {
    snprintf(path, path_size, "/run/shm/window-%ld-%d", vx_process_id(), ++buffers_made);
    unsigned long size = buffer_size(width, height);
    int handle = vx_open(path, VX_OPEN_READ | VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0) {
        return handle;
    }
    *pixels = vx_resize(handle, size) == 0 ? vx_map_file(handle, 0, size, VX_MAP_WRITE) : NULL;
    if (!*pixels) {
        vx_close(handle);
        vx_remove(path);
        return -VX_ENOMEM;
    }
    return handle;
}

/* The windows this program has open (for DESKTOP_RELEASED, by id). */
static struct vx_window *open_windows[64];
static int open_count;

static void register_window(struct vx_window *window) {
    if (open_count < (int)(sizeof(open_windows) / sizeof(open_windows[0]))) {
        open_windows[open_count++] = window;
    }
}

static void unregister_window(struct vx_window *window) {
    for (int i = 0; i < open_count; i++) {
        if (open_windows[i] == window) {
            open_windows[i] = open_windows[--open_count];
            return;
        }
    }
}

/* Takes a DESKTOP_RELEASED (a buffer the desktop has stopped showing) as it
 * comes: true if the message was one. */
static bool absorb(const struct desktop_message *m) {
    if (m->type != DESKTOP_RELEASED) {
        return false;
    }
    for (int i = 0; i < open_count; i++) {
        if (open_windows[i]->id == (int)m->window && m->a >= 0 && m->a < 2) {
            open_windows[i]->busy[m->a] = false;
        }
    }
    return true;
}

/* Waits for the desktop's answer of this type; events that come first wait
 * in the queue. Returns 0, or an error if the desktop is gone. */
static long wait_reply(uint32_t type, struct desktop_message *reply) {
    for (;;) {
        long error = receive_message(reply);
        if (error) {
            return error;
        }
        if (absorb(reply)) {
            continue;
        }
        if (reply->type == type) {
            return 0;
        }
        if (queued_count < QUEUE_MAX) {
            queued[queued_count++] = *reply;
        }
    }
}

/* Waits (a little) until the desktop has stopped showing buffer `index`: the
 * program can't draw on it until then. Events read meanwhile wait in the
 * queue. A desktop that doesn't answer doesn't hold the program up for long. */
static void wait_release(struct vx_window *window, int index) {
    for (int tries = 0; window->busy[index] && tries < 20; tries++) {
        struct vx_poll poll = {connection, VX_POLL_READ, 0};
        if (vx_poll(&poll, 1, 25) <= 0) {
            continue;
        }
        struct desktop_message m;
        if (receive_message(&m)) {
            break;
        }
        if (!absorb(&m) && queued_count < QUEUE_MAX) {
            queued[queued_count++] = m;
        }
    }
    window->busy[index] = false;
}

struct vx_window *vx_window_create_flags(const char *title, int width, int height,
                                         unsigned flags) {
    if (connect_desktop() < 0) {
        return NULL;
    }
    if (flags & VX_WINDOW_FULLSCREEN && !vx_screen_size(&width, &height)) {
        return NULL;
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return NULL;
    }
    struct vx_window *window = calloc(1, sizeof(*window));
    if (!window) {
        return NULL;
    }
    /* The pixels: a file in /run/shm that the desktop maps too. */
    char path[64];
    void *pixels;
    int handle = make_buffer(width, height, path, sizeof(path), &pixels);
    if (handle < 0) {
        free(window);
        return NULL;
    }
    window->surface = (struct vx_surface){pixels, width, height, width};
    window->buffer_handle = handle;
    window->buffers[0] = window->surface;
    window->handles[0] = handle;
    window->handles[1] = -1;

    struct desktop_message m = {.type = DESKTOP_CREATE, .a = width, .b = height,
                                .c = (flags & VX_WINDOW_RESIZABLE ? DESKTOP_RESIZABLE : 0) |
                                     (flags & VX_WINDOW_FULLSCREEN ? DESKTOP_FULLSCREEN : 0)};
    size_t path_length = strlen(path);
    memcpy(m.text, path, path_length + 1);
    strncpy(m.text + path_length + 1, title ? title : "", sizeof(m.text) - path_length - 2);
    struct desktop_message reply;
    bool ok = send_message(&m) == 0 && wait_reply(DESKTOP_CREATED, &reply) == 0 && reply.window;
    vx_remove(path); /* Both sides have it mapped; the name isn't needed. */
    if (!ok) {
        vx_unmap(pixels, buffer_size(width, height));
        vx_close(handle);
        free(window);
        return NULL;
    }
    window->id = (int)reply.window;

    /* A second buffer, for drawing while the desktop shows the first. */
    char back_path[64];
    void *back_pixels;
    int back_handle = make_buffer(width, height, back_path, sizeof(back_path), &back_pixels);
    if (back_handle >= 0) {
        struct desktop_message b = {.type = DESKTOP_BACK_BUFFER, .window = reply.window,
                                    .a = width, .b = height};
        strncpy(b.text, back_path, sizeof(b.text) - 1);
        struct desktop_message answer;
        long error = send_message(&b);
        if (!error) {
            do {
                error = wait_reply(DESKTOP_RESIZED, &answer);
            } while (!error && answer.window != reply.window);
        }
        vx_remove(back_path);
        if (!error && answer.a) {
            window->buffers[1] = (struct vx_surface){back_pixels, width, height, width};
            window->handles[1] = back_handle;
            window->double_buffered = true;
            window->buffers[0] = window->surface;
        } else {
            vx_unmap(back_pixels, buffer_size(width, height));
            vx_close(back_handle);
        }
    }
    register_window(window);
    return window;
}

struct vx_window *vx_window_create(const char *title, int width, int height) {
    return vx_window_create_flags(title, width, height, 0);
}

int vx_window_resize(struct vx_window *window, int width, int height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return -VX_EINVAL;
    }
    char path[64], back_path[64];
    void *pixels, *back_pixels = NULL;
    int handle = make_buffer(width, height, path, sizeof(path), &pixels);
    if (handle < 0) {
        return handle;
    }
    int back_handle = -1;
    if (window->double_buffered) {
        back_handle = make_buffer(width, height, back_path, sizeof(back_path), &back_pixels);
        if (back_handle < 0) {
            vx_unmap(pixels, buffer_size(width, height));
            vx_close(handle);
            vx_remove(path);
            return back_handle;
        }
    }
    struct desktop_message m = {.type = DESKTOP_BUFFER, .window = (uint32_t)window->id,
                                .a = width, .b = height};
    size_t path_length = strlen(path);
    memcpy(m.text, path, path_length + 1);
    if (back_handle >= 0) {
        strncpy(m.text + path_length + 1, back_path, sizeof(m.text) - path_length - 2);
    }
    struct desktop_message reply;
    long error = send_message(&m);
    if (!error) {
        /* Answers for this window only; other windows' can't be mixed up. */
        do {
            error = wait_reply(DESKTOP_RESIZED, &reply);
        } while (!error && reply.window != (uint32_t)window->id);
    }
    vx_remove(path);
    if (back_handle >= 0) {
        vx_remove(back_path);
    }
    if (error || reply.a == 0) {
        vx_unmap(pixels, buffer_size(width, height));
        vx_close(handle);
        if (back_handle >= 0) {
            vx_unmap(back_pixels, buffer_size(width, height));
            vx_close(back_handle);
        }
        return error ? (int)error : -VX_EINVAL;
    }
    /* The old buffers go; the new ones start blank, the first one drawn on. */
    unsigned long old_size = buffer_size(window->surface.width, window->surface.height);
    for (int i = 0; i < 2; i++) {
        if (window->handles[i] >= 0 && window->buffers[i].pixels) {
            vx_unmap(window->buffers[i].pixels, old_size);
            vx_close(window->handles[i]);
        }
    }
    window->buffers[0] = (struct vx_surface){pixels, width, height, width};
    window->handles[0] = handle;
    window->buffers[1] = back_handle >= 0 ? (struct vx_surface){back_pixels, width, height, width}
                                          : (struct vx_surface){NULL, 0, 0, 0};
    window->handles[1] = back_handle;
    window->double_buffered = back_handle >= 0;
    window->back = window->shown = 0;
    window->busy[0] = window->busy[1] = false;
    window->surface = window->buffers[0];
    window->buffer_handle = handle;
    return 0;
}

/* Shows what's drawn on the window's surface (x, y, width, height of it). With
 * two buffers the next one to draw on becomes a copy of this one, so drawing
 * goes on from what's shown, and the desktop never reads what's being drawn. */
void vx_window_present(struct vx_window *window, int x, int y, int width, int height) {
    struct desktop_message m = {.type = DESKTOP_PRESENT, .window = (uint32_t)window->id,
                                .a = x, .b = y, .c = width, .d = height};
    m.text[0] = (char)(window->double_buffered ? window->back : 0);
    if (send_message(&m) || !window->double_buffered) {
        return;
    }
    if (window->back != window->shown) {
        window->busy[window->shown] = true; /* The desktop releases it now it shows the other. */
    }
    window->shown = window->back;
    int next = 1 - window->shown;
    if (window->busy[next]) {
        wait_release(window, next);
    }
    struct vx_surface *from = &window->buffers[window->shown];
    struct vx_surface *to = &window->buffers[next];
    memcpy(to->pixels, from->pixels, (size_t)from->width * from->height * 4);
    window->back = next;
    window->surface = *to;
}

void vx_window_set_title(struct vx_window *window, const char *title) {
    struct desktop_message m = {.type = DESKTOP_TITLE, .window = (uint32_t)window->id};
    strncpy(m.text, title, sizeof(m.text) - 1);
    send_message(&m);
}

bool vx_screen_size(int *width, int *height) {
    struct desktop_message m = {.type = DESKTOP_INFO}, reply;
    if (send_message(&m) || wait_reply(DESKTOP_INFO_REPLY, &reply)) {
        return false;
    }
    *width = reply.a;
    *height = reply.b;
    return true;
}

void vx_window_move(struct vx_window *window, int x, int y) {
    struct desktop_message m = {.type = DESKTOP_MOVE, .window = (uint32_t)window->id, .a = x,
                                .b = y};
    send_message(&m);
}

void vx_window_center(struct vx_window *window) {
    int width, height;
    if (vx_screen_size(&width, &height)) {
        int top = 26 + 28 + 4; /* The panel, and the title bar and border over the content. */
        int y = top + (height - top - window->surface.height) / 2;
        vx_window_move(window, (width - window->surface.width) / 2, y < top ? top : y);
    }
}

void vx_window_set_modified(struct vx_window *window, bool modified) {
    if (window->modified == modified) {
        return;
    }
    window->modified = modified;
    struct desktop_message m = {.type = DESKTOP_MODIFIED, .window = (uint32_t)window->id,
                                .a = modified};
    send_message(&m);
}

void vx_window_set_cursor(struct vx_window *window, int shape) {
    if (window->cursor == shape) {
        return;
    }
    window->cursor = shape;
    struct desktop_message m = {.type = DESKTOP_CURSOR, .window = (uint32_t)window->id, .a = shape};
    send_message(&m);
}

void vx_window_drag_files(struct vx_window *window, const char *const *paths, int count,
                          bool copy) {
    static int drags;
    struct desktop_message m = {.type = DESKTOP_DRAG, .window = (uint32_t)window->id, .a = copy};
    snprintf(m.text, sizeof(m.text), "/tmp/.drag-%ld-%d", vx_process_id(), ++drags);
    int handle = vx_open(m.text, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        vx_write(handle, paths[i], strlen(paths[i]));
        vx_write(handle, "\n", 1);
    }
    vx_close(handle);
    send_message(&m);
}

char *vx_drop_paths(const struct vx_gui_event *event) {
    int handle = vx_open(event->text, VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    size_t size = 0, capacity = 4096;
    char *text = malloc(capacity);
    long n;
    while (text && (n = vx_read(handle, text + size, capacity - size - 1)) > 0) {
        size += (size_t)n;
        if (capacity - size < 512) {
            char *more = realloc(text, capacity *= 2);
            if (!more) {
                free(text);
                text = NULL;
            }
            text = more;
        }
    }
    vx_close(handle);
    if (text) {
        text[size] = '\0';
    }
    return text;
}

void vx_window_destroy(struct vx_window *window) {
    struct desktop_message m = {.type = DESKTOP_DESTROY, .window = (uint32_t)window->id};
    send_message(&m);
    unregister_window(window);
    unsigned long size = buffer_size(window->surface.width, window->surface.height);
    for (int i = 0; i < 2; i++) {
        if (window->handles[i] >= 0 && window->buffers[i].pixels) {
            vx_unmap(window->buffers[i].pixels, size);
            vx_close(window->handles[i]);
        }
    }
    free(window);
}

static void to_event(const struct desktop_message *m, struct vx_gui_event *e) {
    memset(e, 0, sizeof(*e));
    e->window = (int)m->window;
    switch (m->type) {
    case DESKTOP_KEY:
        e->type = VX_GUI_KEY;
        e->key = m->a;
        e->value = m->b;
        e->character = m->c;
        break;
    case DESKTOP_POINTER:
        e->type = VX_GUI_POINTER;
        e->x = m->a;
        e->y = m->b;
        e->buttons = m->c;
        e->wheel = m->d;
        break;
    case DESKTOP_CLOSE:
        e->type = VX_GUI_CLOSE;
        break;
    case DESKTOP_FOCUS:
        e->type = VX_GUI_FOCUS;
        e->value = m->a;
        break;
    case DESKTOP_CONFIGURE:
        e->type = VX_GUI_RESIZE;
        e->width = m->a;
        e->height = m->b;
        break;
    case DESKTOP_THEME:
        vx_theme_load();
        e->type = VX_GUI_THEME;
        break;
    case DESKTOP_DROP:
        e->type = VX_GUI_DROP;
        e->x = m->a;
        e->y = m->b;
        e->value = m->c;
        memcpy(e->text, m->text, sizeof(e->text));
        e->text[sizeof(e->text) - 1] = '\0';
        break;
    }
}

int vx_gui_wait(struct vx_gui_event *event, long timeout_ms) {
    if (connection < 0) {
        return -VX_ENOTCONN;
    }
    for (;;) {
        struct desktop_message m;
        if (queued_count) {
            m = queued[0];
            memmove(queued, queued + 1, (size_t)--queued_count * sizeof(queued[0]));
        } else {
            struct vx_poll poll = {connection, VX_POLL_READ, 0};
            long ready = vx_poll(&poll, 1, timeout_ms);
            if (ready == -VX_EINTR) {
                continue; /* A signal that isn't ours to act on: wait on. */
            }
            if (ready <= 0) {
                return (int)ready;
            }
            long error = receive_message(&m);
            if (error) {
                return (int)error;
            }
        }
        if (absorb(&m)) {
            continue; /* (The desktop's release of a buffer: no event.) */
        }
        to_event(&m, event);
        if (event->type) {
            return 1;
        }
    }
}

void vx_notify(const char *text) {
    if (connect_desktop() < 0) {
        return;
    }
    struct desktop_message m = {.type = DESKTOP_NOTIFY};
    strncpy(m.text, text, sizeof(m.text) - 1);
    send_message(&m);
}

void vx_desktop_reload(void) {
    if (connect_desktop() < 0) {
        return;
    }
    struct desktop_message m = {.type = DESKTOP_RELOAD};
    send_message(&m);
}

void vx_desktop_lock(void) {
    if (connect_desktop() < 0) {
        return;
    }
    struct desktop_message m = {.type = DESKTOP_LOCK};
    send_message(&m);
}

void vx_clipboard_set(const char *text, size_t length) {
    /* Written aside, then renamed: a reader never sees half of it. */
    char temporary[64];
    snprintf(temporary, sizeof(temporary), "%s.%ld", DESKTOP_CLIPBOARD_FILE, vx_process_id());
    int handle = vx_open(temporary, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0) {
        return;
    }
    vx_handle_chmod(handle, 0600); /* Its owner's alone. */
    vx_write(handle, text, length);
    vx_close(handle);
    vx_rename(temporary, DESKTOP_CLIPBOARD_FILE);
    if (connect_desktop() >= 0) {
        struct desktop_message m = {.type = DESKTOP_CLIPBOARD_SET};
        send_message(&m);
    }
}

char *vx_clipboard_get(void) {
    int handle = vx_open(DESKTOP_CLIPBOARD_FILE, VX_OPEN_READ);
    size_t size = 0, capacity = 4096;
    char *text = malloc(capacity);
    if (!text) {
        if (handle >= 0) {
            vx_close(handle);
        }
        return NULL;
    }
    long n;
    while (handle >= 0 && (n = vx_read(handle, text + size, capacity - size - 1)) > 0) {
        size += (size_t)n;
        if (capacity - size < 1024) {
            char *more = realloc(text, capacity * 2);
            if (!more) {
                break;
            }
            text = more;
            capacity *= 2;
        }
    }
    if (handle >= 0) {
        vx_close(handle);
    }
    text[size] = '\0';
    return text;
}
