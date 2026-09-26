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


/* ---- A few widgets' worth of drawing (the look of Vexa's own apps) ---- */

#define VX_COLOR_WINDOW 0x1a1030     /* Backgrounds. */
#define VX_COLOR_VIEW 0x120b22       /* Lists and text areas. */
#define VX_COLOR_TEXT 0xe4dcf2
#define VX_COLOR_DIM 0x8a80a3
#define VX_COLOR_ACCENT 0xb07cff
#define VX_COLOR_SELECTED 0x5b3a96
#define VX_COLOR_BUTTON 0x2c1d4a
#define VX_COLOR_BUTTON_HOT 0x3f2a66
#define VX_COLOR_LINE 0x3a2a5c

/* A rectangle's outline, one pixel wide, inside it. */
void vx_draw_outline(struct vx_surface *s, int x, int y, int width, int height, uint32_t color);
/* Text cut to `width` pixels (ending in "..." when it doesn't fit). */
void vx_draw_text_fit(struct vx_surface *s, int x, int y, int width, const char *text,
                      uint32_t fg, uint32_t bg);
/* A push button with its label centered; `hot`: under the pointer. */
void vx_draw_button(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                    bool hot);
/* A one-line text field: the text (its end, if it's long), and a cursor
 * after it when `focused`. */
void vx_draw_field(struct vx_surface *s, int x, int y, int width, const char *text, bool focused);
/* Editing a field's text (at most size - 1 characters) with a key event:
 * characters are added, Backspace removes one. True if it changed. */
bool vx_field_key(char *text, size_t size, const struct vx_gui_event *event);
/* True if (px, py) is inside the rectangle. */
bool vx_inside(int px, int py, int x, int y, int width, int height);

/* A pop-up menu (a right click's), drawn at (x, y) in a surface. An item
 * without a label is a line between groups. */
struct vx_menu_item {
    const char *label;
    const char *keys;  /* A shortcut shown on the right, or NULL. */
    bool disabled;
};
#define VX_MENU_ITEM_HEIGHT 22
#define VX_MENU_SEPARATOR_HEIGHT 9
/* Its size. */
void vx_menu_size(const struct vx_menu_item *items, int count, int *width, int *height);
/* Draws it; `hot` is the item under the pointer (or -1). */
void vx_draw_menu(struct vx_surface *s, int x, int y, const struct vx_menu_item *items, int count,
                  int hot);
/* The item at (px, py), or -1 (outside, a line, or disabled). */
int vx_menu_item_at(const struct vx_menu_item *items, int count, int x, int y, int px, int py);

/* ---- Images ---- */

struct vx_image {
    struct vx_surface surface; /* 0xRRGGBB, stride == width. */
};

/* Reads a PNG (8 bits per channel, not interlaced), BMP (24 or 32 bits) or
 * PPM (P6) file. Transparent pixels are blended onto `background`, or with
 * VX_IMAGE_ALPHA kept: pixels are then 0xAARRGGBB, for vx_blit_alpha. NULL
 * if the file can't be read or decoded. */
#define VX_IMAGE_ALPHA 0xff000000u
struct vx_image *vx_image_load(const char *path, uint32_t background);
struct vx_image *vx_image_decode(const void *data, size_t size, uint32_t background);
void vx_image_free(struct vx_image *image);
/* Draws `from` scaled into a rectangle of `to` (nearest pixel). */
void vx_blit_scaled(struct vx_surface *to, int x, int y, int width, int height,
                    const struct vx_surface *from);
/* Draws a VX_IMAGE_ALPHA image scaled into a rectangle of `to`, blended
 * onto what's there (made smaller, pixels are averaged: icons). */
void vx_blit_alpha(struct vx_surface *to, int x, int y, int width, int height,
                   const struct vx_surface *from);

/* ---- The desktop ---- */

/* A notification on the desktop for a few seconds ("title: text"). */
void vx_notify(const char *text);
/* Asks the desktop to read its settings (/etc/desktop.conf) again. */
void vx_desktop_reload(void);

#endif
