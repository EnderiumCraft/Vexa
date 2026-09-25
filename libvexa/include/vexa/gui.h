#ifndef LIBVEXA_GUI_H
#define LIBVEXA_GUI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Drawing, and windows on the Vexa desktop.
 *
 *     struct vx_window *w = vx_window_create("Hello", 320, 200);
 *     vx_fill(&w->surface, 0, 0, 320, 200, 0x202040);
 *     vx_draw_text(&w->surface, 10, 10, "Hello", 0xffffff, VX_TRANSPARENT);
 *     vx_window_present(w, 0, 0, 320, 200);
 *     struct vx_gui_event e;
 *     while (vx_gui_wait(&e, -1) > 0 && e.type != VX_GUI_CLOSE) { ... }
 *
 * Colors are 0xRRGGBB.
 */

#define VX_TRANSPARENT 0xff000000u /* As a background: leave what's there. */

/* Pixels in memory: `stride` pixels from one row to the next. */
struct vx_surface {
    uint32_t *pixels;
    int width, height, stride;
};

void vx_fill(struct vx_surface *s, int x, int y, int width, int height, uint32_t color);
void vx_draw_char(struct vx_surface *s, int x, int y, char c, uint32_t fg, uint32_t bg);
/* Draws a line of text (no wrapping); returns the x after it. */
int vx_draw_text(struct vx_surface *s, int x, int y, const char *text, uint32_t fg, uint32_t bg);
/* Copies a rectangle of `from` (at fx, fy) to `to` (at tx, ty), clipped to both. */
void vx_blit(struct vx_surface *to, int tx, int ty, const struct vx_surface *from, int fx, int fy,
             int width, int height);

/* ---- Windows ---- */

struct vx_window {
    int id;
    struct vx_surface surface; /* Draw here, then vx_window_present. */
    int buffer_handle;
};

/* Opens a window (connecting to the desktop the first time). NULL if there
 * is no desktop running. */
struct vx_window *vx_window_create(const char *title, int width, int height);
/* The same, with VX_WINDOW_* flags. A resizable window gets VX_GUI_RESIZE
 * events when the user resizes or maximizes it. */
#define VX_WINDOW_RESIZABLE 0x1
struct vx_window *vx_window_create_flags(const char *title, int width, int height,
                                         unsigned flags);
/* Gives the window a new size: a new, blank surface to draw on (present it
 * all). Returns 0 or a negative VX_E* error (the old surface stays). */
int vx_window_resize(struct vx_window *window, int width, int height);
/* Shows what was drawn in the rectangle. */
void vx_window_present(struct vx_window *window, int x, int y, int width, int height);
void vx_window_set_title(struct vx_window *window, const char *title);
void vx_window_destroy(struct vx_window *window);

enum vx_gui_event_type {
    VX_GUI_KEY = 1,     /* key (VX_KEY_*), value (1 down, 0 up, 2 repeat), character */
    VX_GUI_POINTER = 2, /* x, y (in the window), buttons (bit 0 left, 1 right, 2 middle), wheel */
    VX_GUI_CLOSE = 3,   /* The user asked to close the window. */
    VX_GUI_FOCUS = 4,   /* value: 1 gained, 0 lost */
    VX_GUI_RESIZE = 5,  /* width, height: what the user asked for (see vx_window_resize) */
};

struct vx_gui_event {
    int type;
    int window;
    int x, y, buttons, wheel;
    int key, value;
    int character; /* What the key types, or 0 (arrows...). Ctrl+letter gives 1-26. */
    int width, height;
};

/* Waits up to timeout_ms (-1: no limit) for an event. Returns 1 with one,
 * 0 on timeout, or a negative VX_E* error (-VX_EPIPE: the desktop is gone). */
int vx_gui_wait(struct vx_gui_event *event, long timeout_ms);
/* The connection's handle, to vx_poll it along with others. */
int vx_gui_handle(void);

#endif
