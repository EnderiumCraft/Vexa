/* desktop: Vexa's graphical desktop (the compositor).
 *
 * It takes the screen, the keyboard and the mouse, and draws programs'
 * windows (buffers they share with it) with a title bar you can drag them
 * by, and buttons to minimize, maximize and close them; resizable windows
 * are resized by their right and bottom edges. A panel along the top has the
 * Vexa menu (programs to start), a button for each window and a clock.
 *
 * Clicking a window raises it and gives it the keyboard. Alt+Tab goes to the
 * next window, Ctrl+Alt+T opens a terminal, Ctrl+Alt+X starts X (Xvexa) with
 * an xterm, and Ctrl+Alt+Q goes back to the text console.
 *
 * Programs talk to it over the local socket /run/desktop (see
 * <vexa/desktop.h>; <vexa/gui.h> has the easy way).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/desktop.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/syscall.h>

#define MAX_WINDOWS 32
#define MAX_CLIENTS 32
#define MAX_CHILDREN 32
#define PANEL_HEIGHT 26
#define TITLE_HEIGHT 22
#define BORDER 1
#define GRIP 6         /* How far past a resizable window's edge you can grab it. */
#define BUTTON_WIDTH 20 /* The title bar's buttons. */
#define MIN_WIDTH 120
#define MIN_HEIGHT 60
#define DOUBLE_CLICK_MS 500

#define COLOR_PANEL 0x140c24
#define COLOR_PANEL_LINE 0x3a2a5c
#define COLOR_PANEL_TEXT 0xe4dcf2
#define COLOR_PANEL_DIM 0x8a80a3
#define COLOR_BUTTON 0x2c1d4a
#define COLOR_BUTTON_HOT 0x5b3a96
#define COLOR_BUTTON_OFF 0x1c1230
#define COLOR_TITLE 0x2c1d4a
#define COLOR_TITLE_FOCUSED 0x5b3a96
#define COLOR_TITLE_TEXT 0xe4dcf2
#define COLOR_BORDER 0x3a2a5c
#define COLOR_CLOSE 0xff6b81
#define COLOR_OUTLINE 0xb07cff
#define COLOR_MENU 0x1c1230
#define COLOR_MENU_HOT 0x5b3a96

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
    bool resizable, minimized, maximized;
    bool popup; /* A menu or tooltip: no frame, always on top, no keyboard. */
    bool undecorated; /* It draws its own title bar: no frame. */
    struct rect restore; /* Where it was before it was maximized (content). */
};

static struct vx_display_info display;
static uint32_t *frame;             /* The display's memory. */
static struct vx_surface screen;    /* Composed here, then copied to `frame`. */
static struct vx_surface wallpaper; /* Drawn once. */
static struct window *stack[MAX_WINDOWS]; /* Bottom to top. */
static int window_count, next_window_id = 1;
static struct window *focused;
static struct client clients[MAX_CLIENTS];
static int listener, keyboard = -1, mouse = -1;
static int children[MAX_CHILDREN];

static int pointer_x, pointer_y, buttons;
static struct rect damage; /* What must be drawn again (width 0: nothing). */
static bool quit;

/* What the left button is doing. */
static enum { IDLE, MOVING, RESIZING } drag;
static struct window *dragged;
static int drag_dx, drag_dy;
static bool resize_right, resize_bottom;
static struct rect outline; /* The size a window is being resized to (content). */
static long last_click_ms;
static struct window *last_click_window;

static bool menu_open;

/* Notifications: up to three at a time, each for a few seconds. */
#define MAX_NOTES 3
#define NOTE_MS 4000
#define NOTE_WIDTH 320
#define NOTE_HEIGHT 44
static struct note {
    char text[104];
    long until; /* vx_uptime() */
} notes[MAX_NOTES];
static int note_count;

/* Dragging a window to an edge snaps it there (see snap_target). */
enum snap { SNAP_NONE, SNAP_TOP, SNAP_LEFT, SNAP_RIGHT };
static enum snap snap;
static int menu_hot = -1;
static long shown_minute = -1;

static bool shift, ctrl, alt, caps_lock;

/* ---- Rectangles and damage ---- */

static struct rect frame_rect(const struct window *w) {
    if (w->popup || w->undecorated) {
        return (struct rect){w->x, w->y, w->content.width, w->content.height};
    }
    return (struct rect){w->x - BORDER, w->y - TITLE_HEIGHT - BORDER,
                         w->content.width + 2 * BORDER,
                         w->content.height + TITLE_HEIGHT + 2 * BORDER};
}

/* Where clicks reach the window: its frame, and for a resizable one a little
 * past its right and bottom edges. */
static struct rect hit_rect(const struct window *w) {
    struct rect r = frame_rect(w);
    if (w->resizable && !w->maximized && !w->popup && !w->undecorated) {
        r.width += GRIP;
        r.height += GRIP;
    }
    return r;
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

static struct rect panel_rect(void) {
    return (struct rect){0, 0, screen.width, PANEL_HEIGHT};
}

/* The space windows get when maximized. */
static struct rect work_area(void) {
    return (struct rect){BORDER, PANEL_HEIGHT + TITLE_HEIGHT + BORDER,
                         screen.width - 2 * BORDER,
                         screen.height - PANEL_HEIGHT - TITLE_HEIGHT - 2 * BORDER};
}

/* ---- The menu ---- */

enum menu_action {
    RUN_APP,       /* An app from /apps (see <vexa/app.h>). */
    RUN_LINUX_APP, /* An X program from /linux/usr/share/applications, through xrun. */
    SEPARATOR, LEAVE
};

/* The apps in /apps, and their icons. */
#define MAX_APPS 32
static struct vx_app apps[MAX_APPS];
static struct vx_image *app_icons[MAX_APPS];
static int app_count;

#define MAX_MENU_ITEMS 24
#define MAX_ARGS 4
#define APPLICATIONS "/linux/usr/share/applications"

static struct menu_item {
    char label[48];
    const char *keys;
    enum menu_action action;
    int app; /* RUN_APP: which. */
    char args[MAX_ARGS][64]; /* RUN_LINUX_APP: the program and its arguments. */
    int arg_count;
} menu_items[MAX_MENU_ITEMS];
static int menu_item_count;
#define MENU_ITEMS menu_item_count
#define MENU_WIDTH 300
#define MENU_ITEM_HEIGHT 24
#define MENU_SEPARATOR_HEIGHT 9
#define MENU_BUTTON_WIDTH 72

static int menu_item_height(int i) {
    return menu_items[i].action == SEPARATOR ? MENU_SEPARATOR_HEIGHT : MENU_ITEM_HEIGHT;
}

static struct rect menu_rect(void) {
    int height = 8;
    for (int i = 0; i < MENU_ITEMS; i++) {
        height += menu_item_height(i);
    }
    return (struct rect){4, PANEL_HEIGHT, MENU_WIDTH, height};
}

/* The menu item at a point, or -1. */
static int menu_item_at(int x, int y) {
    struct rect r = menu_rect();
    if (!menu_open || !inside(r, x, y)) {
        return -1;
    }
    int top = r.y + 4;
    for (int i = 0; i < MENU_ITEMS; i++) {
        int h = menu_item_height(i);
        if (y >= top && y < top + h) {
            return menu_items[i].action == SEPARATOR ? -1 : i;
        }
        top += h;
    }
    return -1;
}

static void set_menu(bool open) {
    if (menu_open != open) {
        menu_open = open;
        menu_hot = -1;
        struct rect r = menu_rect();
        add_damage(r);
        add_damage(panel_rect());
    }
}

static struct menu_item *add_menu_item(const char *label, const char *keys,
                                       enum menu_action action) {
    if (menu_item_count == MAX_MENU_ITEMS) {
        return NULL;
    }
    struct menu_item *item = &menu_items[menu_item_count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->keys = keys;
    item->action = action;
    return item;
}

/* A Linux program's .desktop file: its name and command, unless it's
 * hidden or runs in a terminal. */
static void add_linux_app(const char *path) {
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    char text[4096];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    char name[40] = "", exec[256] = "";
    bool in_entry = false, hidden = false;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (line[0] == '[') {
            in_entry = !strncmp(line, "[Desktop Entry]", 15);
        } else if (in_entry && !strncmp(line, "Name=", 5)) {
            strncpy(name, line + 5, sizeof(name) - 1);
        } else if (in_entry && !strncmp(line, "Exec=", 5)) {
            strncpy(exec, line + 5, sizeof(exec) - 1);
        } else if (in_entry && (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Terminal=true") ||
                                !strcmp(line, "Hidden=true"))) {
            hidden = true;
        }
    }
    if (hidden || !name[0] || !exec[0]) {
        return;
    }
    struct menu_item *item = add_menu_item(name, "", RUN_LINUX_APP);
    if (!item) {
        return;
    }
    /* The command's words, without field codes like %U. */
    for (char *word = exec, *next; word && *word && item->arg_count < MAX_ARGS; word = next) {
        next = strchr(word, ' ');
        if (next) {
            *next++ = '\0';
        }
        if (word[0] && word[0] != '%') {
            strncpy(item->args[item->arg_count++], word, sizeof(item->args[0]) - 1);
        }
    }
    if (!item->arg_count) {
        menu_item_count--;
    }
}

static int compare_labels(const void *a, const void *b) {
    return strcmp(((const struct menu_item *)a)->label, ((const struct menu_item *)b)->label);
}

static void load_apps(void) {
    for (int i = 0; i < app_count; i++) {
        vx_image_free(app_icons[i]);
    }
    app_count = vx_app_list(apps, MAX_APPS);
    for (int i = 0; i < app_count; i++) {
        app_icons[i] = apps[i].icon[0] ? vx_image_load(apps[i].icon, VX_IMAGE_ALPHA) : NULL;
    }
}

/* The apps in the menu: Vexa's, or the Linux ones. */
static void add_app_items(bool is_linux) {
    for (int i = 0; i < app_count; i++) {
        if (apps[i].menu && apps[i].is_linux == is_linux) {
            struct menu_item *item = add_menu_item(apps[i].name, apps[i].shortcut, RUN_APP);
            if (item) {
                item->app = i;
            }
        }
    }
}

static void build_menu(void) {
    load_apps();
    menu_item_count = 0;
    add_app_items(false);
    add_menu_item("", "", SEPARATOR);
    add_app_items(true);
    /* Linux programs, by name. */
    int first = menu_item_count;
    int handle = vx_open(APPLICATIONS, VX_OPEN_READ);
    if (handle >= 0) {
        struct vx_dir_entry entries[16];
        long n;
        while ((n = vx_read_dir(handle, entries, 16)) > 0) {
            for (long i = 0; i < n; i++) {
                size_t length = strlen(entries[i].name);
                if (length > 8 && !strcmp(entries[i].name + length - 8, ".desktop") &&
                    menu_item_count < MAX_MENU_ITEMS - 2) {
                    char path[320];
                    snprintf(path, sizeof(path), "%s/%s", APPLICATIONS, entries[i].name);
                    add_linux_app(path);
                }
            }
        }
        vx_close(handle);
    }
    qsort(menu_items + first, (size_t)(menu_item_count - first), sizeof(menu_items[0]),
          compare_labels);
    add_menu_item("", "", SEPARATOR);
    add_menu_item("Back to the console", "Ctrl+Alt+Q", LEAVE);
}

/* ---- The panel's window buttons ---- */

/* Windows in the order they were opened (their buttons' order). */
static int windows_by_id(struct window **out) {
    int n = 0;
    for (int i = 0; i < window_count; i++) {
        if (!stack[i]->popup) {
            out[n++] = stack[i];
        }
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && out[j - 1]->id > out[j]->id; j--) {
            struct window *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    }
    return n;
}

#define CLOCK_WIDTH (21 * FONT_WIDTH)

static struct rect task_button(int index, int count) {
    int left = MENU_BUTTON_WIDTH + 12;
    int room = screen.width - left - CLOCK_WIDTH - 24;
    int width = count ? room / count - 4 : 0;
    if (width > 180) {
        width = 180;
    }
    return (struct rect){left + index * (width + 4), 3, width, PANEL_HEIGHT - 6};
}

/* ---- Drawing ---- */

#define CURSOR_WIDTH 12
#define CURSOR_HEIGHT 19

/* An arrow, and the one for resizing: '#' outline, '.' fill. */
static const char *const cursor_shape[CURSOR_HEIGHT] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
    "#.#  #..#   ", "##    #..#  ", "#     #..#  ", "       ##   ",
};
static const char *const resize_shape[CURSOR_HEIGHT] = {
    "######      ", "#....#      ", "#...#       ", "#....#      ", "#.#..#      ",
    "## #..#     ", "    #..#    ", "     #..# ##", "      #..#.#", "       #....#",
    "       #...#", "      #....#", "      ######", "            ", "            ",
    "            ", "            ", "            ", "            ",
};

static bool resize_cursor;

static struct rect cursor_rect(void) {
    return (struct rect){pointer_x, pointer_y, CURSOR_WIDTH + 1, CURSOR_HEIGHT};
}

/* Text cut to fit `width` pixels, with "..." when it doesn't. */
static void draw_text_fit(struct vx_surface *s, int x, int y, int width, const char *text,
                          uint32_t color) {
    int fits = width / FONT_WIDTH;
    int length = (int)strlen(text);
    if (fits <= 0) {
        return;
    }
    char line[128];
    if (length <= fits) {
        vx_draw_text(s, x, y, text, color, VX_TRANSPARENT);
        return;
    }
    if (fits > (int)sizeof(line) - 1) {
        fits = sizeof(line) - 1;
    }
    int keep = fits > 3 ? fits - 3 : fits;
    memcpy(line, text, (size_t)keep);
    strcpy(line + keep, fits > 3 ? "..." : "");
    vx_draw_text(s, x, y, line, color, VX_TRANSPARENT);
}

/* Draws a character `scale` times its size. */
static void draw_big_char(struct vx_surface *s, int x, int y, char c, int scale, uint32_t color) {
    if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_GLYPH_COUNT) {
        return;
    }
    const uint8_t *rows = font_glyphs[c - FONT_FIRST_CHAR];
    for (int r = 0; r < FONT_HEIGHT; r++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (rows[r] & (0x80 >> col)) {
                vx_fill(s, x + col * scale, y + r * scale, scale, scale, color);
            }
        }
    }
}

static uint32_t mix(uint32_t a, uint32_t b, int num, int den) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
        out |= (uint32_t)(ca + (cb - ca) * num / den) << shift;
    }
    return out;
}

/* ---- Settings (DESKTOP_CONFIG) ---- */

static char setting_wallpaper[32] = "image";
static char setting_image[256] = DESKTOP_DEFAULT_WALLPAPER;
static int setting_clock = 24;     /* Hours. */
static int setting_utc_offset;     /* Minutes east of UTC. */

static void read_config(void) {
    strcpy(setting_wallpaper, "image");
    strcpy(setting_image, DESKTOP_DEFAULT_WALLPAPER);
    setting_clock = 24;
    setting_utc_offset = 0;
    int handle = vx_open(DESKTOP_CONFIG, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    char text[2048];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *value = strchr(line, '=');
        if (!value) {
            continue;
        }
        *value++ = '\0';
        if (!strcmp(line, "wallpaper")) {
            strncpy(setting_wallpaper, value, sizeof(setting_wallpaper) - 1);
        } else if (!strcmp(line, "wallpaper_image")) {
            strncpy(setting_image, value, sizeof(setting_image) - 1);
        } else if (!strcmp(line, "clock")) {
            setting_clock = atoi(value) == 12 ? 12 : 24;
        } else if (!strcmp(line, "utc_offset")) {
            setting_utc_offset = atoi(value);
        }
    }
}

static void make_wallpaper(void) {
    /* An image, scaled to cover the screen... */
    struct vx_image *image = !strcmp(setting_wallpaper, "image") && setting_image[0]
                                 ? vx_image_load(setting_image, 0)
                                 : NULL;
    if (image) {
        int iw = image->surface.width, ih = image->surface.height;
        int w = wallpaper.width, h = iw ? ih * wallpaper.width / iw : wallpaper.height;
        if (h < wallpaper.height) {
            h = wallpaper.height;
            w = iw * wallpaper.height / ih;
        }
        vx_blit_scaled(&wallpaper, (wallpaper.width - w) / 2, (wallpaper.height - h) / 2, w, h,
                       &image->surface);
        vx_image_free(image);
        return;
    }
    /* ... or a gradient. */
    const struct desktop_wallpaper *choice = &desktop_wallpapers[0];
    for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
        if (!strcmp(desktop_wallpapers[i].name, setting_wallpaper)) {
            choice = &desktop_wallpapers[i];
        }
    }
    for (int y = 0; y < wallpaper.height; y++) {
        uint32_t color = mix(choice->top, choice->bottom, y, wallpaper.height);
        uint32_t *row = wallpaper.pixels + (long)y * wallpaper.stride;
        for (int x = 0; x < wallpaper.width; x++) {
            row[x] = color;
        }
    }
    /* The name, large, in the bottom right corner (a little lighter than the
     * gradient's top). */
    uint32_t text_color = mix(choice->top, 0xffffff, 1, 8);
    int scale = wallpaper.width >= 1024 ? 8 : 4;
    const char *name = "Vexa";
    int width = (int)strlen(name) * FONT_WIDTH * scale;
    int x = wallpaper.width - width - 48, y = wallpaper.height - FONT_HEIGHT * scale - 40;
    for (int i = 0; name[i]; i++) {
        draw_big_char(&wallpaper, x + i * FONT_WIDTH * scale, y, name[i], scale,
                      text_color);
    }
    char version[64];
    struct vx_system_info info;
    snprintf(version, sizeof(version), "version %s",
             vx_system_info(&info) == 0 ? info.version : "?");
    vx_draw_text(&wallpaper, x + 4, y + FONT_HEIGHT * scale + 4, version, text_color,
                 VX_TRANSPARENT);
}

static void draw_title_bar(struct vx_surface *view, int ox, int oy, struct window *w) {
    int x = ox + w->x, y = oy + w->y - TITLE_HEIGHT, width = w->content.width;
    vx_fill(view, x, y, width, TITLE_HEIGHT, w == focused ? COLOR_TITLE_FOCUSED : COLOR_TITLE);
    int button_count = w->resizable ? 3 : 2;
    draw_text_fit(view, x + 8, y + 3, width - 16 - button_count * BUTTON_WIDTH, w->title,
                  COLOR_TITLE_TEXT);
    /* Close: an x at the right end; then maximize (a box) and minimize (a bar). */
    int bx = x + width - BUTTON_WIDTH;
    vx_draw_char(view, bx + 4, y + 3, 'x', COLOR_CLOSE, VX_TRANSPARENT);
    if (w->resizable) {
        bx -= BUTTON_WIDTH;
        int size = w->maximized ? 7 : 9;
        int bxx = bx + 5, byy = y + 6;
        vx_fill(view, bxx, byy, size, 2, COLOR_TITLE_TEXT);
        vx_fill(view, bxx, byy + size - 1, size, 1, COLOR_TITLE_TEXT);
        vx_fill(view, bxx, byy, 1, size, COLOR_TITLE_TEXT);
        vx_fill(view, bxx + size - 1, byy, 1, size, COLOR_TITLE_TEXT);
        if (w->maximized) { /* Two boxes: "restore". */
            vx_fill(view, bxx + 2, byy - 2, size, 1, COLOR_TITLE_TEXT);
            vx_fill(view, bxx + size + 1, byy - 2, 1, size, COLOR_TITLE_TEXT);
        }
    }
    bx -= BUTTON_WIDTH;
    vx_fill(view, bx + 5, y + 14, 9, 2, COLOR_TITLE_TEXT);
}

static void draw_panel(struct vx_surface *view, int ox, int oy) {
    vx_fill(view, ox, oy, screen.width, PANEL_HEIGHT - 1, COLOR_PANEL);
    vx_fill(view, ox, oy + PANEL_HEIGHT - 1, screen.width, 1, COLOR_PANEL_LINE);
    /* The Vexa menu's button: a diamond and the name. */
    vx_fill(view, ox + 3, oy + 3, MENU_BUTTON_WIDTH, PANEL_HEIGHT - 6,
            menu_open ? COLOR_BUTTON_HOT : COLOR_BUTTON);
    for (int i = 0; i < 5; i++) {
        vx_fill(view, ox + 14 - i, oy + 8 + i, 2 * i + 1, 1, COLOR_OUTLINE);
        vx_fill(view, ox + 14 - i, oy + 16 - i, 2 * i + 1, 1, COLOR_OUTLINE);
    }
    vx_draw_text(view, ox + 26, oy + 5, "Vexa", COLOR_PANEL_TEXT, VX_TRANSPARENT);

    struct window *list[MAX_WINDOWS];
    int n = windows_by_id(list);
    for (int i = 0; i < n; i++) {
        struct rect b = task_button(i, n);
        struct window *w = list[i];
        uint32_t color = w->minimized ? COLOR_BUTTON_OFF
                         : w == focused ? COLOR_BUTTON_HOT : COLOR_BUTTON;
        vx_fill(view, ox + b.x, oy + b.y, b.width, b.height, color);
        draw_text_fit(view, ox + b.x + 8, oy + b.y + 2, b.width - 12, w->title,
                      w->minimized ? COLOR_PANEL_DIM : COLOR_PANEL_TEXT);
    }

    /* The clock, in the time zone from the settings. */
    long now = vx_time();
    if (now > 0) {
        now += setting_utc_offset * 60L;
        static const char *const days[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
        static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        long day_number = now / 86400, seconds = now % 86400;
        /* Civil date from days since 1970 (Howard Hinnant's algorithm). */
        long z = day_number + 719468, era = z / 146097, doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        long mday = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
        char text[32];
        long hour = seconds / 3600;
        if (setting_clock == 12) {
            snprintf(text, sizeof(text), "%s %ld %s  %ld:%02ld %s", days[day_number % 7], mday,
                     months[month - 1], hour % 12 ? hour % 12 : 12, seconds / 60 % 60,
                     hour < 12 ? "am" : "pm");
        } else {
            snprintf(text, sizeof(text), "%s %ld %s  %02ld:%02ld", days[day_number % 7], mday,
                     months[month - 1], hour, seconds / 60 % 60);
        }
        int width = (int)strlen(text) * FONT_WIDTH;
        vx_draw_text(view, ox + screen.width - width - 12, oy + 5, text, COLOR_PANEL_TEXT,
                     VX_TRANSPARENT);
    }
}

static void draw_menu(struct vx_surface *view, int ox, int oy) {
    struct rect r = menu_rect();
    vx_fill(view, ox + r.x, oy + r.y, r.width, r.height, COLOR_PANEL_LINE);
    vx_fill(view, ox + r.x + 1, oy + r.y, r.width - 2, r.height - 1, COLOR_MENU);
    int top = r.y + 4;
    for (int i = 0; i < MENU_ITEMS; i++) {
        int h = menu_item_height(i);
        if (menu_items[i].action == SEPARATOR) {
            vx_fill(view, ox + r.x + 8, oy + top + h / 2, r.width - 16, 1, COLOR_PANEL_LINE);
        } else {
            if (i == menu_hot) {
                vx_fill(view, ox + r.x + 4, oy + top, r.width - 8, h, COLOR_MENU_HOT);
            }
            int text_x = ox + r.x + 12;
            if (menu_items[i].action == RUN_APP && app_icons[menu_items[i].app]) {
                vx_blit_alpha(view, text_x, oy + top + 4, 16, 16, &app_icons[menu_items[i].app]->surface);
            }
            if (menu_items[i].action == RUN_APP || menu_items[i].action == RUN_LINUX_APP) {
                text_x += 24;
            }
            vx_draw_text(view, text_x, oy + top + 4, menu_items[i].label, COLOR_PANEL_TEXT,
                         VX_TRANSPARENT);
            int keys = (int)strlen(menu_items[i].keys) * FONT_WIDTH;
            vx_draw_text(view, ox + r.x + r.width - keys - 12, oy + top + 4, menu_items[i].keys,
                         COLOR_PANEL_DIM, VX_TRANSPARENT);
        }
        top += h;
    }
}

/* A rectangle's outline, `thickness` pixels wide, inside it. */
static void draw_outline(struct vx_surface *view, struct rect r, int thickness, uint32_t color) {
    vx_fill(view, r.x, r.y, r.width, thickness, color);
    vx_fill(view, r.x, r.y + r.height - thickness, r.width, thickness, color);
    vx_fill(view, r.x, r.y, thickness, r.height, color);
    vx_fill(view, r.x + r.width - thickness, r.y, thickness, r.height, color);
}

/* ---- Desktop icons ---- */

/* The apps with desktop=yes, in the menu's order. */
static int launcher_apps[MAX_APPS];
static int launcher_count;
#define LAUNCHERS launcher_count
#define LAUNCHER_WIDTH 72
#define LAUNCHER_HEIGHT 72
static int selected_launcher = -1;

static void find_launchers(void) {
    launcher_count = 0;
    for (int i = 0; i < app_count; i++) {
        if (apps[i].desktop) {
            launcher_apps[launcher_count++] = i;
        }
    }
}

/* An icon's label: the bundle's name, as a file manager shows it
 * ("Editor" for Editor.vxapp). */
static void launcher_label(int i, char *out, size_t size) {
    const char *bundle = apps[launcher_apps[i]].bundle, *slash = strrchr(bundle, '/');
    snprintf(out, size, "%s", slash ? slash + 1 : bundle);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

/* A column along the left edge, left of where windows open. */
static struct rect launcher_rect(int i) {
    return (struct rect){4, PANEL_HEIGHT + 12 + i * (LAUNCHER_HEIGHT + 4), LAUNCHER_WIDTH,
                         LAUNCHER_HEIGHT};
}

static struct rect launchers_area(void) {
    return (struct rect){0, PANEL_HEIGHT, LAUNCHER_WIDTH + 8,
                         12 + LAUNCHERS * (LAUNCHER_HEIGHT + 4)};
}

static void draw_launchers(struct vx_surface *view, int ox, int oy) {
    for (int i = 0; i < LAUNCHERS; i++) {
        struct rect r = launcher_rect(i);
        if (i == selected_launcher) {
            vx_fill(view, ox + r.x, oy + r.y, r.width, r.height, COLOR_BUTTON_HOT);
        }
        struct vx_image *icon = app_icons[launcher_apps[i]];
        int icon_x = ox + r.x + (r.width - 48) / 2, icon_y = oy + r.y + 3;
        if (icon) {
            vx_blit_alpha(view, icon_x, icon_y, 48, 48, &icon->surface);
        } else {
            vx_fill(view, icon_x + 4, icon_y + 4, 40, 40, COLOR_PANEL_LINE);
        }
        char label[64];
        launcher_label(i, label, sizeof(label));
        int text = (int)strlen(label) * FONT_WIDTH;
        /* A shadow keeps the label readable on a picture. */
        vx_draw_text(view, ox + r.x + (r.width - text) / 2 + 1, oy + r.y + 54, label, 0x000000,
                     VX_TRANSPARENT);
        vx_draw_text(view, ox + r.x + (r.width - text) / 2, oy + r.y + 53, label,
                     COLOR_PANEL_TEXT, VX_TRANSPARENT);
    }
}

/* ---- Notifications ---- */

static struct rect note_rect(int i) {
    return (struct rect){screen.width - NOTE_WIDTH - 12, PANEL_HEIGHT + 12 + i * (NOTE_HEIGHT + 8),
                         NOTE_WIDTH, NOTE_HEIGHT};
}

static struct rect notes_area(void) {
    return (struct rect){screen.width - NOTE_WIDTH - 12, PANEL_HEIGHT + 12, NOTE_WIDTH,
                         MAX_NOTES * (NOTE_HEIGHT + 8)};
}

static void add_note(const char *text) {
    if (note_count == MAX_NOTES) {
        memmove(notes, notes + 1, (MAX_NOTES - 1) * sizeof(notes[0]));
        note_count--;
    }
    struct note *note = &notes[note_count++];
    strncpy(note->text, text, sizeof(note->text) - 1);
    note->text[sizeof(note->text) - 1] = '\0';
    note->until = vx_uptime() + NOTE_MS;
    add_damage(notes_area());
    printf("desktop: notification \"%s\"\n", note->text);
}

/* Takes away old notifications; returns how long until the next one goes
 * (milliseconds), or -1 if there are none. */
static long expire_notes(void) {
    long now = vx_uptime(), next = -1;
    int kept = 0;
    for (int i = 0; i < note_count; i++) {
        if (notes[i].until > now) {
            notes[kept++] = notes[i];
            if (next < 0 || notes[i].until - now < next) {
                next = notes[i].until - now;
            }
        }
    }
    if (kept != note_count) {
        note_count = kept;
        add_damage(notes_area());
    }
    return next;
}

static void draw_notes(struct vx_surface *view, int ox, int oy) {
    int per_line = (NOTE_WIDTH - 20) / FONT_WIDTH;
    for (int i = 0; i < note_count; i++) {
        struct rect r = note_rect(i);
        vx_fill(view, ox + r.x, oy + r.y, r.width, r.height, COLOR_MENU);
        vx_draw_outline(view, ox + r.x, oy + r.y, r.width, r.height, COLOR_OUTLINE);
        /* Two lines: "Title: text" puts the title on the first. */
        char first[128], second[128] = "";
        const char *colon = strstr(notes[i].text, ": ");
        size_t length = strlen(notes[i].text);
        if (colon && colon - notes[i].text < per_line) {
            size_t n = (size_t)(colon - notes[i].text);
            memcpy(first, notes[i].text, n);
            first[n] = '\0';
            snprintf(second, sizeof(second), "%s", colon + 2);
        } else if (length > (size_t)per_line) {
            memcpy(first, notes[i].text, (size_t)per_line);
            first[per_line] = '\0';
            snprintf(second, sizeof(second), "%s", notes[i].text + per_line);
        } else {
            snprintf(first, sizeof(first), "%s", notes[i].text);
        }
        vx_draw_text_fit(view, ox + r.x + 10, oy + r.y + 5, r.width - 20, first, COLOR_OUTLINE,
                         VX_TRANSPARENT);
        vx_draw_text_fit(view, ox + r.x + 10, oy + r.y + 23, r.width - 20, second, COLOR_PANEL_TEXT,
                         VX_TRANSPARENT);
    }
}

/* ---- Snapping ---- */

/* Where a window dragged to here would go. */
static enum snap snap_target(void) {
    if (!dragged || !dragged->resizable) {
        return SNAP_NONE;
    }
    return pointer_y <= PANEL_HEIGHT + 1 ? SNAP_TOP
           : pointer_x <= 1                ? SNAP_LEFT
           : pointer_x >= screen.width - 2 ? SNAP_RIGHT
                                           : SNAP_NONE;
}

/* The content rectangle a snapped window gets. */
static struct rect snap_rect(enum snap where) {
    struct rect area = work_area();
    if (where == SNAP_LEFT || where == SNAP_RIGHT) {
        int half = screen.width / 2;
        area.width = half - 2 * BORDER;
        area.x = where == SNAP_LEFT ? BORDER : half + BORDER;
    }
    return area;
}

static struct rect snap_damage(enum snap where) {
    struct rect r = snap_rect(where);
    return (struct rect){r.x - BORDER - 2, r.y - TITLE_HEIGHT - BORDER - 2,
                         r.width + 2 * BORDER + 4, r.height + TITLE_HEIGHT + 2 * BORDER + 4};
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
    vx_blit(&view, 0, 0, &wallpaper, area.x, area.y, area.width, area.height);
    draw_launchers(&view, ox, oy);
    for (int i = 0; i < window_count; i++) {
        struct window *w = stack[i];
        if (w->minimized) {
            continue;
        }
        if (!w->popup && !w->undecorated) {
            struct rect f = frame_rect(w);
            vx_fill(&view, ox + f.x, oy + f.y, f.width, f.height, COLOR_BORDER);
            draw_title_bar(&view, ox, oy, w);
        }
        vx_blit(&view, ox + w->x, oy + w->y, &w->content, 0, 0, w->content.width,
                w->content.height);
    }
    if (drag == RESIZING) {
        struct rect r = {ox + outline.x - BORDER, oy + outline.y - TITLE_HEIGHT - BORDER,
                         outline.width + 2 * BORDER, outline.height + TITLE_HEIGHT + 2 * BORDER};
        draw_outline(&view, r, 2, COLOR_OUTLINE);
    }
    if (drag == MOVING && snap != SNAP_NONE) {
        struct rect s = snap_damage(snap);
        s.x += ox + 2, s.y += oy + 2, s.width -= 4, s.height -= 4;
        draw_outline(&view, s, 2, COLOR_OUTLINE);
    }
    draw_notes(&view, ox, oy);
    if (area.y < PANEL_HEIGHT) {
        draw_panel(&view, ox, oy);
    }
    if (menu_open) {
        draw_menu(&view, ox, oy);
    }
    const char *const *shape = resize_cursor ? resize_shape : cursor_shape;
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; shape[row][col]; col++) {
            char c = shape[row][col];
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
    add_damage(panel_rect());
}

/* The top window that isn't minimized, or NULL. */
static struct window *top_window(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (!stack[i]->minimized && !stack[i]->popup) {
            return stack[i];
        }
    }
    return NULL;
}

/* Puts the window on top of the others (of its kind: popups stay above). */
static void raise_window(struct window *w) {
    int at = 0;
    while (at < window_count && stack[at] != w) {
        at++;
    }
    if (at == window_count) {
        return;
    }
    memmove(stack + at, stack + at + 1, (size_t)(window_count - at - 1) * sizeof(stack[0]));
    int to = window_count - 1;
    while (!w->popup && to > 0 && stack[to - 1]->popup) {
        to--;
    }
    memmove(stack + to + 1, stack + to, (size_t)(window_count - 1 - to) * sizeof(stack[0]));
    stack[to] = w;
    add_damage(frame_rect(w));
}

/* Tells the program whether its window is maximized or minimized. */
static void send_state(struct window *w) {
    struct desktop_message m = {.type = DESKTOP_STATE, .window = (uint32_t)w->id,
                                .a = w->maximized, .b = w->minimized};
    send_to(w->client, &m);
}

/* Tells the program where its window is now. */
static void send_moved(struct window *w) {
    struct desktop_message m = {.type = DESKTOP_MOVED, .window = (uint32_t)w->id,
                                .a = w->x, .b = w->y};
    send_to(w->client, &m);
}

static void activate(struct window *w) {
    if (w->minimized) {
        w->minimized = false;
        send_state(w);
        add_damage(frame_rect(w));
        add_damage(panel_rect());
    }
    raise_window(w);
    if (!w->popup) {
        set_focus(w);
    }
}

static void minimize(struct window *w) {
    if (w->minimized) {
        return;
    }
    w->minimized = true;
    send_state(w);
    add_damage(frame_rect(w));
    add_damage(panel_rect());
    if (focused == w) {
        set_focus(top_window());
    }
}

/* Asks the window's program for a new size (it answers with a new buffer). */
static void configure(struct window *w, int width, int height) {
    struct desktop_message m = {.type = DESKTOP_CONFIGURE, .window = (uint32_t)w->id,
                                .a = width, .b = height};
    send_to(w->client, &m);
}

static void send_state(struct window *w);

static void toggle_maximized(struct window *w) {
    if (!w->resizable) {
        return;
    }
    add_damage(frame_rect(w));
    if (w->maximized) {
        w->maximized = false;
        w->x = w->restore.x;
        w->y = w->restore.y;
        configure(w, w->restore.width, w->restore.height);
    } else {
        w->maximized = true;
        w->restore = (struct rect){w->x, w->y, w->content.width, w->content.height};
        struct rect area = work_area();
        if (w->undecorated) { /* Its own title bar goes where ours would. */
            area.y -= TITLE_HEIGHT;
            area.height += TITLE_HEIGHT;
        }
        w->x = area.x;
        w->y = area.y;
        configure(w, area.width, area.height);
    }
    add_damage(frame_rect(w));
    send_moved(w);
    send_state(w);
    printf("desktop: %s window %d\n", w->maximized ? "maximized" : "restored", w->id);
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
    if (y < PANEL_HEIGHT) {
        return NULL;
    }
    for (int i = window_count - 1; i >= 0; i--) {
        if (!stack[i]->minimized && inside(hit_rect(stack[i]), x, y)) {
            return stack[i];
        }
    }
    return NULL;
}

static void destroy_window(struct window *w) {
    add_damage(frame_rect(w));
    add_damage(panel_rect());
    int at = 0;
    while (stack[at] != w) {
        at++;
    }
    memmove(stack + at, stack + at + 1, (size_t)(window_count - at - 1) * sizeof(stack[0]));
    window_count--;
    if (dragged == w) {
        dragged = NULL;
        if (drag == RESIZING) {
            add_damage((struct rect){outline.x - 8, outline.y - TITLE_HEIGHT - 8,
                                     outline.width + 16, outline.height + TITLE_HEIGHT + 16});
        }
        drag = IDLE;
    }
    if (last_click_window == w) {
        last_click_window = NULL;
    }
    if (focused == w) {
        focused = NULL;
        set_focus(top_window());
    }
    vx_unmap(w->content.pixels, w->mapped_size);
    vx_close(w->buffer_handle);
    printf("desktop: closed window %d \"%s\"\n", w->id, w->title);
    free(w);
}

/* Maps a program's buffer file. False if it isn't one. */
static bool map_buffer(const char *path, int width, int height, int *handle, void **pixels,
                       size_t *size) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        strncmp(path, "/run/shm/", 9) != 0) {
        return false;
    }
    *size = ((size_t)width * height * 4 + 4095) & ~(size_t)4095;
    *handle = vx_open(path, VX_OPEN_READ);
    if (*handle < 0) {
        return false;
    }
    *pixels = vx_map_file(*handle, 0, *size, 0);
    if (!*pixels) {
        vx_close(*handle);
        return false;
    }
    return true;
}

static void create_window(int client, struct desktop_message *m) {
    struct desktop_message reply = {.type = DESKTOP_CREATED};
    m->text[sizeof(m->text) - 1] = '\0';
    const char *path = m->text;
    size_t path_length = strlen(path);
    const char *title = path_length + 1 < sizeof(m->text) ? path + path_length + 1 : "";
    int width = m->a, height = m->b;
    struct window *w = window_count < MAX_WINDOWS ? calloc(1, sizeof(*w)) : NULL;
    void *pixels;
    if (w && !map_buffer(path, width, height, &w->buffer_handle, &pixels, &w->mapped_size)) {
        free(w);
        w = NULL;
    }
    if (w) {
        w->content = (struct vx_surface){pixels, width, height, width};
        w->id = next_window_id++;
        w->client = client;
        w->resizable = m->c & DESKTOP_RESIZABLE;
        w->popup = m->c & DESKTOP_POPUP;
        w->undecorated = !w->popup && (m->c & DESKTOP_UNDECORATED);
        strncpy(w->title, title, sizeof(w->title) - 1);
        /* Cascade new windows from the top left (popups go where they're told). */
        int n = (w->id - 1) % 8;
        w->x = 80 + 32 * n;
        w->y = 60 + TITLE_HEIGHT + 28 * n;
        if (w->x + width > screen.width) {
            w->x = screen.width > width ? (screen.width - width) / 2 : BORDER;
        }
        if (w->y + height > screen.height) {
            w->y = PANEL_HEIGHT + TITLE_HEIGHT + BORDER;
        }
        stack[window_count++] = w;
        raise_window(w);
        add_damage(frame_rect(w));
        add_damage(panel_rect());
        if (!w->popup) {
            set_focus(w);
        }
        reply.window = (uint32_t)w->id;
        printf("desktop: window %d \"%s\" (%dx%d) at %d,%d\n", w->id, w->title, width, height,
               w->x, w->y);
    }
    send_to(client, &reply);
    if (w) {
        send_moved(w);
    }
}

/* A program's new buffer, at a new size. */
static void replace_buffer(int client, struct window *w, struct desktop_message *m) {
    struct desktop_message reply = {.type = DESKTOP_RESIZED, .window = m->window};
    m->text[sizeof(m->text) - 1] = '\0';
    int handle;
    void *pixels;
    size_t size;
    if (w && map_buffer(m->text, m->a, m->b, &handle, &pixels, &size)) {
        add_damage(frame_rect(w));
        vx_unmap(w->content.pixels, w->mapped_size);
        vx_close(w->buffer_handle);
        w->content = (struct vx_surface){pixels, m->a, m->b, m->a};
        w->buffer_handle = handle;
        w->mapped_size = size;
        add_damage(frame_rect(w));
        reply.a = m->a;
        reply.b = m->b;
        printf("desktop: window %d is now %dx%d\n", w->id, m->a, m->b);
    }
    send_to(client, &reply);
}

static struct rect outline_damage(void);

/* Starts dragging a window by the pointer. A maximized one first goes back
 * to its size from before, under the pointer. */
static void start_move(struct window *w) {
    if (w->maximized) {
        int offset_y = pointer_y - w->y;
        toggle_maximized(w);
        add_damage(frame_rect(w));
        w->x = pointer_x - w->restore.width / 2;
        w->y = pointer_y - offset_y;
        add_damage(frame_rect(w));
    }
    drag = MOVING;
    dragged = w;
    drag_dx = pointer_x - w->x;
    drag_dy = pointer_y - w->y;
}

/* What a program asks of its window, as of a window manager. */
static void window_request(struct window *w, int request, int argument) {
    switch (request) {
    case DESKTOP_WM_MAXIMIZE:
    case DESKTOP_WM_RESTORE:
    case DESKTOP_WM_TOGGLE_MAXIMIZED:
        if (request == DESKTOP_WM_TOGGLE_MAXIMIZED ||
            (request == DESKTOP_WM_MAXIMIZE) != w->maximized) {
            toggle_maximized(w);
        }
        break;
    case DESKTOP_WM_MINIMIZE:
        minimize(w);
        break;
    case DESKTOP_WM_ACTIVATE:
        activate(w);
        break;
    case DESKTOP_WM_MOVE:
        /* The program saw a press on its own title bar: drag from here, as
         * long as the button is down. */
        if ((buttons & 1) && drag == IDLE) {
            start_move(w);
            printf("desktop: window %d moves itself\n", w->id);
        }
        break;
    case DESKTOP_WM_RESIZE:
        if ((buttons & 1) && drag == IDLE && w->resizable) {
            drag = RESIZING;
            dragged = w;
            resize_right = argument & 1;
            resize_bottom = argument & 2;
            drag_dx = w->x + w->content.width - pointer_x;
            drag_dy = w->y + w->content.height - pointer_y;
            outline = (struct rect){w->x, w->y, w->content.width, w->content.height};
            add_damage(outline_damage());
        }
        break;
    }
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
        if (w && !w->minimized) {
            add_damage((struct rect){w->x + m.a, w->y + m.b, m.c, m.d});
        }
        break;
    case DESKTOP_TITLE:
        if (w) {
            m.text[sizeof(m.text) - 1] = '\0';
            strncpy(w->title, m.text, sizeof(w->title) - 1);
            add_damage(frame_rect(w));
            add_damage(panel_rect());
            printf("desktop: window %d is now called \"%s\"\n", w->id, w->title);
        }
        break;
    case DESKTOP_DESTROY:
        if (w) {
            destroy_window(w);
        }
        break;
    case DESKTOP_BUFFER:
        replace_buffer(client, w, &m);
        break;
    case DESKTOP_MOVE:
        if (w && (w->x != m.a || w->y != m.b)) {
            add_damage(frame_rect(w));
            w->x = m.a;
            w->y = w->popup || m.b >= PANEL_HEIGHT + TITLE_HEIGHT + BORDER
                       ? m.b : PANEL_HEIGHT + TITLE_HEIGHT + BORDER;
            add_damage(frame_rect(w));
            if (w->y != m.b) {
                send_moved(w);
            }
        }
        break;
    case DESKTOP_WM:
        if (w) {
            window_request(w, m.a, m.b);
        }
        break;
    case DESKTOP_NOTIFY:
        m.text[sizeof(m.text) - 1] = '\0';
        add_note(m.text);
        break;
    case DESKTOP_RELOAD:
        read_config();
        make_wallpaper();
        build_menu();
        find_launchers();
        add_damage((struct rect){0, 0, screen.width, screen.height});
        printf("desktop: settings reloaded\n");
        break;
    case DESKTOP_INFO: {
        struct desktop_message reply = {.type = DESKTOP_INFO_REPLY, .a = screen.width,
                                        .b = screen.height};
        send_to(client, &reply);
        break;
    }
    }
}

/* ---- Programs ---- */

static void add_child(int process);

/* Starts a program: argv[0] is its path. */
static void launch_argv(const char *const *argv, int argc) {
    const char *path = argv[0];
    unsigned long envc = 0;
    while (environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {
        .argv = argv, .argc = (unsigned long)argc, .envp = (const char *const *)environ, .envc = envc,
        .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
    };
    int process = vx_spawn(path, &spawn);
    if (process < 0) {
        printf("desktop: can't start %s: %s\n", path, vx_strerror(process));
        return;
    }
    add_child(process);
}

/* A program the desktop started: waited for when it ends. */
static void add_child(int process) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (children[i] < 0) {
            children[i] = process;
            return;
        }
    }
    vx_close(process);
}

static void launch(const char *path) {
    const char *argv[] = {path};
    launch_argv(argv, 1);
}

static void reap_children(void) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (children[i] >= 0 && vx_wait(children[i], VX_WAIT_NO_HANG) != -VX_EAGAIN) {
            vx_close(children[i]);
            children[i] = -1;
        }
    }
}

static void run_app(int i) {
    int process = vx_app_open(&apps[i], NULL);
    if (process < 0) {
        printf("desktop: can't start %s: %s\n", apps[i].name, vx_strerror(process));
        return;
    }
    add_child(process);
}

static void run_menu_item(int index) {
    struct menu_item *item = &menu_items[index];
    if (item->action == LEAVE) {
        quit = true;
        return;
    }
    if (item->action == RUN_APP) {
        run_app(item->app);
        return;
    }
    if (item->action != RUN_LINUX_APP) {
        return;
    }
    const char *argv[MAX_ARGS + 1] = {"/linux/usr/bin/xrun"};
    for (int i = 0; i < item->arg_count; i++) {
        argv[i + 1] = item->args[i];
    }
    launch_argv(argv, item->arg_count + 1);
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

/* Alt+Tab: the window opened after the focused one (round the list). */
static void next_window(void) {
    struct window *list[MAX_WINDOWS];
    int n = windows_by_id(list);
    if (n == 0) {
        return;
    }
    int at = 0;
    while (at < n && list[at] != focused) {
        at++;
    }
    activate(list[at < n ? (at + 1) % n : 0]);
}

static void key_event(int key, int value) {
    bool down = value != 0;
    /* Modifiers change what keys type here, and go to the window too (an X
     * server keeps track of them itself). */
    switch (key) {
    case VX_KEY_LEFTSHIFT:
    case VX_KEY_RIGHTSHIFT: shift = down; break;
    case VX_KEY_LEFTCTRL:
    case VX_KEY_RIGHTCTRL: ctrl = down; break;
    case VX_KEY_LEFTALT:
    case VX_KEY_RIGHTALT: alt = down; break;
    case VX_KEY_CAPSLOCK:
        if (value == 1) {
            caps_lock = !caps_lock;
        }
        break;
    }
    if (ctrl && alt && value == 1) {
        /* Apps' shortcuts ("Ctrl+Alt+T"). */
        for (int i = 0; i < app_count; i++) {
            const char *s = apps[i].shortcut;
            if (!strncmp(s, "Ctrl+Alt+", 9) && s[9] && !s[10] && key < (int)sizeof(keymap) &&
                keymap[key] == (s[9] | 0x20)) {
                run_app(i);
                return;
            }
        }
        if (key == 16) { /* Q */
            quit = true;
            return;
        }
    }
    if (alt && !ctrl && key == 15 && value != 0) { /* Tab */
        next_window();
        return;
    }
    if (menu_open && key == 1 && value == 1) { /* Escape */
        set_menu(false);
        return;
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

/* Which edges of a resizable window the pointer is on (for resizing). */
static bool on_edges(struct window *w, int x, int y, bool *right, bool *bottom) {
    if (!w->resizable || w->maximized) {
        return false;
    }
    struct rect f = frame_rect(w);
    *right = x >= f.x + f.width - 3;
    *bottom = y >= f.y + f.height - 3;
    return *right || *bottom;
}

static struct rect outline_damage(void) {
    return (struct rect){outline.x - BORDER - 2, outline.y - TITLE_HEIGHT - BORDER - 2,
                         outline.width + 2 * BORDER + 4,
                         outline.height + TITLE_HEIGHT + 2 * BORDER + 4};
}

/* A click on the panel. */
static void panel_click(void) {
    if (pointer_x < MENU_BUTTON_WIDTH + 4) {
        set_menu(!menu_open);
        return;
    }
    set_menu(false);
    struct window *list[MAX_WINDOWS];
    int n = windows_by_id(list);
    for (int i = 0; i < n; i++) {
        if (inside(task_button(i, n), pointer_x, pointer_y)) {
            if (list[i] == focused && !list[i]->minimized) {
                minimize(list[i]);
            } else {
                activate(list[i]);
            }
            return;
        }
    }
}

/* A click on a window's title bar: its buttons, or the start of a move. */
static void title_click(struct window *w) {
    int right = w->x + w->content.width;
    int button = pointer_x >= right - BUTTON_WIDTH ? 0
                 : pointer_x >= right - 2 * BUTTON_WIDTH ? 1
                 : pointer_x >= right - 3 * BUTTON_WIDTH ? 2 : -1;
    if (!w->resizable && button == 2) {
        button = -1;
    }
    if (button == 0) {
        struct desktop_message m = {.type = DESKTOP_CLOSE, .window = (uint32_t)w->id};
        send_to(w->client, &m);
        printf("desktop: asked window %d to close\n", w->id);
        return;
    }
    if ((button == 1 && w->resizable) || (button == 2 && w->resizable)) {
        if (button == 1) {
            toggle_maximized(w);
        } else {
            minimize(w);
        }
        return;
    }
    if (button == 1) { /* Not resizable: the second button minimizes. */
        minimize(w);
        return;
    }
    long now = vx_uptime();
    if (last_click_window == w && now - last_click_ms < DOUBLE_CLICK_MS) {
        last_click_window = NULL;
        toggle_maximized(w);
        return;
    }
    last_click_window = w;
    last_click_ms = now;
    start_move(w);
}

static void button_event(int bit, bool down) {
    int before = buttons;
    buttons = down ? buttons | bit : buttons & ~bit;
    bool left_down = bit == 1 && down && !(before & 1);
    if (left_down) {
        printf("desktop: left button at %d,%d\n", pointer_x, pointer_y);
    }
    if (left_down && menu_open && pointer_y >= PANEL_HEIGHT) {
        /* A click on an item runs it; anywhere else it just closes the menu. */
        int item = menu_item_at(pointer_x, pointer_y);
        if (item >= 0 || !inside(menu_rect(), pointer_x, pointer_y)) {
            set_menu(false);
        }
        if (item >= 0) {
            run_menu_item(item);
        }
        return;
    }
    if (left_down && pointer_y < PANEL_HEIGHT) {
        panel_click();
        return;
    }
    struct window *w = window_at(pointer_x, pointer_y);
    if (left_down && !w) {
        /* The desktop: its icons are selected, and started by a double click. */
        int hit = -1;
        for (int i = 0; i < LAUNCHERS; i++) {
            if (inside(launcher_rect(i), pointer_x, pointer_y)) {
                hit = i;
            }
        }
        long now = vx_uptime();
        if (hit >= 0 && hit == selected_launcher && now - last_click_ms < DOUBLE_CLICK_MS &&
            last_click_window == NULL) {
            char label[64];
            launcher_label(hit, label, sizeof(label));
            printf("desktop: starting %s\n", label);
            run_app(launcher_apps[hit]);
        }
        if (hit != selected_launcher) {
            selected_launcher = hit;
            add_damage(launchers_area());
        }
        last_click_ms = now;
        last_click_window = NULL;
        return;
    }
    if (left_down && w) {
        activate(w);
        bool right, bottom;
        if (on_edges(w, pointer_x, pointer_y, &right, &bottom)) {
            drag = RESIZING;
            dragged = w;
            resize_right = right;
            resize_bottom = bottom;
            drag_dx = w->x + w->content.width - pointer_x;
            drag_dy = w->y + w->content.height - pointer_y;
            outline = (struct rect){w->x, w->y, w->content.width, w->content.height};
            add_damage(outline_damage());
            return;
        }
        if (pointer_y < w->y && !w->popup && !w->undecorated) {
            title_click(w);
            return;
        }
    }
    if (bit == 1 && !down && drag != IDLE) {
        struct window *d = dragged;
        if (drag == MOVING && d && snap != SNAP_NONE) {
            /* Snapped: maximized, or half of the screen. */
            add_damage(snap_damage(snap));
            if (snap == SNAP_TOP) {
                d->maximized = false;
                toggle_maximized(d);
            } else {
                struct rect r = snap_rect(snap);
                add_damage(frame_rect(d));
                d->restore = (struct rect){d->x, d->y, d->content.width, d->content.height};
                d->x = r.x;
                d->y = r.y;
                d->maximized = false;
                configure(d, r.width, r.height);
                add_damage(frame_rect(d));
                send_moved(d);
                printf("desktop: snapped window %d to the %s\n", d->id,
                       snap == SNAP_LEFT ? "left" : "right");
            }
            snap = SNAP_NONE;
        } else if (drag == MOVING && d) {
            send_moved(d);
            printf("desktop: moved window %d to %d,%d\n", d->id, d->x, d->y);
        } else if (drag == RESIZING && d) {
            add_damage(outline_damage());
            if (outline.width != d->content.width || outline.height != d->content.height) {
                configure(d, outline.width, outline.height);
            }
        }
        drag = IDLE;
        dragged = NULL;
        return;
    }
    if (w && drag == IDLE && pointer_y >= w->y && pointer_x < w->x + w->content.width &&
        pointer_y < w->y + w->content.height) {
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
        if (drag == MOVING && dragged) {
            add_damage(frame_rect(dragged));
            dragged->x = pointer_x - drag_dx;
            dragged->y = pointer_y - drag_dy;
            int top = dragged->undecorated ? PANEL_HEIGHT : PANEL_HEIGHT + TITLE_HEIGHT + BORDER;
            if (dragged->y < top) {
                dragged->y = top; /* Keep the title bar out from under the panel. */
            }
            add_damage(frame_rect(dragged));
            enum snap target = snap_target();
            if (target != snap) {
                if (snap != SNAP_NONE) {
                    add_damage(snap_damage(snap));
                }
                snap = target;
                if (snap != SNAP_NONE) {
                    add_damage(snap_damage(snap));
                }
            }
        } else if (drag == RESIZING && dragged) {
            add_damage(outline_damage());
            if (resize_right) {
                outline.width = pointer_x + drag_dx - outline.x;
                outline.width = outline.width < MIN_WIDTH ? MIN_WIDTH : outline.width;
            }
            if (resize_bottom) {
                outline.height = pointer_y + drag_dy - outline.y;
                outline.height = outline.height < MIN_HEIGHT ? MIN_HEIGHT : outline.height;
            }
            add_damage(outline_damage());
        }
        /* The resize arrows over a resizable window's edges. */
        struct window *under = window_at(pointer_x, pointer_y);
        bool right, bottom;
        bool edge = drag == RESIZING ||
                    (drag == IDLE && under && on_edges(under, pointer_x, pointer_y, &right,
                                                       &bottom));
        resize_cursor = edge;
        if (menu_open) {
            int hot = menu_item_at(pointer_x, pointer_y);
            if (hot != menu_hot) {
                menu_hot = hot;
                add_damage(menu_rect());
            }
        }
        add_damage(cursor_rect());
        if (drag != IDLE) {
            return;
        }
    }
    struct window *w = window_at(pointer_x, pointer_y);
    if (w && pointer_y >= w->y && pointer_x < w->x + w->content.width &&
        pointer_y < w->y + w->content.height) {
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
    screen.width = wallpaper.width = (int)display.width;
    screen.height = wallpaper.height = (int)display.height;
    screen.stride = wallpaper.stride = screen.width;
    size_t size = (size_t)screen.width * screen.height * 4;
    screen.pixels = vx_map(size, VX_MAP_WRITE);
    wallpaper.pixels = vx_map(size, VX_MAP_WRITE);
    if (!frame || !screen.pixels || !wallpaper.pixels) {
        fprintf(stderr, "desktop: out of memory\n");
        return false;
    }
    read_config();
    make_wallpaper();
    build_menu();
    find_launchers();
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
        long minute = vx_time() / 60;
        if (minute != shown_minute) { /* The clock. */
            shown_minute = minute;
            add_damage(panel_rect());
        }
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
        long wait = expire_notes();
        wait = wait < 0 || wait > 500 ? 500 : wait + 1;
        if (vx_poll(polls, (size_t)count, wait) <= 0) {
            expire_notes();
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
