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

void vx_draw_char(struct vx_surface *s, int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_GLYPH_COUNT) {
        c = ' ';
    }
    const uint8_t *rows = font_glyphs[c - FONT_FIRST_CHAR];
    for (int r = 0; r < FONT_HEIGHT; r++) {
        int py = y + r;
        if (py < 0 || py >= s->height) {
            continue;
        }
        uint32_t *line = s->pixels + (long)py * s->stride;
        for (int col = 0; col < FONT_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= s->width) {
                continue;
            }
            if (rows[r] & (0x80 >> col)) {
                line[px] = fg;
            } else if (bg != VX_TRANSPARENT) {
                line[px] = bg;
            }
        }
    }
}

int vx_draw_text(struct vx_surface *s, int x, int y, const char *text, uint32_t fg, uint32_t bg) {
    for (; *text; text++, x += FONT_WIDTH) {
        vx_draw_char(s, x, y, *text, fg, bg);
    }
    return x;
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

/* Waits for the desktop's answer of this type; events that come first wait
 * in the queue. Returns 0, or an error if the desktop is gone. */
static long wait_reply(uint32_t type, struct desktop_message *reply) {
    for (;;) {
        long error = receive_message(reply);
        if (error) {
            return error;
        }
        if (reply->type == type) {
            return 0;
        }
        if (queued_count < QUEUE_MAX) {
            queued[queued_count++] = *reply;
        }
    }
}

struct vx_window *vx_window_create_flags(const char *title, int width, int height,
                                         unsigned flags) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || connect_desktop() < 0) {
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

    struct desktop_message m = {.type = DESKTOP_CREATE, .a = width, .b = height,
                                .c = flags & VX_WINDOW_RESIZABLE ? DESKTOP_RESIZABLE : 0};
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
    return window;
}

struct vx_window *vx_window_create(const char *title, int width, int height) {
    return vx_window_create_flags(title, width, height, 0);
}

int vx_window_resize(struct vx_window *window, int width, int height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return -VX_EINVAL;
    }
    char path[64];
    void *pixels;
    int handle = make_buffer(width, height, path, sizeof(path), &pixels);
    if (handle < 0) {
        return handle;
    }
    struct desktop_message m = {.type = DESKTOP_BUFFER, .window = (uint32_t)window->id,
                                .a = width, .b = height};
    strncpy(m.text, path, sizeof(m.text) - 1);
    struct desktop_message reply;
    long error = send_message(&m);
    if (!error) {
        /* Answers for this window only; other windows' can't be mixed up. */
        do {
            error = wait_reply(DESKTOP_RESIZED, &reply);
        } while (!error && reply.window != (uint32_t)window->id);
    }
    vx_remove(path);
    if (error || reply.a == 0) {
        vx_unmap(pixels, buffer_size(width, height));
        vx_close(handle);
        return error ? (int)error : -VX_EINVAL;
    }
    vx_unmap(window->surface.pixels, buffer_size(window->surface.width, window->surface.height));
    vx_close(window->buffer_handle);
    window->surface = (struct vx_surface){pixels, width, height, width};
    window->buffer_handle = handle;
    return 0;
}

void vx_window_present(struct vx_window *window, int x, int y, int width, int height) {
    struct desktop_message m = {.type = DESKTOP_PRESENT, .window = (uint32_t)window->id,
                                .a = x, .b = y, .c = width, .d = height};
    send_message(&m);
}

void vx_window_set_title(struct vx_window *window, const char *title) {
    struct desktop_message m = {.type = DESKTOP_TITLE, .window = (uint32_t)window->id};
    strncpy(m.text, title, sizeof(m.text) - 1);
    send_message(&m);
}

void vx_window_destroy(struct vx_window *window) {
    struct desktop_message m = {.type = DESKTOP_DESTROY, .window = (uint32_t)window->id};
    send_message(&m);
    vx_unmap(window->surface.pixels, buffer_size(window->surface.width, window->surface.height));
    vx_close(window->buffer_handle);
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
            if (ready <= 0) {
                return (int)ready;
            }
            long error = receive_message(&m);
            if (error) {
                return (int)error;
            }
        }
        to_event(&m, event);
        if (event->type) {
            return 1;
        }
    }
}
