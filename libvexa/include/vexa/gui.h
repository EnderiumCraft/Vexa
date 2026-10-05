#ifndef LIBVEXA_GUI_H
#define LIBVEXA_GUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* ---- Text ----
 *
 * Text is UTF-8, drawn smooth with TrueType fonts (DejaVu, in /share/fonts):
 * a face at a size in pixels. Apps' text is the UI font (VX_UI_FONT_SIZE
 * Sans) in lines of VX_LINE_HEIGHT pixels; terminals and editors use the
 * monospaced face in cells of VX_CELL_WIDTH by VX_LINE_HEIGHT. Without the
 * font files, text is the console's 8x16 bitmap font. */
enum { VX_FACE_SANS, VX_FACE_BOLD, VX_FACE_MONO, VX_FACE_COUNT };
#define VX_UI_FONT_SIZE 13
#define VX_MONO_FONT_SIZE 13
#define VX_LINE_HEIGHT 16
#define VX_CELL_WIDTH 8
struct vx_font;
/* A face at a size (kept for the program's life: just ask again). */
const struct vx_font *vx_font(int face, int size);
const struct vx_font *vx_font_ui(void);
/* A line's height, and the baseline's distance from its top. */
int vx_font_height(const struct vx_font *font);
int vx_font_ascent(const struct vx_font *font);
/* Draws a line of text with its top at y (no wrapping); returns the x after
 * it. A `bg` other than VX_TRANSPARENT fills behind it first. */
int vx_text(struct vx_surface *s, const struct vx_font *font, int x, int y, const char *text,
            uint32_t fg, uint32_t bg);
int vx_text_width_font(const struct vx_font *font, const char *text);
/* The width of the text's first `length` bytes. */
int vx_text_width_bytes(const struct vx_font *font, const char *text, size_t length);
/* How many bytes of the text fit in `width` pixels (whole characters). */
size_t vx_text_fit_bytes(const struct vx_font *font, const char *text, int width);
/* In the UI font, centered in a VX_LINE_HEIGHT line whose top is y. */
int vx_draw_text(struct vx_surface *s, int x, int y, const char *text, uint32_t fg, uint32_t bg);
int vx_text_width(const char *text);
/* One character in a monospaced cell (VX_CELL_WIDTH by VX_LINE_HEIGHT). */
void vx_draw_char(struct vx_surface *s, int x, int y, uint32_t c, uint32_t fg, uint32_t bg);
/* The same with any font and cell (VX_FACE_MONO at another size, say): a
 * cell's width for the font, and drawing in one (centered in its height). */
int vx_font_cell_width(const struct vx_font *font);
void vx_draw_cell(struct vx_surface *s, const struct vx_font *font, int x, int y, int width,
                  int height, uint32_t c, uint32_t fg, uint32_t bg);
/* UTF-8: the character at *text (moving past it; 0xFFFD for a bad byte),
 * a character's bytes (returns how many), and where the character before
 * byte `at` starts. */
uint32_t vx_utf8_next(const char **text);
int vx_utf8_encode(uint32_t c, char out[4]);
size_t vx_utf8_previous(const char *text, size_t at);

/* Copies a rectangle of `from` (at fx, fy) to `to` (at tx, ty), clipped to both. */
void vx_blit(struct vx_surface *to, int tx, int ty, const struct vx_surface *from, int fx, int fy,
             int width, int height);

/* ---- Windows ---- */

struct vx_window {
    int id;
    struct vx_surface surface; /* Draw here, then vx_window_present. */
    int buffer_handle;
    int cursor;
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
/* The pointer's shape over the window (until it's set again). */
enum { VX_CURSOR_ARROW, VX_CURSOR_TEXT, VX_CURSOR_HAND, VX_CURSOR_WAIT, VX_CURSOR_CROSS,
       VX_CURSOR_MOVE, VX_CURSOR_COUNT };
void vx_window_set_cursor(struct vx_window *window, int shape);
/* Files dragged out of the window, let go where the pointer is now (a
 * pointer event outside the window, with the button up: the desktop keeps
 * sending them while a button that was pressed in the window is down). The
 * desktop puts them there: on the desktop, or in another window (a
 * VX_GUI_DROP event for it). */
void vx_window_drag_files(struct vx_window *window, const char *const *paths, int count, bool copy);
void vx_window_destroy(struct vx_window *window);

enum vx_gui_event_type {
    VX_GUI_KEY = 1,     /* key (VX_KEY_*), value (1 down, 0 up, 2 repeat), character */
    VX_GUI_POINTER = 2, /* x, y (in the window), buttons (bit 0 left, 1 right, 2 middle), wheel */
    VX_GUI_CLOSE = 3,   /* The user asked to close the window. */
    VX_GUI_FOCUS = 4,   /* value: 1 gained, 0 lost */
    VX_GUI_RESIZE = 5,  /* width, height: what the user asked for (see vx_window_resize) */
    VX_GUI_THEME = 6,   /* The theme changed (vx_theme has the new one): draw again. */
    VX_GUI_DROP = 7,    /* x, y; value 1 to copy; text: files dropped (vx_drop_paths) */
};

struct vx_gui_event {
    int type;
    int window;
    int x, y, buttons, wheel;
    int key, value;
    int character; /* What the key types (Unicode), or 0 (arrows...). Ctrl+letter gives 1-26. */
    int width, height;
    char text[104];
};

/* The paths of a VX_GUI_DROP, one per line, in a new string to free(); NULL
 * if they can't be read. */
char *vx_drop_paths(const struct vx_gui_event *event);

/* Waits up to timeout_ms (-1: no limit) for an event. Returns 1 with one,
 * 0 on timeout, or a negative VX_E* error (-VX_EPIPE: the desktop is gone). */
int vx_gui_wait(struct vx_gui_event *event, long timeout_ms);
/* The connection's handle, to vx_poll it along with others. */
int vx_gui_handle(void);


/* ---- The theme: Vexa's look, dark or light, with an accent color ----
 *
 * Settings (Appearance) chooses them ("theme" and "accent" in
 * /etc/desktop.conf); every window follows at once: the desktop tells the
 * programs, libvexa reads the theme again, and vx_gui_wait returns a
 * VX_GUI_THEME event so the program draws itself again. */

struct vx_theme {
    bool dark;
    uint32_t window;     /* Backgrounds. */
    uint32_t view;       /* Lists and text areas. */
    uint32_t text, dim;  /* Text, and less important text. */
    uint32_t accent;     /* Highlights: the chosen color. */
    uint32_t selected;   /* Behind what's selected. */
    uint32_t button, button_hot;
    uint32_t line;       /* Outlines and separators. */
    uint32_t sidebar, stripe, shadow;
    uint32_t panel, menu, title, title_focused, title_text; /* The desktop's. */
};
extern struct vx_theme vx_theme;

/* The accent colors to choose from. */
struct vx_accent {
    const char *name, *label;
    uint32_t color;
};
extern const struct vx_accent vx_accents[];
extern const int vx_accent_count;

/* Makes the theme from a theme name ("dark", "light") and an accent name. */
void vx_theme_make(struct vx_theme *theme, const char *name, const char *accent);
/* Reads the theme from /etc/desktop.conf into vx_theme (windows do this
 * themselves when they open, and when it changes). */
void vx_theme_load(void);
/* theme=auto is dark at night: from VX_THEME_NIGHT_STARTS (local time, hours)
 * to VX_THEME_DAY_STARTS. True if it's night now. */
#define VX_THEME_NIGHT_STARTS 19
#define VX_THEME_DAY_STARTS 7
bool vx_theme_night(void);
/* The default wallpaper (DESKTOP_DEFAULT_WALLPAPER) in a theme's colors:
 * /share/pictures/glass/<accent>-<light|dark>.png. */
void vx_theme_wallpaper(const struct vx_theme *theme, char *out, size_t size);

#define VX_COLOR_WINDOW (vx_theme.window)
#define VX_COLOR_VIEW (vx_theme.view)
#define VX_COLOR_TEXT (vx_theme.text)
#define VX_COLOR_DIM (vx_theme.dim)
#define VX_COLOR_ACCENT (vx_theme.accent)
#define VX_COLOR_SELECTED (vx_theme.selected)
#define VX_COLOR_BUTTON (vx_theme.button)
#define VX_COLOR_BUTTON_HOT (vx_theme.button_hot)
#define VX_COLOR_LINE (vx_theme.line)

/* Mixes two colors: `amount` of `b` (0 to 255) into `a`. */
uint32_t vx_mix(uint32_t a, uint32_t b, int amount);

/* ---- A few widgets' worth of drawing (the look of Vexa's own apps) ---- */

/* Vexa's look is glossy, like gel or glass: round shapes, light on their top
 * half, a glow along the bottom. */

/* How far in from the side row `row` (counted from the top or the bottom)
 * of a round corner of `radius` starts, and how much of the pixel just
 * outside it is covered (0 to 255), for a smooth edge. */
int vx_corner_inset(int radius, int row, int *coverage);
/* A rectangle with round corners, filled: `alpha` (0 to 255) of `color`
 * over what's there. */
void vx_fill_rounded(struct vx_surface *s, int x, int y, int width, int height, int radius,
                     uint32_t color, int alpha);
/* A glossy shape with round corners (like Aqua's buttons): `color`, lighter
 * and shiny on top, deeper in the middle, glowing at the bottom, with a
 * darker edge. */
void vx_draw_gel(struct vx_surface *s, int x, int y, int width, int height, int radius,
                 uint32_t color);
/* The color of a gel's row `row` of `height` (for drawing parts of one). */
uint32_t vx_gel_color(uint32_t color, int row, int height);

/* A rectangle's outline, one pixel wide, inside it. */
void vx_draw_outline(struct vx_surface *s, int x, int y, int width, int height, uint32_t color);
/* Text cut to `width` pixels (ending in "..." when it doesn't fit). */
void vx_draw_text_fit(struct vx_surface *s, int x, int y, int width, const char *text,
                      uint32_t fg, uint32_t bg);
/* A push button with its label centered; `hot`: under the pointer. */
void vx_draw_button(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                    bool hot);
/* A push button's states, for vx_draw_button_flags. */
#define VX_BUTTON_HOT 1      /* Under the pointer, or chosen: the accent. */
#define VX_BUTTON_DISABLED 2 /* Greyed out. */
void vx_draw_button_flags(struct vx_surface *s, int x, int y, int width, int height,
                          const char *label, unsigned flags);
/* A toolbar's background across (x, y, width, height): brushed, lighter at
 * the top, with a line under it. */
void vx_draw_toolbar(struct vx_surface *s, int x, int y, int width, int height);
/* A tab: the chosen one a gel of the accent, the others plain. */
void vx_draw_tab(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                 bool chosen);
/* Behind a selected row or item in a list: a glossy bar. */
void vx_draw_selection(struct vx_surface *s, int x, int y, int width, int height);
/* A progress bar: `done` of `total` filled with the accent. */
void vx_draw_progress(struct vx_surface *s, int x, int y, int width, int height,
                      unsigned long long done, unsigned long long total);
/* A panel over the window's content (a question, a sheet): rounded, with a
 * soft shadow. */
void vx_draw_sheet(struct vx_surface *s, int x, int y, int width, int height);
/* A check box, 16 by 16: on, a gel of the accent with a white tick. */
void vx_draw_check(struct vx_surface *s, int x, int y, bool on);
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
/* Locks the screen (the lock screen asks for the password, if one is set). */
void vx_desktop_lock(void);

/* The clipboard: text (UTF-8), shared by every program, X programs too. */
void vx_clipboard_set(const char *text, size_t length);
/* Its text, in a new string to free() (empty if there's none); NULL if
 * memory runs out. */
char *vx_clipboard_get(void);

/* Open and Save dialogs: a window of their own, until the user chooses.
 * `folder` is where they start (NULL: the home folder); Save suggests
 * `name`. True, with the chosen path in `out`, or false if cancelled. */
bool vx_open_dialog(const char *title, const char *folder, char *out, size_t size);
bool vx_save_dialog(const char *title, const char *folder, const char *name, char *out,
                    size_t size);

/* Writes a surface as a PNG file: 0, or a negative error. */
int vx_image_save_png(const char *path, const struct vx_surface *surface);

#ifdef __cplusplus
}
#endif

#endif
