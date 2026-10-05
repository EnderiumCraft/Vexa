/* settings: Vexa's System Settings.
 *
 * A sidebar of sections (with a search field that finds them), and the
 * section's settings on the right: Appearance (dark or light, the accent
 * color), Wallpaper (pictures, gradients, how a picture fits), Desktop &
 * Panel (icons, the clock, notifications, snapping, title bars), Date &
 * Time (the time zone), Mouse & Keyboard (speed, double click, scrolling,
 * buttons, layout, key repeat), Display (resolution, size), Default Apps,
 * Startup, Network (the computer's name, the interfaces), Storage, About.
 *
 * Changes apply as they're made: Settings writes /etc/desktop.conf (or
 * apps.conf, hostname) through <vexa/settings.h>, which also keeps them on
 * a disk when there is one, and tells the desktop, which tells every
 * program. What it changes is printed ("settings: theme=light").
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/desktop.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/settings.h>
#include <vexa/users.h>
#include <vexa/syscall.h>
#include <vexa/time.h>

#define WIDTH 860
#define HEIGHT 580
#define SIDEBAR 210
#define LEFT (SIDEBAR + 24)         /* The content's left edge. */
#define CONTROL (LEFT + 220)        /* Where controls start, after their labels. */
#define ROW 34
#define PICTURES "/share/pictures"
#define REVERT_SECONDS 15

static struct vx_window *window;
static struct vx_settings desk; /* desktop.conf */
static int pointer_x, pointer_y;

/* ---- Sections ---- */

enum section {
    S_APPEARANCE, S_WALLPAPER, S_DESKTOP, S_DATE, S_INPUT, S_DISPLAY, S_SOUND, S_LOCK, S_DEFAULTS,
    S_STARTUP, S_NETWORK, S_STORAGE, S_ABOUT, S_USERS, SECTION_COUNT
};

static const struct {
    const char *label, *keywords;
    uint32_t color;
    char symbol;              /* (On the color, if its icon can't be read.) */
} sections[SECTION_COUNT] = {
    {"Appearance", "theme dark light mode accent color colour look", 0x9b6bff, 'A'},
    {"Wallpaper", "background picture image gradient fill fit tile", 0x3fbf6f, 'W'},
    {"Desktop & Panel", "icons clock seconds date weekday notifications snapping title bar",
     0x4c8dff, 'D'},
    {"Date & Time", "clock time zone city utc 12 24 hours", 0xff8c3a, 'T'},
    {"Mouse & Keyboard",
     "pointer speed double click scroll natural left handed buttons layout key repeat shortcuts",
     0x8e8ea0, 'M'},
    {"Display", "resolution screen size scale bigger mode monitor", 0x2ec4b6, 'S'},
    {"Sound", "volume mute speaker headphones headset output usb audio notifications chime",
     0xffa424, 'V'},
    {"Lock Screen", "screensaver password lock idle security", 0x5a6acf, 'K'},
    {"Default Apps", "open with file types extensions", 0xff5fa2, 'O'},
    {"Startup", "login start apps terminal", 0xf0524f, 'L'},
    {"Network", "computer name hostname address ip dns router internet", 0x4c8dff, 'N'},
    {"Storage", "disks space free used mounts", 0x8e8ea0, 'H'},
    {"About", "version system memory cpu restart shut down power off", 0x9b6bff, 'i'},
    {"Users", "accounts people login password administrator add remove", 0x3fbf6f, 'U'},
};

static enum section current = S_APPEARANCE;
static char search[40];

/* ---- Fields (typing) ---- */

static enum {
    FIELD_NONE, FIELD_SEARCH, FIELD_PICTURE, FIELD_NAME, FIELD_PASSWORD, FIELD_OLD_PASSWORD,
    FIELD_NEW_LOGIN, FIELD_NEW_FULL, FIELD_NEW_PASSWORD
} field;
static char picture_path[256], computer_name[80], password[64], old_password[64];
static char new_login[32], new_full[64], new_password[64];
static bool new_admin;
static char account_message[160]; /* What the last change to an account said. */

/* ---- What was drawn where: clicking finds it here ---- */

enum hit_kind {
    H_SECTION, H_SEGMENT, H_TOGGLE, H_SLIDER, H_ACCENT, H_PICTURE, H_GRADIENT, H_PICTURE_FIELD,
    H_USE_PICTURE, H_ICON_APP, H_STARTUP_APP, H_ZONE, H_MODE, H_DEFAULT_APP, H_NAME_FIELD,
    H_SAVE_NAME, H_POWER, H_KEEP, H_REVERT, H_SEARCH_FIELD, H_TEST_AREA, H_PASSWORD_FIELD,
    H_SET_PASSWORD, H_REMOVE_PASSWORD, H_LOCK_NOW, H_OUTPUT, H_TEST_SOUND, H_OLD_PASSWORD_FIELD,
    H_NEW_LOGIN_FIELD, H_NEW_FULL_FIELD, H_NEW_PASSWORD_FIELD, H_NEW_ADMIN, H_ADD_ACCOUNT,
    H_REMOVE_ACCOUNT, H_ADMIN_ACCOUNT, H_GO_WALLPAPER, H_ADD_PICTURE, H_WALLPAPER_MODE,
};

struct hit {
    int x, y, w, h;
    enum hit_kind kind;
    const char *key, *value; /* A setting, and the value this sets it to. */
    int index, min, max, step;
};

#define MAX_HITS 160
static struct hit hits[MAX_HITS];
static int hit_count;

/* The page (right of the sidebar) is drawn on a tall surface of its own and
 * shown from `scroll` down: long pages scroll with the wheel. */
#define PAGE_HEIGHT 1600
static struct vx_surface page;
static struct vx_surface *target; /* What's drawn on: the page, or the window. */
static int scroll, page_bottom;   /* (page_bottom: where a long page ends; 0 if it fits.) */

static struct hit *add_hit(int x, int y, int w, int h, enum hit_kind kind) {
    static struct hit spare;
    if (target == &page) { /* On the page: where it is in the window (if it shows). */
        y -= scroll;
        int bottom = y + h, end = window->surface.height;
        y = y < 0 ? 0 : y;
        h = (bottom > end ? end : bottom) - y;
        if (h <= 0) {
            return &spare;
        }
    }
    if (hit_count == MAX_HITS) {
        return &spare;
    }
    struct hit *hit = &hits[hit_count++];
    memset(hit, 0, sizeof(*hit));
    *hit = (struct hit){x, y, w, h, kind, NULL, NULL, 0, 0, 0, 0};
    return hit;
}

/* ---- Changing settings ---- */

static void changed(const char *key, const char *value) {
    printf("settings: %s=%s\n", key, value);
    fflush(stdout);
}

static void apply_volume(void);

/* Sets a desktop setting, saves, and tells the desktop. */
static void set(const char *key, const char *value) {
    vx_settings_set(&desk, key, value);
    vx_settings_save(&desk);
    changed(key, value);
    if (!strcmp(key, "volume") || !strcmp(key, "muted")) {
        apply_volume();
    }
    vx_desktop_reload();
}

static void set_int(const char *key, int value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    set(key, text);
}

static void set_bool(const char *key, bool value) {
    set(key, value ? "yes" : "no");
}

/* ---- The apps ---- */

#define MAX_APPS 32
static struct vx_app apps[MAX_APPS];
static int app_count;

static void bundle_stem(const struct vx_app *app, char *out, size_t size) {
    const char *file = strrchr(app->bundle, '/');
    snprintf(out, size, "%s", file ? file + 1 : app->bundle);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

/* True if `list` ("Terminal,Files") has `name`. */
static bool list_has(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        size_t m = strcspn(p, ",");
        if (m == n && !strncmp(p, name, n)) {
            return true;
        }
        p += m + (p[m] == ',');
    }
    return false;
}

/* `list` with `name` added or taken out. */
static void list_toggle(char *list, size_t size, const char *name) {
    char out[256] = "";
    bool had = list_has(list, name);
    for (const char *p = list; *p;) {
        size_t m = strcspn(p, ",");
        if (!(m == strlen(name) && !strncmp(p, name, m)) && m) {
            size_t used = strlen(out);
            snprintf(out + used, sizeof(out) - used, "%s%.*s", used ? "," : "", (int)m, p);
        }
        p += m + (p[m] == ',');
    }
    if (!had) {
        size_t used = strlen(out);
        snprintf(out + used, sizeof(out) - used, "%s%s", used ? "," : "", name);
    }
    snprintf(list, size, "%s", out);
}

/* ---- Drawing helpers ---- */

static struct vx_surface *S(void) {
    return target ? target : &window->surface;
}

static void text(int x, int y, const char *t, uint32_t color) {
    vx_draw_text(S(), x, y, t, color, VX_TRANSPARENT);
}

static int big_text(int x, int y, const char *t, int size, uint32_t color) {
    return vx_text(S(), vx_font(VX_FACE_BOLD, size), x, y, t, color, VX_TRANSPARENT);
}

static void rounded(int x, int y, int w, int h, uint32_t color) {
    vx_fill_rounded(S(), x, y, w, h, h / 2 < 8 ? h / 2 : 8, color, 255);
}

/* A sunken track (switches, sliders, bars): darker at the top. */
static void track(int x, int y, int w, int h, uint32_t color) {
    vx_fill_rounded(S(), x, y, w, h, h / 2, vx_mix(color, 0x000000, 60), 255);
    vx_fill_rounded(S(), x + 1, y + 1, w - 2, h - 2, (h - 2) / 2, color, 255);
    vx_fill(S(), x + h / 2, y + 1, w - h, 1, vx_mix(color, 0x000000, 35));
}

/* A white, glossy knob. */
static void knob(int x, int y, int w, int h) {
    vx_draw_gel(S(), x, y, w, h, h / 2, vx_theme.dark ? 0xd8d6e0 : 0xf4f5f8);
}

static void heading(const char *t, int y) {
    text(LEFT, y, t, VX_COLOR_ACCENT);
    vx_fill(S(), LEFT, y + VX_LINE_HEIGHT + 3, window->surface.width - LEFT - 24, 1, VX_COLOR_LINE);
}

static void label(int y, const char *t) {
    text(LEFT, y + 5, t, VX_COLOR_TEXT);
}

static void note(int y, const char *t) {
    vx_draw_text_fit(S(), LEFT, y, window->surface.width - LEFT - 24, t, VX_COLOR_DIM,
                     VX_TRANSPARENT);
}

/* A switch: on or off. */
static void toggle(int y, const char *title, const char *key, bool fallback) {
    label(y, title);
    bool on = vx_settings_bool(&desk, key, fallback);
    int x = CONTROL;
    if (on) {
        vx_draw_gel(S(), x, y + 3, 40, 20, 10, VX_COLOR_ACCENT);
    } else {
        track(x, y + 3, 40, 20, VX_COLOR_BUTTON_HOT);
    }
    knob(on ? x + 21 : x + 1, y + 4, 18, 18);
    struct hit *h = add_hit(x, y, 40 + 64, 26, H_TOGGLE);
    h->key = key;
    h->index = fallback;
    text(x + 50, y + 5, on ? "On" : "Off", VX_COLOR_DIM);
}

/* A row of choices, one of them chosen. */
struct option {
    const char *value, *title;
};

static void segments(int y, const char *title, const char *key, const struct option *options,
                     int count, const char *fallback) {
    if (title) {
        label(y, title);
    }
    const char *value = vx_settings_get(&desk, key, fallback);
    int x = title ? CONTROL : LEFT;
    for (int i = 0; i < count; i++) {
        int w = vx_text_width(options[i].title) + 20;
        bool on = !strcmp(value, options[i].value);
        vx_draw_button(S(), x, y + 1, w, 26, options[i].title, on);
        struct hit *h = add_hit(x, y + 1, w, 26, H_SEGMENT);
        h->key = key;
        h->value = options[i].value;
        x += w + 4;
    }
}

/* A value from min to max, by step: a track and a knob. */
static void slider(int y, const char *title, const char *key, int min, int max, int step,
                   int fallback, const char *format) {
    label(y, title);
    int value = vx_settings_int(&desk, key, fallback);
    value = value < min ? min : value > max ? max : value;
    int x = CONTROL, w = 180;
    track(x, y + 11, w, 6, VX_COLOR_BUTTON_HOT);
    int at = x + (int)((long)(value - min) * w / (max - min));
    if (at - x > 3) {
        vx_draw_gel(S(), x, y + 11, at - x + 3, 6, 3, VX_COLOR_ACCENT);
    }
    knob(at - 8, y + 6, 16, 16);
    char shown[32];
    snprintf(shown, sizeof(shown), format, value);
    text(x + w + 16, y + 5, shown, VX_COLOR_DIM);
    struct hit *h = add_hit(x - 8, y, w + 16, 28, H_SLIDER);
    h->key = key;
    h->min = min;
    h->max = max;
    h->step = step;
}

static void checkbox(int x, int y, bool on, const char *title) {
    vx_draw_check(S(), x, y + 3, on);
    text(x + 24, y + 3, title, VX_COLOR_TEXT);
}

static void format_bytes(char *out, size_t size, unsigned long long bytes) {
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(out, size, "%llu.%llu GiB", bytes >> 30, (bytes * 10 >> 30) % 10);
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%llu.%llu MiB", bytes >> 20, (bytes * 10 >> 20) % 10);
    } else {
        snprintf(out, size, "%llu KiB", bytes >> 10);
    }
}

/* A bar: used out of total. */
static void bar(int x, int y, int w, unsigned long long used, unsigned long long total) {
    track(x, y, w, 10, VX_COLOR_BUTTON_HOT);
    if (total) {
        int filled = (int)((unsigned long long)w * used / total);
        if (filled > 4) {
            vx_draw_gel(S(), x, y, filled, 10, 5, VX_COLOR_ACCENT);
        }
    }
}

/* ---- A menu (for choices from a list) ---- */

#define MAX_MENU 40
static struct vx_menu_item menu[MAX_MENU];
static int menu_values[MAX_MENU];
static int menu_count, menu_x, menu_y, menu_hot = -1;
static bool menu_open;
static const char *menu_key; /* The extension (Default Apps), or "power". */

static void open_menu(int x, int y) {
    int w, h;
    vx_menu_size(menu, menu_count, &w, &h);
    menu_x = x + w > window->surface.width - 4 ? window->surface.width - w - 4 : x;
    menu_y = y + h > window->surface.height - 4 ? window->surface.height - h - 4 : y;
    menu_hot = -1;
    menu_open = true;
}

/* ---- Cards, thumbnails (Appearance, Wallpaper) ---- */

#define CONTENT_W (WIDTH - LEFT - 24) /* The page's width, right of the sidebar. */

/* (The display, which the Display page reads: the Wallpaper page shows
 * pictures as on the screen.) */
static struct vx_display_info display_info;
static bool have_display;
#define CARD_PAD 16

static uint32_t card_color(void) {
    return vx_theme.dark ? vx_mix(vx_theme.window, 0xffffff, 14) : 0xffffff;
}

/* A card: a rounded panel grouping a few things, from y down h. */
static void card(int y, int h) {
    uint32_t edge = vx_theme.dark ? vx_mix(vx_theme.window, 0xffffff, 28) : 0xd9dde4;
    vx_fill_rounded(S(), LEFT, y, CONTENT_W, h, 12, edge, 255);
    vx_fill_rounded(S(), LEFT + 1, y + 1, CONTENT_W - 2, h - 2, 11, card_color(), 255);
}

/* A card's title (bold) and a line under it saying what it's for (dim). */
static void card_title(int y, const char *title, const char *about) {
    vx_text(S(), vx_font(VX_FACE_BOLD, 14), LEFT + CARD_PAD, y, title, VX_COLOR_TEXT,
            VX_TRANSPARENT);
    if (about) {
        vx_draw_text_fit(S(), LEFT + CARD_PAD, y + 22, CONTENT_W - 2 * CARD_PAD, about,
                         VX_COLOR_DIM, VX_TRANSPARENT);
    }
}

/* Takes the corners of (x, y, w, h) off, round (radius r), showing `bg`
 * there: smooth, a quarter circle at each corner. */
static void round_corners(struct vx_surface *s, int x, int y, int w, int h, int r, uint32_t bg) {
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            /* How much of this pixel is outside the circle (of 4 samples). */
            int cx = r - dx, cy = r - dy;
            int out = 0;
            for (int sub = 0; sub < 4; sub++) { /* (Four samples in the pixel.) */
                int sx = cx * 4 - (sub & 1) * 2 - 1, sy = cy * 4 - (sub >> 1) * 2 - 1;
                out += sx * sx + sy * sy > r * r * 16;
            }
            if (!out) {
                continue;
            }
            int corners[4][2] = {{x + dx, y + dy},
                                 {x + w - 1 - dx, y + dy},
                                 {x + dx, y + h - 1 - dy},
                                 {x + w - 1 - dx, y + h - 1 - dy}};
            for (int c = 0; c < 4; c++) {
                int px = corners[c][0], py = corners[c][1];
                if (px < 0 || py < 0 || px >= s->width || py >= s->height) {
                    continue;
                }
                uint32_t *p = &s->pixels[(size_t)py * s->stride + px];
                *p = vx_mix(*p, bg, out * 255 / 4);
            }
        }
    }
}

/* A tick: two strokes, `size` across, at (x, y). */
static void tick(struct vx_surface *s, int x, int y, int size, uint32_t color) {
    int t = size / 6 > 1 ? size / 6 : 2; /* (How thick.) */
    for (int i = 0; i <= size * 3 / 8; i++) { /* Down, to the bottom of the tick. */
        vx_fill(s, x + i, y + size / 2 + i - t / 2, t, t, color);
    }
    for (int i = 0; i <= size * 5 / 8; i++) { /* Then up, to the right. */
        vx_fill(s, x + size * 3 / 8 + i, y + size / 2 + size * 3 / 8 - i - t / 2, t, t, color);
    }
}

/* What's chosen: a ring of the accent around (x, y, w, h), and a tick in
 * a disc at its top right. */
static void chosen_ring(int x, int y, int w, int h, int r) {
    vx_fill_rounded(S(), x - 4, y - 4, w + 8, h + 8, r + 4, VX_COLOR_ACCENT, 255);
    vx_fill_rounded(S(), x - 2, y - 2, w + 4, h + 4, r + 2, card_color(), 255);
}

static void chosen_badge(int x, int y, int w) {
    int cx = x + w - 12, cy = y + 12;
    vx_fill_rounded(S(), cx - 11, cy - 11, 22, 22, 11, 0xffffff, 255);
    vx_draw_gel(S(), cx - 9, cy - 9, 18, 18, 9, VX_COLOR_ACCENT);
    tick(S(), cx - 5, cy - 6, 11, 0xffffff);
}

/* A smaller copy of `from` in (x, y, w, h) of `to`: each pixel the average
 * of a few of the ones it covers (not jagged, like the nearest one). */
static void blit_average(struct vx_surface *to, int x, int y, int w, int h,
                         const struct vx_surface *from, int fx, int fy, int fw, int fh) {
    for (int ty = 0; ty < h; ty++) {
        int py = y + ty;
        if (py < 0 || py >= to->height) {
            continue;
        }
        int y0 = fy + ty * fh / h, y1 = fy + (ty + 1) * fh / h;
        y1 = y1 > y0 ? y1 : y0 + 1;
        for (int tx = 0; tx < w; tx++) {
            int px = x + tx;
            if (px < 0 || px >= to->width) {
                continue;
            }
            int x0 = fx + tx * fw / w, x1 = fx + (tx + 1) * fw / w;
            x1 = x1 > x0 ? x1 : x0 + 1;
            /* At most 4 by 4 samples. */
            int sx = (x1 - x0 + 3) / 4, sy = (y1 - y0 + 3) / 4;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += sy) {
                const uint32_t *row = &from->pixels[(size_t)yy * from->stride];
                for (int xx = x0; xx < x1; xx += sx) {
                    uint32_t c = row[xx];
                    r += (c >> 16) & 0xff, g += (c >> 8) & 0xff, b += c & 0xff, n++;
                }
            }
            to->pixels[(size_t)py * to->stride + px] = (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

/* Thumbnails of pictures, made when Settings has nothing else to do (a big
 * picture takes a while to read): asked for while drawing, shown when
 * they're ready. */
enum thumb_kind {
    THUMB_FILL,    /* Covering w by h (its middle). */
    THUMB_FIT,     /* All of it, in w by h. */
    THUMB_STRETCH, /* To w by h. */
    THUMB_SCALE,   /* Made w/h of its size (h: the screen's width; w: the preview's). */
};

#define MAX_THUMBS 40
static struct thumb {
    char path[256];
    enum thumb_kind kind;
    int w, h;
    struct vx_image *image;
    int picture_w, picture_h; /* The picture's own size. */
    bool tried;
} thumbs[MAX_THUMBS];
static int thumb_next; /* (Which to reuse when they're all taken.) */

/* The thumbnail, or NULL until it's made (or if it can't be). */
static struct thumb *thumb_of(const char *path, enum thumb_kind kind, int w, int h) {
    for (int i = 0; i < MAX_THUMBS; i++) {
        struct thumb *t = &thumbs[i];
        if (t->path[0] && t->kind == kind && t->w == w && t->h == h && !strcmp(t->path, path)) {
            return t;
        }
    }
    struct thumb *t = NULL;
    for (int i = 0; i < MAX_THUMBS && !t; i++) {
        t = thumbs[i].path[0] ? NULL : &thumbs[i];
    }
    if (!t) {
        t = &thumbs[thumb_next];
        thumb_next = (thumb_next + 1) % MAX_THUMBS;
        vx_image_free(t->image);
    }
    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", path);
    t->kind = kind;
    t->w = w;
    t->h = h;
    return t;
}

static bool thumbs_waiting(void) {
    for (int i = 0; i < MAX_THUMBS; i++) {
        if (thumbs[i].path[0] && !thumbs[i].tried) {
            return true;
        }
    }
    return false;
}

static struct vx_image *new_image(int w, int h) {
    struct vx_image *image = malloc(sizeof(*image));
    uint32_t *pixels = image ? malloc((size_t)w * h * 4) : NULL;
    if (!pixels) {
        free(image);
        return NULL;
    }
    image->surface = (struct vx_surface){pixels, w, h, w};
    return image;
}

/* Makes the next thumbnail asked for (true if there was one). */
static bool make_next_thumbnail(void) {
    for (int i = 0; i < MAX_THUMBS; i++) {
        struct thumb *t = &thumbs[i];
        if (!t->path[0] || t->tried) {
            continue;
        }
        t->tried = true;
        struct vx_image *full = vx_image_load(t->path, 0);
        if (!full) {
            return true;
        }
        const struct vx_surface *f = &full->surface;
        t->picture_w = f->width;
        t->picture_h = f->height;
        int w = t->w, h = t->h, fx = 0, fy = 0, fw = f->width, fh = f->height;
        if (t->kind == THUMB_FILL) { /* Its middle, in w by h's shape. */
            if ((long)fw * h > (long)fh * w) {
                fw = (int)((long)fh * w / h);
                fx = (f->width - fw) / 2;
            } else {
                fh = (int)((long)fw * h / w);
                fy = (f->height - fh) / 2;
            }
        } else if (t->kind == THUMB_FIT) {
            if ((long)fw * h > (long)fh * w) {
                h = (int)((long)fh * w / fw);
            } else {
                w = (int)((long)fw * h / fh);
            }
        } else if (t->kind == THUMB_SCALE) {
            w = (int)((long)fw * t->w / t->h);
            h = (int)((long)fh * t->w / t->h);
        }
        w = w < 1 ? 1 : w > 2048 ? 2048 : w;
        h = h < 1 ? 1 : h > 2048 ? 2048 : h;
        t->image = new_image(w, h);
        if (t->image) {
            blit_average(&t->image->surface, 0, 0, w, h, f, fx, fy, fw, fh);
        }
        vx_image_free(full);
        return true;
    }
    return false;
}

/* A picture's name to show: "aurora.png" is "Aurora". */
static void picture_name(const char *path, char *out, size_t size) {
    const char *file = strrchr(path, '/');
    file = file ? file + 1 : path;
    const char *dot = strrchr(file, '.');
    int n = dot && dot > file ? (int)(dot - file) : (int)strlen(file);
    snprintf(out, size, "%.*s", n, file);
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 'a' + 'A');
    }
}

/* ---- What the wallpaper is ---- */

static const char *setting_wallpaper(void) {
    return vx_settings_get(&desk, "wallpaper", "image");
}

static const char *setting_image(void) {
    return vx_settings_get(&desk, "wallpaper_image", DESKTOP_DEFAULT_WALLPAPER);
}

static bool is_default_picture(const char *path) {
    return !strcmp(path, DESKTOP_DEFAULT_WALLPAPER);
}

/* The picture the desktop shows for `path` in a theme (the default one is in
 * the theme's colors). */
static void shown_picture(const char *path, const struct vx_theme *t, char *out, size_t size) {
    if (is_default_picture(path)) {
        vx_theme_wallpaper(t, out, size);
    } else {
        snprintf(out, size, "%s", path);
    }
}

/* The wallpaper as the desktop would show it, in (x, y, w, h) of `s`: a
 * picture (filling it, as it fills the screen) or a gradient. */
static void draw_wallpaper_into(struct vx_surface *s, int x, int y, int w, int h,
                                const struct vx_theme *t) {
    const char *wallpaper = setting_wallpaper();
    if (!strcmp(wallpaper, "image")) {
        char path[256];
        shown_picture(setting_image(), t, path, sizeof(path));
        struct thumb *th = thumb_of(path, THUMB_FILL, w, h);
        if (th->image) {
            vx_blit(s, x, y, &th->image->surface, 0, 0, w, h);
            return;
        }
        /* Until it's read: the accent, deep (like the default picture). */
        for (int row = 0; row < h; row++) {
            vx_fill(s, x, y + row, w, 1,
                    vx_mix(vx_mix(t->accent, 0, t->dark ? 150 : 70),
                           vx_mix(t->accent, 0, t->dark ? 230 : 200), row * 255 / (h - 1)));
        }
        return;
    }
    const struct desktop_wallpaper *g = &desktop_wallpapers[0];
    for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
        if (!strcmp(desktop_wallpapers[i].name, wallpaper)) {
            g = &desktop_wallpapers[i];
        }
    }
    for (int row = 0; row < h; row++) {
        vx_fill(s, x, y + row, w, 1, vx_mix(g->top, g->bottom, row * 255 / (h - 1)));
    }
}

/* ---- Appearance ---- */

#define PREVIEW_W 168
#define PREVIEW_H 105

/* A little desktop in a theme: the wallpaper, the panel, and a window (its
 * glass title bar and three balls, a sidebar, text and a button). */
static void draw_little_desktop(struct vx_surface *s, const struct vx_theme *t) {
    int w = s->width, h = s->height;
    draw_wallpaper_into(s, 0, 0, w, h, t);
    vx_fill_rounded(s, 0, 0, w, 9, 0, t->panel, 215);
    vx_fill_rounded(s, 3, 2, 14, 5, 2, t->accent, 255);
    vx_fill(s, w - 22, 4, 18, 2, t->text);
    /* The window: its shadow, title bar, sidebar and content. */
    int wx = 20, wy = 21, ww = w - 40, wh = h - 32;
    vx_fill_rounded(s, wx - 2, wy, ww + 4, wh + 4, 7, 0x000000, 70);
    vx_fill_rounded(s, wx, wy, ww, wh, 6, t->window, 255);
    uint32_t bar = t->title_focused;
    vx_fill_rounded(s, wx, wy, ww, 14, 6, bar, 255);
    vx_fill(s, wx, wy + 8, ww, 6, bar);
    for (int row = 0; row < 6; row++) {
        vx_fill(s, wx + 4, wy + 1 + row, ww - 8, 1, vx_mix(bar, 0xffffff, 110 - row * 16));
    }
    static const uint32_t balls[3] = {0xfebc2e, 0x2ac845, 0xff5f57};
    for (int i = 0; i < 3; i++) {
        vx_draw_gel(s, wx + ww - 30 + i * 9, wy + 4, 7, 7, 3, balls[i]);
    }
    vx_fill(s, wx, wy + 14, 30, wh - 14, t->sidebar);
    vx_fill_rounded(s, wx + 3, wy + 18, 24, 6, 3, t->accent, 255);
    vx_fill(s, wx + 5, wy + 29, 18, 3, t->dim);
    vx_fill(s, wx + 5, wy + 36, 14, 3, t->dim);
    vx_fill(s, wx + 38, wy + 20, 54, 5, t->text);
    vx_fill(s, wx + 38, wy + 30, ww - 50, 3, t->dim);
    vx_fill(s, wx + 38, wy + 37, ww - 60, 3, t->dim);
    vx_draw_gel(s, wx + ww - 38, wy + wh - 15, 30, 10, 5, t->accent);
    vx_fill_rounded(s, wx + ww - 74, wy + wh - 15, 30, 10, 5, t->button, 255);
}

static void draw_theme_choice(int x, int y, const char *value, const char *title,
                              const char *about, bool on) {
    static uint32_t pixels[2][PREVIEW_W * PREVIEW_H];
    struct vx_surface little[2] = {{pixels[0], PREVIEW_W, PREVIEW_H, PREVIEW_W},
                                   {pixels[1], PREVIEW_W, PREVIEW_H, PREVIEW_W}};
    const char *accent = vx_settings_get(&desk, "accent", "blue");
    struct vx_theme light, dark;
    vx_theme_make(&light, "light", accent);
    vx_theme_make(&dark, "dark", accent);
    bool is_auto = !strcmp(value, "auto");
    draw_little_desktop(&little[0], !strcmp(value, "dark") ? &dark : &light);
    if (is_auto) { /* Half of each: light at the left, dark at the right, cut aslant. */
        draw_little_desktop(&little[1], &dark);
        for (int row = 0; row < PREVIEW_H; row++) {
            int cut = PREVIEW_W / 2 + (PREVIEW_H / 2 - row) * 2 / 5;
            memcpy(&pixels[0][row * PREVIEW_W + cut], &pixels[1][row * PREVIEW_W + cut],
                   (size_t)(PREVIEW_W - cut) * 4);
        }
    }
    /* Chosen: a ring of the accent. If not, a thin edge (light ones stand out). */
    uint32_t edge = vx_theme.dark ? vx_mix(card_color(), 0xffffff, 40) : 0xc9ced8;
    if (on) {
        chosen_ring(x, y, PREVIEW_W, PREVIEW_H, 10);
    } else {
        vx_fill_rounded(S(), x - 1, y - 1, PREVIEW_W + 2, PREVIEW_H + 2, 11, edge, 255);
    }
    vx_blit(S(), x, y, &little[0], 0, 0, PREVIEW_W, PREVIEW_H);
    round_corners(S(), x, y, PREVIEW_W, PREVIEW_H, 10, on ? card_color() : edge);
    if (on) {
        chosen_badge(x, y, PREVIEW_W);
    }
    int tw = vx_text_width(title);
    vx_text(S(), vx_font(on ? VX_FACE_BOLD : VX_FACE_SANS, 13), x + (PREVIEW_W - tw) / 2,
            y + PREVIEW_H + 10, title, VX_COLOR_TEXT, VX_TRANSPARENT);
    int aw = vx_text_width(about);
    vx_draw_text_fit(S(), x + (aw < PREVIEW_W ? (PREVIEW_W - aw) / 2 : 0), y + PREVIEW_H + 28,
                     PREVIEW_W, about, VX_COLOR_DIM, VX_TRANSPARENT);
    struct hit *h = add_hit(x - 4, y - 4, PREVIEW_W + 8, PREVIEW_H + 52, H_SEGMENT);
    h->key = "theme";
    h->value = value;
}

/* A sample of the controls, in the colors chosen (not working: to look at). */
static void draw_sample(int x, int y) {
    vx_draw_button(S(), x, y, 82, 26, "Cancel", false);
    vx_draw_button_flags(S(), x + 90, y, 82, 26, "OK", VX_BUTTON_HOT);
    vx_draw_gel(S(), x + 192, y + 3, 40, 20, 10, VX_COLOR_ACCENT); /* A switch, on. */
    knob(x + 213, y + 4, 18, 18);
    track(x + 248, y + 3, 40, 20, VX_COLOR_BUTTON_HOT); /* ...and off. */
    knob(x + 249, y + 4, 18, 18);
    vx_draw_check(S(), x + 306, y + 5, true);
    text(x + 328, y + 5, "Checked", VX_COLOR_TEXT);
    vx_draw_check(S(), x + 410, y + 5, false);
    text(x + 432, y + 5, "Not", VX_COLOR_TEXT);
    y += 40;
    track(x, y + 8, 172, 6, VX_COLOR_BUTTON_HOT); /* A slider. */
    vx_draw_gel(S(), x, y + 8, 108, 6, 3, VX_COLOR_ACCENT);
    knob(x + 100, y + 3, 16, 16);
    vx_draw_progress(S(), x + 192, y + 5, 150, 12, 62, 100);
    vx_draw_field(S(), x + 362, y - 2, 176, "Some text", false);
    y += 34;
    uint32_t list = vx_theme.view;
    vx_fill_rounded(S(), x, y, 538, 58, 6, VX_COLOR_LINE, 255);
    vx_fill_rounded(S(), x + 1, y + 1, 536, 56, 5, list, 255);
    vx_draw_selection(S(), x + 4, y + 4, 530, 24);
    text(x + 14, y + 8, "A chosen row", 0xffffff);
    text(x + 14, y + 34, "Another row", VX_COLOR_TEXT);
    text(x + 440, y + 34, "12 KB", VX_COLOR_DIM);
}

static void draw_appearance(void) {
    int y = 60;
    const char *theme = vx_settings_get(&desk, "theme", "light");
    /* The theme: light, dark, or each when it suits (dark at night). */
    card(y, 230);
    card_title(y + 14, "Theme", "How windows, the panel and menus look. Apps change at once.");
    static const char *const values[] = {"light", "dark", "auto"};
    static const char *const titles[] = {"Light", "Dark", "Automatic"};
    char night[48];
    snprintf(night, sizeof(night), "Dark from %d pm to %d am", VX_THEME_NIGHT_STARTS - 12,
             VX_THEME_DAY_STARTS);
    const char *abouts[] = {"Bright, for daytime", "Easy on the eyes", night};
    int gap = (CONTENT_W - 2 * CARD_PAD - 3 * PREVIEW_W) / 2;
    for (int i = 0; i < 3; i++) {
        draw_theme_choice(LEFT + CARD_PAD + i * (PREVIEW_W + gap), y + 62, values[i], titles[i],
                          abouts[i], !strcmp(theme, values[i]));
    }
    y += 230 + 14;

    /* The accent: a color for what's chosen, buttons and the glass. */
    card(y, 126);
    const char *accent = vx_settings_get(&desk, "accent", "blue");
    int chosen = 1;
    for (int i = 0; i < vx_accent_count; i++) {
        chosen = !strcmp(accent, vx_accents[i].name) ? i : chosen;
    }
    char about[96];
    snprintf(about, sizeof(about), "%s: for selections, buttons, switches and title bars.",
             vx_accents[chosen].label);
    card_title(y + 14, "Accent color", about);
    int step = (CONTENT_W - 2 * CARD_PAD) / vx_accent_count;
    for (int i = 0; i < vx_accent_count; i++) {
        int cx = LEFT + CARD_PAD + i * step + step / 2, cy = y + 74;
        bool on = i == chosen;
        if (on) {
            vx_fill_rounded(S(), cx - 22, cy - 22, 44, 44, 22, vx_accents[i].color, 255);
            vx_fill_rounded(S(), cx - 20, cy - 20, 40, 40, 20, card_color(), 255);
        }
        vx_draw_gel(S(), cx - 16, cy - 16, 32, 32, 16, vx_accents[i].color);
        if (on) {
            tick(S(), cx - 7, cy - 8, 14, 0xffffff);
        }
        int lw = vx_text_width(vx_accents[i].label);
        vx_draw_text_fit(S(), cx - (lw < step ? lw / 2 : step / 2), cy + 26, step,
                         vx_accents[i].label, on ? VX_COLOR_TEXT : VX_COLOR_DIM, VX_TRANSPARENT);
        struct hit *h = add_hit(cx - step / 2, cy - 24, step, 64, H_ACCENT);
        h->index = i;
    }
    y += 126 + 14;

    /* How it looks: a few controls, in these colors. */
    card(y, 198);
    card_title(y + 14, "Preview", "Vexa's apps draw their controls like this.");
    draw_sample(LEFT + CARD_PAD, y + 60);
    y += 198 + 14;

    /* The wallpaper (it's on its own page): what it is, and a way there. */
    card(y, 76);
    static uint32_t pixels[96 * 60];
    struct vx_surface small = {pixels, 96, 60, 96};
    draw_wallpaper_into(&small, 0, 0, 96, 60, &vx_theme);
    vx_blit(S(), LEFT + CARD_PAD, y + 8, &small, 0, 0, 96, 60);
    round_corners(S(), LEFT + CARD_PAD, y + 8, 96, 60, 6, card_color());
    char name[64];
    const char *wallpaper = setting_wallpaper();
    if (!strcmp(wallpaper, "image")) {
        picture_name(setting_image(), name, sizeof(name));
    } else {
        snprintf(name, sizeof(name), "%s", "Gradient");
        for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
            if (!strcmp(desktop_wallpapers[i].name, wallpaper)) {
                snprintf(name, sizeof(name), "%s", desktop_wallpapers[i].label);
            }
        }
    }
    int tx = LEFT + CARD_PAD + 112;
    vx_text(S(), vx_font(VX_FACE_BOLD, 13), tx, y + 18, "Wallpaper", VX_COLOR_TEXT, VX_TRANSPARENT);
    char line[96];
    snprintf(line, sizeof(line), "%s%s", name,
             !strcmp(wallpaper, "image") && is_default_picture(setting_image())
                 ? " - in your theme's colors" : "");
    text(tx, y + 40, line, VX_COLOR_DIM);
    int bx = LEFT + CONTENT_W - CARD_PAD - 110;
    vx_draw_button(S(), bx, y + 24, 110, 28, "Change...", false);
    add_hit(bx, y + 24, 110, 28, H_GO_WALLPAPER);
    y += 76;
    page_bottom = y + 24;
}

/* ---- Wallpaper ---- */

/* Pictures: Vexa's (in /share/pictures), then the person's own (in their
 * Pictures folder). */
#define MAX_PICTURES 24
static struct picture {
    char path[256];
    char name[64];
    bool mine;
} pictures[MAX_PICTURES];
static int picture_count;

static void find_in(const char *folder, bool mine) {
    int handle = vx_open(folder, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    struct vx_dir_entry entries[16];
    long n;
    int limit = mine ? MAX_PICTURES : 12;
    while ((n = vx_read_dir(handle, entries, 16)) > 0) {
        for (long i = 0; i < n && picture_count < limit; i++) {
            const char *dot = strrchr(entries[i].name, '.');
            if (entries[i].type != VX_TYPE_FILE || !dot ||
                (strcmp(dot, ".png") && strcmp(dot, ".bmp") && strcmp(dot, ".ppm"))) {
                continue;
            }
            struct picture *p = &pictures[picture_count++];
            memset(p, 0, sizeof(*p));
            snprintf(p->path, sizeof(p->path), "%s/%s", folder, entries[i].name);
            picture_name(p->path, p->name, sizeof(p->name));
            p->mine = mine;
        }
    }
    vx_close(handle);
}

static void find_pictures(void) {
    picture_count = 0;
    find_in(PICTURES, false);
    /* The default first. */
    for (int i = 1; i < picture_count; i++) {
        if (is_default_picture(pictures[i].path)) {
            struct picture p = pictures[0];
            pictures[0] = pictures[i];
            pictures[i] = p;
        }
    }
    find_in(vx_home_folder("Pictures"), true);
}

#define TILE_W 176
#define TILE_H 110

/* A picture to choose: its thumbnail, rounded, and its name. */
static void draw_picture_tile(int x, int y, const struct picture *p, int index, bool on) {
    if (on) {
        chosen_ring(x, y, TILE_W, TILE_H, 10);
    }
    char path[256];
    shown_picture(p->path, &vx_theme, path, sizeof(path));
    struct thumb *t = thumb_of(path, THUMB_FILL, TILE_W, TILE_H);
    if (t->image) {
        vx_blit(S(), x, y, &t->image->surface, 0, 0, TILE_W, TILE_H);
    } else {
        vx_fill(S(), x, y, TILE_W, TILE_H, VX_COLOR_BUTTON);
        const char *what = t->tried ? "Can't be read" : "Loading...";
        text(x + (TILE_W - vx_text_width(what)) / 2, y + TILE_H / 2 - 8, what, VX_COLOR_DIM);
    }
    round_corners(S(), x, y, TILE_W, TILE_H, 10, card_color());
    if (on) {
        chosen_badge(x, y, TILE_W);
    }
    vx_draw_text_fit(S(), x, y + TILE_H + 8, TILE_W, p->name, on ? VX_COLOR_TEXT : VX_COLOR_DIM,
                     VX_TRANSPARENT);
    if (is_default_picture(p->path)) {
        vx_draw_text_fit(S(), x, y + TILE_H + 26, TILE_W, "Changes with your colors",
                         VX_COLOR_DIM, VX_TRANSPARENT);
    }
    struct hit *h = add_hit(x - 4, y - 4, TILE_W + 8, TILE_H + 30, H_PICTURE);
    h->index = index;
}

/* The tile that adds a picture from anywhere (the Open dialog). */
static void draw_add_tile(int x, int y) {
    uint32_t edge = vx_theme.dark ? vx_mix(card_color(), 0xffffff, 40) : 0xc9ced8;
    vx_fill_rounded(S(), x, y, TILE_W, TILE_H, 10, edge, 255);
    vx_fill_rounded(S(), x + 2, y + 2, TILE_W - 4, TILE_H - 4, 9,
                    vx_mix(card_color(), VX_COLOR_ACCENT, 18), 255);
    int cx = x + TILE_W / 2, cy = y + TILE_H / 2 - 10;
    vx_draw_gel(S(), cx - 16, cy - 16, 32, 32, 16, VX_COLOR_ACCENT);
    vx_fill(S(), cx - 8, cy - 1, 16, 3, 0xffffff);
    vx_fill(S(), cx - 1, cy - 8, 3, 16, 0xffffff);
    const char *t = "Choose a Picture...";
    text(cx - vx_text_width(t) / 2, cy + 24, t, VX_COLOR_TEXT);
    add_hit(x, y, TILE_W, TILE_H, H_ADD_PICTURE);
}

/* The screen's size (pictures are shown as on it). */
static void screen_size(int *w, int *h) {
    int scale = vx_settings_int(&desk, "display_scale", 1);
    scale = scale < 1 ? 1 : scale;
    *w = have_display && display_info.width ? (int)display_info.width / scale : 1280;
    *h = have_display && display_info.height ? (int)display_info.height / scale : 800;
}

#define SCREEN_W 272

/* The screen, small: the wallpaper as it's placed (fill, fit, center,
 * tile, stretch), the panel across its top. */
static void draw_screen_preview(int x, int y, int sh) {
    int sw = SCREEN_W, w, h;
    screen_size(&w, &h);
    /* The monitor: a dark bezel, and its stand. */
    vx_fill_rounded(S(), x + sw / 2 - 30, y + sh + 12, 60, 6, 3, VX_COLOR_DIM, 255);
    vx_fill(S(), x + sw / 2 - 10, y + sh + 6, 20, 7, vx_mix(VX_COLOR_DIM, 0, 40));
    vx_fill_rounded(S(), x - 6, y - 6, sw + 12, sh + 12, 10, 0x1a1a1c, 255);
    struct vx_surface *s = S();
    const char *mode = vx_settings_get(&desk, "wallpaper_mode", "fill");
    if (strcmp(setting_wallpaper(), "image") || !strcmp(mode, "fill")) {
        draw_wallpaper_into(s, x, y, sw, sh, &vx_theme);
    } else {
        for (int row = 0; row < sh; row++) { /* Behind a picture that doesn't cover it. */
            vx_fill(s, x, y + row, sw, 1, vx_mix(0x202028, 0x08080c, row * 255 / (sh - 1)));
        }
        char path[256];
        shown_picture(setting_image(), &vx_theme, path, sizeof(path));
        enum thumb_kind kind = !strcmp(mode, "fit") ? THUMB_FIT
                               : !strcmp(mode, "stretch") ? THUMB_STRETCH : THUMB_SCALE;
        struct thumb *t = kind == THUMB_SCALE ? thumb_of(path, kind, sw, w)
                                              : thumb_of(path, kind, sw, sh);
        if (t->image) {
            const struct vx_surface *im = &t->image->surface;
            int iw = im->width, ih = im->height;
            int ox = (sw - iw) / 2, oy = (sh - ih) / 2, step_x = sw, step_y = sh;
            if (!strcmp(mode, "tile")) {
                ox = oy = 0;
                step_x = iw;
                step_y = ih;
            }
            for (int ty = oy; ty < sh; ty += step_y) {
                for (int tx = ox; tx < sw; tx += step_x) {
                    /* (Only what's inside the screen.) */
                    int fx = tx < 0 ? -tx : 0, fy = ty < 0 ? -ty : 0;
                    int cw = (tx + iw > sw ? sw - tx : iw) - fx;
                    int ch = (ty + ih > sh ? sh - ty : ih) - fy;
                    if (cw > 0 && ch > 0) {
                        vx_blit(s, x + tx + fx, y + ty + fy, im, fx, fy, cw, ch);
                    }
                    if (strcmp(mode, "tile")) {
                        break;
                    }
                }
                if (strcmp(mode, "tile")) {
                    break;
                }
            }
        }
    }
    vx_fill_rounded(s, x, y, sw, 7, 0, vx_theme.panel, 215); /* The panel. */
    vx_fill_rounded(s, x + 3, y + 1, 12, 5, 2, VX_COLOR_ACCENT, 255);
    round_corners(s, x, y, sw, sh, 4, 0x1a1a1c);
}

static const struct option wallpaper_modes[] = {
    {"fill", "Fill the screen"}, {"fit", "Fit to the screen"}, {"center", "Center"},
    {"tile", "Tile"},            {"stretch", "Stretch"},
};
#define WALLPAPER_MODE_COUNT 5

static void draw_wallpaper(void) {
    int y = 60, w, h;
    screen_size(&w, &h);
    int sh = SCREEN_W * h / w;
    const char *wallpaper = setting_wallpaper();
    bool picture = !strcmp(wallpaper, "image");

    /* What it is now: the screen, small, and how a picture is placed. */
    int top = sh + 58 > 200 ? sh + 58 : 200;
    card(y, top);
    draw_screen_preview(LEFT + CARD_PAD + 6, y + 22, sh);
    int rx = LEFT + CARD_PAD + SCREEN_W + 36, rw = LEFT + CONTENT_W - CARD_PAD - rx;
    char name[64], about[96];
    if (picture) {
        picture_name(setting_image(), name, sizeof(name));
        if (is_default_picture(setting_image())) {
            snprintf(about, sizeof(about), "Vexa's picture, in your theme's colors");
        } else {
            char path[256];
            shown_picture(setting_image(), &vx_theme, path, sizeof(path));
            struct thumb *t = thumb_of(path, THUMB_FILL, TILE_W, TILE_H);
            if (t->picture_w) {
                snprintf(about, sizeof(about), "A picture, %d x %d", t->picture_w, t->picture_h);
            } else {
                snprintf(about, sizeof(about), "A picture");
            }
        }
    } else {
        snprintf(name, sizeof(name), "Gradient");
        for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
            if (!strcmp(desktop_wallpapers[i].name, wallpaper)) {
                snprintf(name, sizeof(name), "%s", desktop_wallpapers[i].label);
            }
        }
        snprintf(about, sizeof(about), "A color, darker at the bottom");
    }
    vx_text(S(), vx_font(VX_FACE_BOLD, 18), rx, y + 20, name, VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_text_fit(S(), rx, y + 46, rw, about, VX_COLOR_DIM, VX_TRANSPARENT);
    text(rx, y + 82, "Position", picture ? VX_COLOR_TEXT : VX_COLOR_DIM);
    const char *mode = vx_settings_get(&desk, "wallpaper_mode", "fill");
    const char *mode_title = wallpaper_modes[0].title;
    for (int i = 0; i < WALLPAPER_MODE_COUNT; i++) {
        mode_title = !strcmp(mode, wallpaper_modes[i].value) ? wallpaper_modes[i].title
                                                             : mode_title;
    }
    char shown[48];
    snprintf(shown, sizeof(shown), "%s  v", mode_title);
    vx_draw_button_flags(S(), rx, y + 102, rw, 28, shown, picture ? 0 : VX_BUTTON_DISABLED);
    if (picture) {
        add_hit(rx, y + 102, rw, 28, H_WALLPAPER_MODE);
    }
    vx_draw_button(S(), rx, y + 142, rw, 28, "Choose a Picture...", false);
    add_hit(rx, y + 142, rw, 28, H_ADD_PICTURE);
    y += top + 14;

    /* Vexa's pictures. */
    int gap = (CONTENT_W - 2 * CARD_PAD - 3 * TILE_W) / 2;
    int vexa = 0, mine = 0;
    for (int i = 0; i < picture_count; i++) {
        vexa += !pictures[i].mine;
        mine += pictures[i].mine;
    }
    int row_h = TILE_H + 50;
    int vexa_h = 58 + ((vexa + 2) / 3) * row_h;
    card(y, vexa_h);
    card_title(y + 14, "Vexa Pictures", NULL);
    for (int i = 0, n = 0; i < picture_count; i++) {
        if (pictures[i].mine) {
            continue;
        }
        bool on = picture && !strcmp(setting_image(), pictures[i].path);
        draw_picture_tile(LEFT + CARD_PAD + (n % 3) * (TILE_W + gap), y + 50 + (n / 3) * row_h,
                          &pictures[i], i, on);
        n++;
    }
    y += vexa_h + 14;

    /* The person's own pictures (their Pictures folder), and others from
     * anywhere. */
    int mine_h = 64 + ((mine + 1 + 2) / 3) * row_h - 24;
    card(y, mine_h);
    card_title(y + 14, "Your Pictures", "From the Pictures folder in your home (PNG, BMP or PPM).");
    int n = 0;
    for (int i = 0; i < picture_count; i++) {
        if (!pictures[i].mine) {
            continue;
        }
        bool on = picture && !strcmp(setting_image(), pictures[i].path);
        draw_picture_tile(LEFT + CARD_PAD + (n % 3) * (TILE_W + gap), y + 64 + (n / 3) * row_h,
                          &pictures[i], i, on);
        n++;
    }
    draw_add_tile(LEFT + CARD_PAD + (n % 3) * (TILE_W + gap), y + 64 + (n / 3) * row_h);
    y += mine_h + 14;

    /* Colors: gradients. */
    int per_row = 5, cw = (CONTENT_W - 2 * CARD_PAD - (per_row - 1) * 14) / per_row, ch = 64;
    int colors_h = 58 + ((DESKTOP_WALLPAPER_COUNT + per_row - 1) / per_row) * (ch + 40);
    card(y, colors_h);
    card_title(y + 14, "Colors", NULL);
    for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
        int x = LEFT + CARD_PAD + (i % per_row) * (cw + 14), gy = y + 50 + (i / per_row) * (ch + 40);
        bool on = !strcmp(wallpaper, desktop_wallpapers[i].name);
        if (on) {
            chosen_ring(x, gy, cw, ch, 10);
        }
        for (int row = 0; row < ch; row++) {
            vx_fill(S(), x, gy + row, cw, 1,
                    vx_mix(desktop_wallpapers[i].top, desktop_wallpapers[i].bottom, row * 255 / (ch - 1)));
        }
        round_corners(S(), x, gy, cw, ch, 10, card_color());
        if (on) {
            tick(S(), x + cw / 2 - 8, gy + ch / 2 - 9, 16, 0xffffff);
        }
        vx_draw_text_fit(S(), x, gy + ch + 8, cw, desktop_wallpapers[i].label,
                         on ? VX_COLOR_TEXT : VX_COLOR_DIM, VX_TRANSPARENT);
        struct hit *hit = add_hit(x - 4, gy - 4, cw + 8, ch + 30, H_GRADIENT);
        hit->index = i;
    }
    y += colors_h;
    page_bottom = y + 24;
}

/* ---- Desktop & Panel ---- */

static void draw_desktop(void) {
    int y = 64;
    heading("Desktop", y);
    y += 28;
    toggle(y, "Icons on the desktop", "desktop_icons", true);
    y += ROW;
    label(y, "Icons for");
    const char *chosen = vx_settings_get(&desk, "desktop_apps", "");
    int x = CONTROL, row_y = y;
    for (int i = 0; i < app_count; i++) {
        char stem[64];
        bundle_stem(&apps[i], stem, sizeof(stem));
        bool on = chosen[0] ? list_has(chosen, stem) : apps[i].desktop;
        int w = 24 + vx_text_width(stem) + 16;
        if (x + w > window->surface.width - 20) {
            x = CONTROL;
            row_y += 26;
        }
        checkbox(x, row_y, on, stem);
        struct hit *h = add_hit(x, row_y, w, 24, H_ICON_APP);
        h->index = i;
        x += w;
    }
    y = row_y + ROW + 4;
    heading("Clock", y);
    y += 28;
    static const struct option hours[] = {{"24", "24-hour"}, {"12", "12-hour"}};
    segments(y, "Hours", "clock", hours, 2, "24");
    y += ROW;
    toggle(y, "Show the weekday", "clock_weekday", true);
    y += ROW;
    toggle(y, "Show the date", "clock_date", true);
    y += ROW;
    toggle(y, "Show seconds", "clock_seconds", false);
    y += ROW + 4;
    heading("Windows and notifications", y);
    y += 28;
    toggle(y, "Snap to the screen's edges", "snapping", true);
    y += ROW;
    toggle(y, "Animations", "animations", true);
    y += ROW;
    static const struct option clicks[] = {
        {"maximize", "Maximize"}, {"minimize", "Minimize"}, {"none", "Nothing"},
    };
    segments(y, "Double click on a title", "title_double_click", clicks, 3, "maximize");
    y += ROW;
    slider(y, "Notifications stay", "notification_seconds", 2, 12, 1, 4, "%d seconds");
}

/* ---- Date & Time ---- */

static int zone_top; /* The first zone in view. */
#define ZONE_ROWS 11

static void draw_date(void) {
    int y = 64;
    struct vx_date d;
    vx_local_now(&d);
    char line[64];
    int hours = vx_settings_int(&desk, "clock", 24);
    if (hours == 12) {
        snprintf(line, sizeof(line), "%d:%02d:%02d %s", d.hour % 12 ? d.hour % 12 : 12, d.minute,
                 d.second, d.hour < 12 ? "am" : "pm");
    } else {
        snprintf(line, sizeof(line), "%02d:%02d:%02d", d.hour, d.minute, d.second);
    }
    big_text(LEFT, y, line, 40, VX_COLOR_TEXT);
    snprintf(line, sizeof(line), "%s %d %s %d", vx_weekday_names[d.weekday], d.day,
             vx_month_names[d.month - 1], d.year);
    text(LEFT, y + 56, line, VX_COLOR_DIM);
    y += 88;
    static const struct option clock_hours[] = {{"24", "24-hour"}, {"12", "12-hour"}};
    segments(y, "Clock", "clock", clock_hours, 2, "24");
    y += ROW + 6;
    heading("Time zone", y);
    y += 28;
    const char *zone = vx_settings_get(&desk, "time_zone", "");
    int list_w = window->surface.width - LEFT - 24;
    vx_fill_rounded(S(), LEFT, y, list_w, ZONE_ROWS * 22 + 4, 6, VX_COLOR_LINE, 255);
    vx_fill_rounded(S(), LEFT + 1, y + 1, list_w - 2, ZONE_ROWS * 22 + 2, 5, VX_COLOR_VIEW, 255);
    long now = vx_time();
    for (int row = 0; row < ZONE_ROWS; row++) {
        int i = zone_top + row - 1; /* -1: UTC, no zone. */
        if (i >= vx_zone_count) {
            break;
        }
        int ry = y + 2 + row * 22;
        const char *city = i < 0 ? "UTC" : vx_zones[i].city;
        bool on = i < 0 ? !zone[0] || !strcmp(zone, "UTC") : !strcmp(zone, city);
        if (on) {
            vx_draw_selection(S(), LEFT + 3, ry, list_w - 6, 22);
        }
        int offset = i < 0 ? 0 : vx_zone_offset(&vx_zones[i], now);
        int a = offset < 0 ? -offset : offset;
        char utc[16];
        snprintf(utc, sizeof(utc), "UTC%c%02d:%02d", offset < 0 ? '-' : '+', a / 60, a % 60);
        text(LEFT + 10, ry + 3, utc, VX_COLOR_DIM);
        text(LEFT + 120, ry + 3, city, VX_COLOR_TEXT);
        if (i >= 0) {
            text(LEFT + 300, ry + 3, vx_zones[i].region, VX_COLOR_DIM);
            if (vx_zones[i].dst != VX_DST_NONE) {
                text(LEFT + 420, ry + 3, offset != vx_zones[i].offset ? "summer time now"
                                                                     : "has summer time",
                     VX_COLOR_DIM);
            }
        }
        struct hit *h = add_hit(LEFT, ry, list_w, 22, H_ZONE);
        h->index = i;
    }
    note(y + ZONE_ROWS * 22 + 12, "Scroll for more cities. Summer time changes by itself.");
}

/* ---- Mouse & Keyboard ---- */

static long test_clicked_ms; /* The double click test: when it last worked. */

static void draw_input(void) {
    int y = 64;
    heading("Mouse", y);
    y += 28;
    slider(y, "Pointer speed", "pointer_speed", 1, 10, 1, 5, "%d");
    y += ROW;
    slider(y, "Double click speed", "double_click_ms", 200, 900, 50, 500, "%d ms");
    int tx = window->surface.width - 130;
    bool lit = vx_uptime() - test_clicked_ms < 800;
    vx_draw_button(S(), tx, y - 2, 100, 30, lit ? "It works!" : "Try it here", lit);
    add_hit(tx, y - 2, 100, 30, H_TEST_AREA);
    y += ROW;
    toggle(y, "Natural scrolling", "natural_scroll", false);
    y += ROW;
    toggle(y, "Left-handed (swap buttons)", "left_handed", false);
    y += ROW + 6;
    heading("Keyboard", y);
    y += 28;
    static const struct option layouts[] = {
        {"us", "US"}, {"gb", "UK"}, {"de", "German"}, {"fr", "French"}, {"es", "Spanish"},
        {"dvorak", "Dvorak"},
    };
    segments(y, "Layout", "keyboard_layout", layouts, 6, "us");
    y += ROW;
    slider(y, "Repeat after", "key_delay", 250, 1000, 250, 500, "%d ms");
    y += ROW;
    slider(y, "Repeat speed", "key_rate", 2, 30, 1, 20, "%d a second");
    y += ROW;
    note(y, "Vexa's apps type the layout's ASCII letters; X programs get all of it.");
    y += 26;
    heading("Shortcuts", y);
    y += 26;
    static const char *const shortcuts[][2] = {
        {"Alt+Tab", "the next window"}, {"Ctrl+Alt+Q", "back to the text console"},
    };
    int col = 0;
    for (int i = 0; i < app_count && y < window->surface.height - 30; i++) {
        if (!apps[i].shortcut[0]) {
            continue;
        }
        text(LEFT + col * 300, y, apps[i].shortcut, VX_COLOR_ACCENT);
        text(LEFT + col * 300 + 104, y, apps[i].name, VX_COLOR_TEXT);
        col = !col;
        y += col ? 0 : 20;
    }
    for (int i = 0; i < 2; i++) {
        text(LEFT + col * 300, y, shortcuts[i][0], VX_COLOR_ACCENT);
        text(LEFT + col * 300 + 104, y, shortcuts[i][1], VX_COLOR_TEXT);
        col = !col;
        y += col ? 0 : 20;
    }
}

/* ---- Display ---- */

static struct vx_display_modes modes;
static struct vx_display_info display_info;
static bool have_display;
/* After a change: what to go back to, and when. */
static long revert_at;
static int revert_width, revert_height, revert_scale;

static void read_display(void) {
    int handle = vx_open("/dev/display0", VX_OPEN_READ);
    have_display = handle >= 0 && vx_control(handle, VX_DISPLAY_MODES, &modes, sizeof(modes)) == 0 &&
                   vx_control(handle, VX_DISPLAY_INFO, &display_info, sizeof(display_info)) == 0;
    if (handle >= 0) {
        vx_close(handle);
    }
}

/* ---- Sound ---- */

static int audio_handle = -1;
static struct vx_audio_volume sound_volume;
static struct vx_audio_outputs sound_outputs;

static bool read_sound(void) {
    if (audio_handle < 0) {
        audio_handle = vx_open("/dev/audio0", VX_OPEN_READ);
    }
    return audio_handle >= 0 &&
           vx_control(audio_handle, VX_AUDIO_GET_VOLUME, &sound_volume, sizeof(sound_volume)) == 0 &&
           vx_control(audio_handle, VX_AUDIO_OUTPUTS, &sound_outputs, sizeof(sound_outputs)) == 0;
}

static void apply_volume(void) {
    if (read_sound()) {
        sound_volume.volume = (unsigned)vx_settings_int(&desk, "volume", 80);
        sound_volume.muted = vx_settings_bool(&desk, "muted", false);
        vx_control(audio_handle, VX_AUDIO_SET_VOLUME, &sound_volume, sizeof(sound_volume));
    }
}

static void play_chime(void) {
    const char *argv[] = {"play", "--chime"};
    struct vx_spawn spawn = {.argv = argv, .argc = 2, .handles = {0, 1, 2}};
    long child = vx_spawn("/bin/play", &spawn);
    if (child >= 0) {
        vx_close((int)child);
    }
}

static void draw_sound(void) {
    int y = 64;
    heading("Output", y);
    y += 28;
    if (!read_sound()) {
        note(y, "No sound device.");
        return;
    }
    int w = window->surface.width - LEFT - 24;
    for (unsigned i = 0; i < sound_outputs.count; i++) {
        bool on = sound_outputs.output[i].id == sound_outputs.current;
        if (on) {
            vx_draw_gel(S(), LEFT, y, w, 30, 8, VX_COLOR_ACCENT);
        } else {
            vx_fill_rounded(S(), LEFT, y, w, 30, 8, VX_COLOR_BUTTON, 255);
        }
        uint32_t color = on ? 0xffffff : VX_COLOR_TEXT;
        text(LEFT + 14, y + 7, sound_outputs.output[i].name, color);
        char rate[24];
        snprintf(rate, sizeof(rate), "%u Hz", sound_outputs.output[i].rate);
        text(LEFT + w - vx_text_width(rate) - 14, y + 7, rate, on ? 0xffffff : VX_COLOR_DIM);
        struct hit *h = add_hit(LEFT, y, w, 30, H_OUTPUT);
        h->index = (int)sound_outputs.output[i].id;
        y += 36;
    }
    y += 12;
    heading("Volume", y);
    y += 28;
    /* (What the sound core has now: the panel or the volume keys may have changed it.) */
    vx_settings_set_int(&desk, "volume", (int)sound_volume.volume);
    vx_settings_set_bool(&desk, "muted", sound_volume.muted != 0);
    slider(y, "Volume", "volume", 0, 100, 5, 80, "%d%%");
    y += ROW;
    toggle(y, "Mute", "muted", false);
    y += ROW;
    toggle(y, "A sound for notifications", "notification_sound", false);
    y += ROW + 8;
    vx_draw_button(S(), LEFT, y, 170, 28, "Play a test sound", false);
    add_hit(LEFT, y, 170, 28, H_TEST_SOUND);
    y += 48;
    note(y, "Programs play at the same time, mixed together. A USB sound card or headset");
    note(y + 18, "plays as soon as it's plugged in; choose the output here, or with the speaker");
    note(y + 36, "on the panel, which also has the volume (and the volume keys change it).");
}

static void draw_display(void) {
    int y = 64;
    read_display();
    if (revert_at) {
        long left = (revert_at - vx_uptime() + 999) / 1000;
        rounded(LEFT, y, window->surface.width - LEFT - 24, 48, VX_COLOR_SELECTED);
        char line[96];
        snprintf(line, sizeof(line), "Keep this? It goes back in %ld second%s.", left,
                 left == 1 ? "" : "s");
        text(LEFT + 14, y + 16, line, VX_COLOR_TEXT);
        int bx = window->surface.width - 24 - 200;
        vx_draw_button(S(), bx, y + 11, 90, 26, "Keep", false);
        add_hit(bx, y + 11, 90, 26, H_KEEP);
        vx_draw_button(S(), bx + 98, y + 11, 90, 26, "Go back", false);
        add_hit(bx + 98, y + 11, 90, 26, H_REVERT);
        y += 64;
    }
    heading("Resolution", y);
    y += 28;
    if (!have_display) {
        note(y, "No display.");
        return;
    }
    int cols = 3, w = 170;
    for (unsigned i = 0; i < modes.count; i++) {
        int x = LEFT + (int)(i % cols) * (w + 8), ry = y + (int)(i / cols) * 32;
        bool on = modes.modes[i].width == display_info.width &&
                  modes.modes[i].height == display_info.height;
        char name[32];
        snprintf(name, sizeof(name), "%u x %u", modes.modes[i].width, modes.modes[i].height);
        vx_draw_button(S(), x, ry, w, 26, name, on);
        struct hit *h = add_hit(x, ry, w, 26, H_MODE);
        h->index = (int)i;
    }
    y += (int)((modes.count + cols - 1) / cols) * 32 + 8;
    if (modes.count <= 1) {
        note(y, "This display's size is the one the firmware set (Vexa can change it on QEMU's");
        note(y + 18, "and Bochs's standard VGA). For another, choose \"Vexa at 1024x768\" or");
        note(y + 36, "\"Vexa at 1280x720\" in the boot menu.");
        y += 62;
    }
    heading("Size of everything", y);
    y += 28;
    static const struct option scales[] = {{"1", "Normal"}, {"2", "Twice as big"}};
    segments(y, "Scale", "display_scale", scales, 2, "1");
    y += ROW + 8;
    char line[128];
    int scale = vx_settings_int(&desk, "display_scale", 1) == 2 ? 2 : 1;
    snprintf(line, sizeof(line), "The display: %u x %u, %u bits a pixel; the desktop works in %u x %u.",
             display_info.width, display_info.height, display_info.bits_per_pixel,
             display_info.width / scale, display_info.height / scale);
    note(y, line);
    note(y + 20, "X programs keep the size they started with until they're started again.");
}

/* ---- Default Apps ---- */

static const struct {
    const char *extension, *kind;
} file_kinds[] = {
    {"png", "PNG picture"}, {"bmp", "BMP picture"}, {"ppm", "PPM picture"}, {"txt", "Text"},
    {"md", "Markdown text"}, {"conf", "Settings file"}, {"c", "C source"}, {"h", "C header"},
    {"py", "Python script"}, {"sh", "Shell script"}, {"log", "Log"},
};
#define FILE_KINDS (int)(sizeof(file_kinds) / sizeof(file_kinds[0]))

static void draw_defaults(void) {
    int y = 64;
    heading("Opens with", y);
    y += 28;
    for (int i = 0; i < FILE_KINDS; i++) {
        char dotted[16], path[32];
        snprintf(dotted, sizeof(dotted), ".%s", file_kinds[i].extension);
        text(LEFT, y + 5, dotted, VX_COLOR_ACCENT);
        text(LEFT + 70, y + 5, file_kinds[i].kind, VX_COLOR_TEXT);
        snprintf(path, sizeof(path), "file.%s", file_kinds[i].extension);
        struct vx_app app;
        const char *name = vx_app_for_file(path, &app) == 0 ? app.name : "(none)";
        int x = CONTROL + 40, w = 200;
        vx_draw_button(S(), x, y + 1, w, 26, name, false);
        text(x + w - 16, y + 6, "v", VX_COLOR_DIM);
        struct hit *h = add_hit(x, y + 1, w, 26, H_DEFAULT_APP);
        h->index = i;
        y += 32;
    }
    note(y + 6, "Files, `open` and double clicks use these.");
    note(y + 26, "Apps say in their Info.conf what they can open.");
}

/* ---- Startup ---- */

static void draw_startup(void) {
    int y = 64;
    heading("When the desktop starts", y);
    y += 28;
    toggle(y, "Open a terminal", "startup_terminal", false);
    y += ROW + 6;
    heading("Also open", y);
    y += 28;
    const char *chosen = vx_settings_get(&desk, "startup_apps", "");
    for (int i = 0; i < app_count; i++) {
        char stem[64];
        bundle_stem(&apps[i], stem, sizeof(stem));
        int x = LEFT + (i % 2) * 280, ry = y + (i / 2) * 30;
        checkbox(x, ry, list_has(chosen, stem), apps[i].name);
        struct hit *h = add_hit(x, ry, 260, 26, H_STARTUP_APP);
        h->index = i;
    }
}

/* ---- Accounts: `accounts` (set-user-id root) makes the changes ---- */

/* Runs a program with `input` on its input; its first line of output (or
 * errors) goes to account_message. Returns its exit code. */
static long run_helper(const char *const *argv, int argc, const char *input) {
    int in[2], out[2];
    if (vx_pipe(in)) {
        return -1;
    }
    if (vx_pipe(out)) {
        vx_close(in[0]);
        vx_close(in[1]);
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), "/bin/%s", argv[0]);
    struct vx_spawn spawn = {.argv = argv, .argc = (unsigned long)argc,
                             .handles = {in[0], out[1], out[1]}};
    int child = vx_spawn(path, &spawn);
    vx_close(in[0]);
    vx_close(out[1]);
    if (child >= 0 && input) {
        vx_write(in[1], input, strlen(input));
    }
    vx_close(in[1]);
    char text[512];
    long n = 0, got;
    while (n < (long)sizeof(text) - 1 && (got = vx_read(out[0], text + n, sizeof(text) - 1 - (size_t)n)) > 0) {
        n += got;
    }
    vx_close(out[0]);
    text[n] = '\0';
    text[strcspn(text, "\n")] = '\0';
    const char *shown = !strncmp(text, "accounts: ", 10) ? text + 10 : text;
    snprintf(account_message, sizeof(account_message), "%s", shown);
    long code = child >= 0 ? vx_wait(child, 0) : child;
    if (child >= 0) {
        vx_close(child);
    }
    printf("settings: %s: %s (%ld)\n", argv[0], shown, code);
    fflush(stdout);
    return code;
}

static struct vx_user me_user;
static bool me_admin, me_has_password;

static void read_me(void) {
    if (vx_current_user(&me_user)) {
        memset(&me_user, 0, sizeof(me_user));
        snprintf(me_user.name, sizeof(me_user.name), "root");
    }
    me_admin = me_user.uid == 0 || vx_user_is_admin(me_user.name);
    me_has_password = !vx_password_check(me_user.name, "");
}

/* ---- Lock Screen ---- */

static void save_password(bool remove) {
    field = FIELD_NONE;
    if (!remove && !password[0]) {
        return;
    }
    char input[160];
    snprintf(input, sizeof(input), "%s%s%s\n", me_has_password ? old_password : "",
             me_has_password ? "\n" : "", remove ? "" : password);
    const char *argv[] = {"accounts", "password", "--password-stdin"};
    long code = run_helper(argv, 3, input);
    memset(input, 0, sizeof(input));
    memset(password, 0, sizeof(password));
    memset(old_password, 0, sizeof(old_password));
    changed("password", code == 0 ? (remove ? "removed" : "changed") : "not changed");
    read_me();
    vx_desktop_reload(); /* (The lock screen asks for it, or not.) */
}

static void dots_field(int x, int y, int width, const char *secret, bool active) {
    char dots[200] = "";
    size_t n = 0;
    for (const char *p = secret; *p && n + 4 < sizeof(dots);) {
        vx_utf8_next(&p);
        n += (size_t)snprintf(dots + n, sizeof(dots) - n, "\xe2\x80\xa2"); /* A dot. */
    }
    vx_draw_field(S(), x, y, width, dots, active);
}

static void draw_lock(void) {
    int y = 64;
    heading("Screensaver", y);
    y += 28;
    static const struct option minutes[] = {
        {"0", "Never"}, {"1", "1"}, {"2", "2"}, {"5", "5"}, {"10", "10"}, {"15", "15"}, {"30", "30"},
    };
    segments(y, "Start after (minutes)", "screensaver_minutes", minutes, 7, "10");
    y += ROW;
    toggle(y, "Lock when it ends", "lock_on_wake", false);
    y += ROW;
    note(y, "The time drifts over the wallpaper, blurred; a key or the mouse wakes it.");
    y += 34;
    heading("Your password", y);
    y += 28;
    if (me_has_password) {
        label(y, "Current password");
        dots_field(CONTROL, y + 1, 220, old_password, field == FIELD_OLD_PASSWORD);
        add_hit(CONTROL, y + 1, 220, 24, H_OLD_PASSWORD_FIELD);
        y += ROW;
    }
    label(y, me_has_password ? "New password" : "Password");
    dots_field(CONTROL, y + 1, 220, password, field == FIELD_PASSWORD);
    add_hit(CONTROL, y + 1, 220, 24, H_PASSWORD_FIELD);
    vx_draw_button(S(), CONTROL + 228, y + 1, 60, 24, "Set", false);
    add_hit(CONTROL + 228, y + 1, 60, 24, H_SET_PASSWORD);
    if (me_has_password) {
        vx_draw_button(S(), CONTROL + 296, y + 1, 80, 24, "Remove", false);
        add_hit(CONTROL + 296, y + 1, 80, 24, H_REMOVE_PASSWORD);
    }
    y += ROW;
    note(y, account_message[0] ? account_message
            : me_has_password  ? "Your account's password: logging in and the lock screen ask for it."
                               : "No password: anyone can log in as you, and any key unlocks.");
    y += 34;
    heading("Lock now", y);
    y += 28;
    vx_draw_button(S(), LEFT, y, 140, 28, "Lock Screen", false);
    add_hit(LEFT, y, 140, 28, H_LOCK_NOW);
    y += 38;
    note(y, "Anywhere: Super+L or Ctrl+Alt+L, or Lock Screen in the Vexa menu.");
}

/* ---- Users ---- */

static struct vx_user user_list[VX_USERS_MAX];
static int user_count;

static void add_account(void) {
    field = FIELD_NONE;
    if (!new_login[0]) {
        return;
    }
    const char *argv[7] = {"accounts", "add", new_login, "--full", new_full, "--password-stdin"};
    int argc = 6;
    if (new_admin) {
        argv[argc++] = "--admin";
    }
    char input[80];
    snprintf(input, sizeof(input), "%s\n", new_password);
    long code = run_helper(argv, argc, input);
    memset(input, 0, sizeof(input));
    memset(new_password, 0, sizeof(new_password));
    if (code == 0) {
        changed("account", new_login);
        new_login[0] = new_full[0] = '\0';
        new_admin = false;
    }
}

static void draw_users(void) {
    int y = 64;
    heading("Accounts", y);
    y += 28;
    user_count = vx_users(user_list, VX_USERS_MAX);
    for (int i = 0; i < user_count; i++) {
        const struct vx_user *u = &user_list[i];
        bool admin = vx_user_is_admin(u->name), self = u->uid == me_user.uid;
        vx_fill_rounded(S(), LEFT, y, 32, 32, 16, admin ? VX_COLOR_ACCENT : 0x8e8ea0, 255);
        char initial[2] = {u->full_name[0] ? u->full_name[0] : u->name[0], '\0'};
        if (initial[0] >= 'a' && initial[0] <= 'z') {
            initial[0] = (char)(initial[0] - 32);
        }
        text(LEFT + 16 - vx_text_width(initial) / 2, y + 9, initial, 0xffffff);
        char line[160];
        snprintf(line, sizeof(line), "%s%s", u->full_name[0] ? u->full_name : u->name,
                 self ? " (you)" : "");
        text(LEFT + 44, y + 1, line, VX_COLOR_TEXT);
        snprintf(line, sizeof(line), "%s - %s", u->name, admin ? "Administrator" : "Standard");
        text(LEFT + 44, y + 17, line, VX_COLOR_DIM);
        if (me_admin && !self) {
            int bx = window->surface.width - 24 - 90;
            vx_draw_button(S(), bx, y + 4, 90, 24, "Remove", false);
            struct hit *h = add_hit(bx, y + 4, 90, 24, H_REMOVE_ACCOUNT);
            h->index = i;
            bx -= 156;
            vx_draw_button(S(), bx, y + 4, 148, 24, admin ? "Make Standard" : "Make Administrator",
                           false);
            h = add_hit(bx, y + 4, 148, 24, H_ADMIN_ACCOUNT);
            h->index = i;
        }
        y += 42;
    }
    y += 10;
    heading("Add an account", y);
    y += 28;
    if (!me_admin) {
        note(y, "Only administrators can add or remove accounts.");
        return;
    }
    label(y, "Full name");
    vx_draw_field(S(), CONTROL, y + 1, 220, new_full, field == FIELD_NEW_FULL);
    add_hit(CONTROL, y + 1, 220, 24, H_NEW_FULL_FIELD);
    y += ROW;
    label(y, "Account name");
    vx_draw_field(S(), CONTROL, y + 1, 220, new_login, field == FIELD_NEW_LOGIN);
    add_hit(CONTROL, y + 1, 220, 24, H_NEW_LOGIN_FIELD);
    y += ROW;
    label(y, "Password");
    dots_field(CONTROL, y + 1, 220, new_password, field == FIELD_NEW_PASSWORD);
    add_hit(CONTROL, y + 1, 220, 24, H_NEW_PASSWORD_FIELD);
    y += ROW;
    checkbox(CONTROL, y, new_admin, "Administrator (can change the system)");
    add_hit(CONTROL, y, 300, 26, H_NEW_ADMIN);
    y += ROW;
    vx_draw_button(S(), CONTROL, y, 120, 28, "Add Account", false);
    add_hit(CONTROL, y, 120, 28, H_ADD_ACCOUNT);
    y += 38;
    note(y, account_message[0] ? account_message
                               : "Account names: small letters, digits, - and _. Each gets a home "
                                 "folder in /home.");
}

/* ---- Network ---- */

static void draw_network(void) {
    int y = 64;
    heading("This computer", y);
    y += 28;
    label(y, "Name");
    if (field != FIELD_NAME) {
        vx_get_hostname(computer_name, sizeof(computer_name));
    }
    vx_draw_field(S(), CONTROL, y + 1, 220, computer_name, field == FIELD_NAME);
    add_hit(CONTROL, y + 1, 220, 24, H_NAME_FIELD);
    vx_draw_button(S(), CONTROL + 228, y + 1, 70, 24, "Save", false);
    add_hit(CONTROL + 228, y + 1, 70, 24, H_SAVE_NAME);
    y += ROW;
    note(y, "Letters, digits, '-' and '.'. Linux programs see it too (uname -n).");
    y += 30;
    struct vx_net_interface list[8];
    long n = vx_net_info(list, 8);
    for (long i = 0; i < n && i < 8; i++) {
        struct vx_net_interface *nif = &list[i];
        char title[64], line[128], a[16], b[16], c[16];
        snprintf(title, sizeof(title), "Interface %s", nif->name);
        heading(title, y);
        y += 26;
        vx_format_ipv4(nif->address, a);
        vx_format_ipv4(nif->netmask, b);
        snprintf(line, sizeof(line), "%s / %s", nif->address ? a : "(none)", b);
        label(y, "Address");
        text(CONTROL, y + 5, line, VX_COLOR_TEXT);
        y += 22;
        if (nif->gateway || nif->dns) {
            vx_format_ipv4(nif->gateway, a);
            vx_format_ipv4(nif->dns, c);
            snprintf(line, sizeof(line), "router %s, DNS %s", nif->gateway ? a : "-", nif->dns ? c : "-");
            label(y, "Network");
            text(CONTROL, y + 5, line, VX_COLOR_TEXT);
            y += 22;
        }
        if (nif->mac[0] | nif->mac[1] | nif->mac[2] | nif->mac[3] | nif->mac[4] | nif->mac[5]) {
            snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x", nif->mac[0], nif->mac[1],
                     nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5]);
            label(y, "Hardware");
            text(CONTROL, y + 5, line, VX_COLOR_TEXT);
            y += 22;
        }
        char in[24], out[24];
        format_bytes(in, sizeof(in), nif->rx_bytes);
        format_bytes(out, sizeof(out), nif->tx_bytes);
        snprintf(line, sizeof(line), "%s in, %s out", in, out);
        label(y, "Traffic");
        text(CONTROL, y + 5, line, VX_COLOR_TEXT);
        y += 30;
    }
}

/* ---- Storage ---- */

static void draw_storage(void) {
    int y = 64;
    heading("Disks and file systems", y);
    y += 30;
    struct vx_mount_info mounts[16];
    long n = vx_mounts(mounts, 16);
    int w = window->surface.width - LEFT - 24;
    for (long i = 0; i < n && i < 16; i++) {
        struct vx_mount_info *m = &mounts[i];
        if (!m->total) {
            continue;
        }
        char line[160], total[24], free[24];
        format_bytes(total, sizeof(total), m->total);
        format_bytes(free, sizeof(free), m->free);
        snprintf(line, sizeof(line), "%s  (%s, %s%s)", m->path, m->type, m->source,
                 m->read_only ? ", read-only" : "");
        text(LEFT, y, line, VX_COLOR_TEXT);
        snprintf(line, sizeof(line), "%s free of %s", free, total);
        text(LEFT + w - vx_text_width(line), y, line, VX_COLOR_DIM);
        bar(LEFT, y + 22, w, m->total - m->free, m->total);
        y += 48;
    }
    y += 6;
    heading("Settings", y);
    y += 28;
    char disk[128], line[200];
    if (vx_settings_disk(disk, sizeof(disk))) {
        snprintf(line, sizeof(line), "Settings are kept on the disk at %s (in %s/%s).", disk, disk,
                 VX_SETTINGS_DIR);
    } else {
        snprintf(line, sizeof(line), "No disk to keep settings on: they last until Vexa restarts.");
    }
    note(y, line);
    note(y + 20, "The first writable ext2 disk keeps them; vinit puts them back at boot.");
}

/* ---- About ---- */

static void draw_about(void) {
    int y = 60;
    struct vx_system_info info;
    if (vx_system_info(&info)) {
        memset(&info, 0, sizeof(info));
    }
    int end = big_text(LEFT, y, "Vexa", 52, VX_COLOR_ACCENT);
    char line[128];
    snprintf(line, sizeof(line), "version %s", info.version);
    text(end + 16, y + 40, line, VX_COLOR_DIM);
    y += 88;
    char name[80] = "";
    vx_get_hostname(name, sizeof(name));
    label(y, "Computer name");
    text(CONTROL, y + 5, name, VX_COLOR_TEXT);
    y += 26;
    snprintf(line, sizeof(line), "%u", info.cpus);
    label(y, "CPUs");
    text(CONTROL, y + 5, line, VX_COLOR_TEXT);
    y += 26;
    char used[24], total[24];
    format_bytes(used, sizeof(used), info.memory_total - info.memory_free);
    format_bytes(total, sizeof(total), info.memory_total);
    snprintf(line, sizeof(line), "%s of %s used", used, total);
    label(y, "Memory");
    text(CONTROL, y + 5, line, VX_COLOR_TEXT);
    bar(CONTROL, y + 26, 240, info.memory_total - info.memory_free, info.memory_total);
    y += 44;
    unsigned long long s = info.uptime_ms / 1000;
    snprintf(line, sizeof(line), "%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    label(y, "Up for");
    text(CONTROL, y + 5, line, VX_COLOR_TEXT);
    y += 26;
    read_display();
    if (have_display) {
        snprintf(line, sizeof(line), "%u x %u", display_info.width, display_info.height);
        label(y, "Display");
        text(CONTROL, y + 5, line, VX_COLOR_TEXT);
        y += 26;
    }
    struct vx_mount_info mounts[16];
    long n = vx_mounts(mounts, 16), disks = 0;
    for (long i = 0; i < n && i < 16; i++) {
        disks += mounts[i].path[0] == '/' && !strncmp(mounts[i].path, "/mnt/", 5);
    }
    snprintf(line, sizeof(line), "%ld mounted", disks);
    label(y, "Disks and CDs");
    text(CONTROL, y + 5, line, VX_COLOR_TEXT);
    y += 44;
    vx_draw_button(S(), LEFT, y, 120, 28, "Restart...", false);
    struct hit *h = add_hit(LEFT, y, 120, 28, H_POWER);
    h->index = VX_POWER_RESTART;
    vx_draw_button(S(), LEFT + 130, y, 120, 28, "Shut Down...", false);
    h = add_hit(LEFT + 130, y, 120, 28, H_POWER);
    h->index = VX_POWER_OFF;
    note(y + 44, "A hobby operating system, written from scratch.");
    note(y + 64, "github.com/EnderiumCraft/Vexa");
}

/* ---- The sidebar ---- */

static bool section_matches(int i) {
    if (!search[0]) {
        return true;
    }
    char hay[256];
    snprintf(hay, sizeof(hay), "%s %s", sections[i].label, sections[i].keywords);
    size_t n = strlen(search);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && (p[k] | 0x20) == (search[k] | 0x20)) {
            k++;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

/* Each section's icon, from Vexa's set (/share/icons). */
static const char *const section_icons[SECTION_COUNT] = {
    "palette", "pictures", "settings", "calendar", "usb",    "computer", "music-folder",
    "shield",  "control-panel", "power", "network", "drive", "help",     "users",
};

static struct vx_image *section_icon(int i) {
    static struct vx_image *icons[SECTION_COUNT];
    static bool tried[SECTION_COUNT];
    if (!tried[i]) {
        tried[i] = true;
        char path[96];
        snprintf(path, sizeof(path), "/share/icons/%s.png", section_icons[i]);
        icons[i] = vx_image_load(path, VX_IMAGE_ALPHA);
    }
    return icons[i];
}

static void draw_sidebar(void) {
    int h = window->surface.height;
    vx_fill(S(), 0, 0, SIDEBAR, h, vx_theme.sidebar);
    vx_fill(S(), SIDEBAR - 1, 0, 1, h, VX_COLOR_LINE);
    vx_draw_field(S(), 12, 14, SIDEBAR - 24, search, field == FIELD_SEARCH);
    if (!search[0] && field != FIELD_SEARCH) {
        text(18, 18, "Search", VX_COLOR_DIM);
    }
    add_hit(12, 14, SIDEBAR - 24, 24, H_SEARCH_FIELD);
    int y = 52;
    for (int i = 0; i < SECTION_COUNT; i++) {
        if (!section_matches(i)) {
            continue;
        }
        bool on = i == (int)current;
        if (on) {
            vx_draw_gel(S(), 8, y, SIDEBAR - 16, 30, 8, VX_COLOR_ACCENT);
        }
        struct vx_image *icon = section_icon(i);
        if (icon) {
            vx_blit_alpha(S(), 14, y + 3, 24, 24, &icon->surface);
        } else {
            vx_draw_gel(S(), 16, y + 5, 20, 20, 6, sections[i].color);
            char symbol[2] = {sections[i].symbol, 0};
            text(26 - vx_text_width(symbol) / 2, y + 7, symbol, 0xffffff);
        }
        text(46, y + 7, sections[i].label, on ? 0xffffff : VX_COLOR_TEXT);
        struct hit *hit = add_hit(8, y, SIDEBAR - 16, 30, H_SECTION);
        hit->index = i;
        y += 34;
    }
    if (y == 52) {
        text(20, y + 6, "Nothing found", VX_COLOR_DIM);
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    hit_count = 0;
    target = NULL;
    draw_sidebar();
    /* The page, on its own surface (made once), then the part of it showing. */
    if (!page.pixels || page.width != s->width) {
        free(page.pixels);
        page = (struct vx_surface){malloc((size_t)s->width * PAGE_HEIGHT * 4), s->width,
                                   PAGE_HEIGHT, s->width};
    }
    if (!page.pixels) {
        page.width = 0;
        return;
    }
    target = &page;
    page_bottom = 0;
    int shown = scroll + s->height < PAGE_HEIGHT ? scroll + s->height : PAGE_HEIGHT;
    vx_fill(&page, SIDEBAR, 0, page.width - SIDEBAR, shown, VX_COLOR_WINDOW);
    big_text(LEFT, 14, sections[current].label, 24, VX_COLOR_TEXT);
    switch (current) {
    case S_APPEARANCE: draw_appearance(); break;
    case S_WALLPAPER: draw_wallpaper(); break;
    case S_DESKTOP: draw_desktop(); break;
    case S_DATE: draw_date(); break;
    case S_INPUT: draw_input(); break;
    case S_DISPLAY: draw_display(); break;
    case S_SOUND: draw_sound(); break;
    case S_USERS: draw_users(); break;
    case S_LOCK: draw_lock(); break;
    case S_DEFAULTS: draw_defaults(); break;
    case S_STARTUP: draw_startup(); break;
    case S_NETWORK: draw_network(); break;
    case S_STORAGE: draw_storage(); break;
    case S_ABOUT: draw_about(); break;
    case SECTION_COUNT: break;
    }
    target = NULL;
    int most = page_bottom > s->height ? page_bottom - s->height + 16 : 0;
    if (scroll > most) { /* (It got shorter.) */
        scroll = most;
        draw();
        return;
    }
    vx_blit(s, SIDEBAR, 0, &page, SIDEBAR, scroll, s->width - SIDEBAR, s->height);
    if (most) { /* A thin bar: where in the page this is. */
        int total = most + s->height, bar = s->height * s->height / total;
        int y = (s->height - bar) * scroll / most;
        vx_fill_rounded(s, s->width - 7, y + 2, 4, bar - 4, 2, VX_COLOR_DIM, 140);
    }
    if (menu_open) {
        vx_draw_menu(s, menu_x, menu_y, menu, menu_count, menu_hot);
    }
    vx_window_present(window, 0, 0, s->width, s->height);
}

/* ---- Doing things ---- */

static void show_section(int i) {
    if (i != (int)current) {
        current = (enum section)i;
        scroll = 0;
        printf("settings: showing %s\n", sections[i].label);
        fflush(stdout);
    }
}

static void set_display(int width, int height, int scale) {
    /* What to go back to if it isn't kept. */
    if (!revert_at) {
        revert_width = vx_settings_int(&desk, "display_width", 0);
        revert_height = vx_settings_int(&desk, "display_height", 0);
        revert_scale = vx_settings_int(&desk, "display_scale", 1);
    }
    vx_settings_set_int(&desk, "display_width", width);
    vx_settings_set_int(&desk, "display_height", height);
    vx_settings_set_int(&desk, "display_scale", scale);
    vx_settings_save(&desk);
    char value[32];
    snprintf(value, sizeof(value), "%dx%d, scale %d", width, height, scale);
    changed("display", value);
    vx_desktop_reload();
    revert_at = vx_uptime() + REVERT_SECONDS * 1000;
}

static void revert_display(void) {
    revert_at = 0;
    vx_settings_set_int(&desk, "display_width", revert_width);
    vx_settings_set_int(&desk, "display_height", revert_height);
    vx_settings_set_int(&desk, "display_scale", revert_scale);
    vx_settings_save(&desk);
    changed("display", "back as it was");
    vx_desktop_reload();
}

static void save_name(void) {
    /* `hostname` (set-user-id root, for administrators) names it and keeps it. */
    const char *argv[] = {"hostname", computer_name};
    if (run_helper(argv, 2, NULL) != 0) {
        printf("settings: can't name the computer \"%s\": %s\n", computer_name, account_message);
        return;
    }
    changed("hostname", computer_name);
    field = FIELD_NONE;
}

static void use_picture(const char *path) {
    struct vx_stat stat;
    if (vx_stat(path, &stat) || stat.type != VX_TYPE_FILE) {
        printf("settings: no picture at %s\n", path);
        return;
    }
    vx_settings_set(&desk, "wallpaper_image", path);
    set("wallpaper", "image");
}

static void choose_default_app(int kind, int x, int y) {
    menu_count = 0;
    static char labels[MAX_APPS][64];
    for (int i = 0; i < app_count && menu_count < MAX_MENU; i++) {
        if (!apps[i].opens[0] || apps[i].is_linux) {
            continue;
        }
        snprintf(labels[menu_count], sizeof(labels[0]), "%s", apps[i].name);
        menu[menu_count] = (struct vx_menu_item){labels[menu_count], NULL, false};
        menu_values[menu_count++] = i;
    }
    menu_key = file_kinds[kind].extension;
    open_menu(x, y);
}

static void menu_chosen(int item) {
    menu_open = false;
    if (item < 0) {
        return;
    }
    if (!strcmp(menu_key, "wallpaper_mode")) {
        set("wallpaper_mode", wallpaper_modes[menu_values[item]].value);
        return;
    }
    if (!strcmp(menu_key, "power")) {
        if (menu_values[item]) {
            printf("settings: %s\n", menu_values[item] == VX_POWER_RESTART ? "restart" : "shut down");
            fflush(stdout);
            vx_power(menu_values[item]);
        }
        return;
    }
    struct vx_settings chosen;
    vx_settings_load(&chosen, VX_APP_DEFAULTS);
    char stem[64];
    bundle_stem(&apps[menu_values[item]], stem, sizeof(stem));
    vx_settings_set(&chosen, menu_key, stem);
    vx_settings_save(&chosen);
    char key[24];
    snprintf(key, sizeof(key), "open .%s", menu_key);
    changed(key, stem);
}

static void click(struct hit *h, int px) {
    field = FIELD_NONE;
    switch (h->kind) {
    case H_SECTION: show_section(h->index); break;
    case H_SEARCH_FIELD: field = FIELD_SEARCH; break;
    case H_SEGMENT:
        if (!strcmp(h->key, "display_scale")) {
            read_display();
            set_display((int)display_info.width, (int)display_info.height, atoi(h->value));
        } else {
            set(h->key, h->value);
        }
        break;
    case H_TOGGLE: set_bool(h->key, !vx_settings_bool(&desk, h->key, h->index)); break;
    case H_SLIDER: {
        int value = h->min + (int)((long)(px - h->x - 8) * (h->max - h->min) / (h->w - 16));
        value = h->min + (value - h->min + h->step / 2) / h->step * h->step;
        value = value < h->min ? h->min : value > h->max ? h->max : value;
        set_int(h->key, value);
        break;
    }
    case H_ACCENT: set("accent", vx_accents[h->index].name); break;
    case H_PICTURE: use_picture(pictures[h->index].path); break;
    case H_GO_WALLPAPER: show_section(S_WALLPAPER); break;
    case H_ADD_PICTURE: {
        char path[256];
        if (vx_open_dialog("Choose a Picture", vx_home_folder("Pictures"), path, sizeof(path))) {
            use_picture(path);
        }
        break;
    }
    case H_WALLPAPER_MODE:
        menu_count = 0;
        for (int i = 0; i < WALLPAPER_MODE_COUNT; i++) {
            menu[menu_count] = (struct vx_menu_item){wallpaper_modes[i].title, NULL, false};
            menu_values[menu_count++] = i;
        }
        menu_key = "wallpaper_mode";
        open_menu(h->x, h->y + h->h + 2);
        break;
    case H_GRADIENT: set("wallpaper", desktop_wallpapers[h->index].name); break;
    case H_PICTURE_FIELD:
        field = FIELD_PICTURE;
        snprintf(picture_path, sizeof(picture_path), "%s",
                 vx_settings_get(&desk, "wallpaper_image", ""));
        break;
    case H_USE_PICTURE:
        use_picture(picture_path[0] ? picture_path : vx_settings_get(&desk, "wallpaper_image", ""));
        break;
    case H_ICON_APP: {
        char list[256], stem[64];
        snprintf(list, sizeof(list), "%s", vx_settings_get(&desk, "desktop_apps", ""));
        if (!list[0]) { /* Start from what the apps ask for. */
            for (int i = 0; i < app_count; i++) {
                if (apps[i].desktop) {
                    bundle_stem(&apps[i], stem, sizeof(stem));
                    list_toggle(list, sizeof(list), stem);
                }
            }
        }
        bundle_stem(&apps[h->index], stem, sizeof(stem));
        list_toggle(list, sizeof(list), stem);
        set("desktop_apps", list[0] ? list : ",");
        break;
    }
    case H_STARTUP_APP: {
        char list[256], stem[64];
        snprintf(list, sizeof(list), "%s", vx_settings_get(&desk, "startup_apps", ""));
        bundle_stem(&apps[h->index], stem, sizeof(stem));
        list_toggle(list, sizeof(list), stem);
        set("startup_apps", list);
        break;
    }
    case H_ZONE: set("time_zone", h->index < 0 ? "UTC" : vx_zones[h->index].city); break;
    case H_MODE:
        set_display((int)modes.modes[h->index].width, (int)modes.modes[h->index].height,
                    vx_settings_int(&desk, "display_scale", 1));
        break;
    case H_KEEP:
        revert_at = 0;
        changed("display", "kept");
        break;
    case H_REVERT: revert_display(); break;
    case H_DEFAULT_APP: choose_default_app(h->index, h->x, h->y + h->h); break;
    case H_NAME_FIELD: field = FIELD_NAME; break;
    case H_PASSWORD_FIELD: field = FIELD_PASSWORD; break;
    case H_OLD_PASSWORD_FIELD: field = FIELD_OLD_PASSWORD; break;
    case H_SET_PASSWORD: save_password(false); break;
    case H_REMOVE_PASSWORD: save_password(true); break;
    case H_NEW_LOGIN_FIELD: field = FIELD_NEW_LOGIN; break;
    case H_NEW_FULL_FIELD: field = FIELD_NEW_FULL; break;
    case H_NEW_PASSWORD_FIELD: field = FIELD_NEW_PASSWORD; break;
    case H_NEW_ADMIN: new_admin = !new_admin; break;
    case H_ADD_ACCOUNT: add_account(); break;
    case H_REMOVE_ACCOUNT: {
        const char *argv[] = {"accounts", "remove", user_list[h->index].name};
        run_helper(argv, 3, NULL);
        break;
    }
    case H_ADMIN_ACCOUNT: {
        bool admin = vx_user_is_admin(user_list[h->index].name);
        const char *argv[] = {"accounts", "admin", user_list[h->index].name, admin ? "no" : "yes"};
        run_helper(argv, 4, NULL);
        break;
    }
    case H_OUTPUT: {
        unsigned id = (unsigned)h->index;
        if (read_sound() && vx_control(audio_handle, VX_AUDIO_SET_OUTPUT, &id, sizeof(id)) == 0) {
            changed("sound_output", "chosen");
        }
        break;
    }
    case H_TEST_SOUND:
        changed("sound", "test");
        play_chime();
        break;
    case H_LOCK_NOW:
        changed("lock", "now");
        vx_desktop_lock();
        break;
    case H_SAVE_NAME: save_name(); break;
    case H_POWER:
        menu_count = 0;
        menu[menu_count] = (struct vx_menu_item){h->index == VX_POWER_RESTART ? "Restart Now"
                                                                              : "Shut Down Now",
                                                 NULL, false};
        menu_values[menu_count++] = h->index;
        menu[menu_count] = (struct vx_menu_item){NULL, NULL, false};
        menu_values[menu_count++] = 0;
        menu[menu_count] = (struct vx_menu_item){"Cancel", NULL, false};
        menu_values[menu_count++] = 0;
        menu_key = "power";
        open_menu(h->x, h->y + h->h);
        break;
    case H_TEST_AREA: {
        static long last;
        long now = vx_uptime();
        if (now - last < vx_settings_int(&desk, "double_click_ms", 500)) {
            test_clicked_ms = now;
            last = 0;
        } else {
            last = now;
        }
        break;
    }
    }
}

static void key(const struct vx_gui_event *e) {
    if (!e->value) {
        return;
    }
    if (menu_open) {
        if (e->key == VX_KEY_ESC) {
            menu_open = false;
        }
        return;
    }
    switch (field) {
    case FIELD_SEARCH:
        if (e->key == VX_KEY_ESC) {
            search[0] = '\0';
            field = FIELD_NONE;
        } else if (e->key == VX_KEY_ENTER) {
            field = FIELD_NONE;
        } else if (vx_field_key(search, sizeof(search), e)) {
            for (int i = 0; i < SECTION_COUNT; i++) {
                if (section_matches(i)) {
                    show_section(i); /* The first that matches. */
                    break;
                }
            }
        }
        return;
    case FIELD_PICTURE:
        if (e->key == VX_KEY_ENTER) {
            field = FIELD_NONE;
            use_picture(picture_path);
        } else if (e->key == VX_KEY_ESC) {
            field = FIELD_NONE;
        } else {
            vx_field_key(picture_path, sizeof(picture_path), e);
        }
        return;
    case FIELD_PASSWORD:
    case FIELD_OLD_PASSWORD:
        if (e->key == VX_KEY_ENTER) {
            save_password(false);
        } else if (e->key == VX_KEY_TAB && field == FIELD_OLD_PASSWORD) {
            field = FIELD_PASSWORD;
        } else if (e->key == VX_KEY_ESC) {
            memset(password, 0, sizeof(password));
            memset(old_password, 0, sizeof(old_password));
            field = FIELD_NONE;
        } else if (field == FIELD_OLD_PASSWORD) {
            vx_field_key(old_password, sizeof(old_password), e);
        } else {
            vx_field_key(password, sizeof(password), e);
        }
        return;
    case FIELD_NEW_LOGIN:
    case FIELD_NEW_FULL:
    case FIELD_NEW_PASSWORD: {
        char *buffer = field == FIELD_NEW_LOGIN ? new_login
                       : field == FIELD_NEW_FULL ? new_full
                                                 : new_password;
        size_t size = field == FIELD_NEW_LOGIN  ? sizeof(new_login)
                      : field == FIELD_NEW_FULL ? sizeof(new_full)
                                                : sizeof(new_password);
        if (e->key == VX_KEY_ENTER) {
            add_account();
        } else if (e->key == VX_KEY_TAB) {
            field = field == FIELD_NEW_FULL    ? FIELD_NEW_LOGIN
                    : field == FIELD_NEW_LOGIN ? FIELD_NEW_PASSWORD
                                               : FIELD_NEW_FULL;
        } else if (e->key == VX_KEY_ESC) {
            field = FIELD_NONE;
        } else {
            vx_field_key(buffer, size, e);
            if (field == FIELD_NEW_FULL && buffer[0]) {
                /* The account name follows the full name, until it's typed itself. */
                size_t n = 0;
                for (const char *c = new_full; *c && n + 1 < sizeof(new_login); c++) {
                    char low = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
                    if ((low >= 'a' && low <= 'z') || (low >= '0' && low <= '9' && n)) {
                        new_login[n++] = low;
                    } else if (*c == ' ') {
                        break;
                    }
                }
                new_login[n] = '\0';
            }
        }
        return;
    }
    case FIELD_NAME:
        if (e->key == VX_KEY_ENTER) {
            save_name();
        } else if (e->key == VX_KEY_ESC) {
            field = FIELD_NONE;
        } else {
            vx_field_key(computer_name, sizeof(computer_name), e);
        }
        return;
    case FIELD_NONE:
        break;
    }
    /* Up and Down go through the sections; typing searches. */
    if (e->key == VX_KEY_UP && current > 0) {
        show_section(current - 1);
    } else if (e->key == VX_KEY_DOWN && current + 1 < SECTION_COUNT) {
        show_section(current + 1);
    } else if (e->character > ' ' && e->character != 127) {
        field = FIELD_SEARCH;
        vx_field_key(search, sizeof(search), e);
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool pressed = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    pointer_x = e->x;
    pointer_y = e->y;
    if (menu_open) {
        menu_hot = vx_menu_item_at(menu, menu_count, menu_x, menu_y, e->x, e->y);
        if (pressed) {
            menu_chosen(menu_hot);
        }
        return;
    }
    if (e->wheel && page_bottom) { /* A long page scrolls. */
        scroll -= e->wheel * 48;
        scroll = scroll < 0 ? 0 : scroll;
    } else if (e->wheel && current == S_DATE) {
        zone_top -= e->wheel * 3;
        zone_top = zone_top < 0 ? 0 : zone_top > vx_zone_count + 1 - ZONE_ROWS
                                          ? vx_zone_count + 1 - ZONE_ROWS : zone_top;
    }
    if (!pressed) {
        return;
    }
    for (int i = hit_count - 1; i >= 0; i--) {
        if (vx_inside(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
            click(&hits[i], e->x);
            return;
        }
    }
    field = FIELD_NONE;
}

int main(int argc, char **argv) {
    window = vx_window_create_flags("Settings", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "settings: no desktop to open a window on\n");
        return 1;
    }
    vx_settings_load(&desk, "desktop.conf");
    read_me();
    app_count = vx_app_list(apps, MAX_APPS);
    find_pictures();
    read_display(); /* (The Wallpaper page shows pictures as on the screen.) */
    /* `settings Display` opens that section. */
    for (int i = 0; argc > 1 && i < SECTION_COUNT; i++) {
        if (!strncmp(sections[i].label, argv[1], strlen(argv[1]))) {
            current = (enum section)i;
        }
    }
    int held = 0;
    for (;;) {
        draw();
        /* The clock ticks, a display change counts down, and thumbnails are
         * made when nothing else is happening. */
        bool busy = thumbs_waiting(); /* (Thumbnails to make.) */
        long wait = busy ? 0 : (current == S_DATE || current == S_ABOUT || revert_at ||
                                current == S_INPUT || current == S_SOUND) ? 500 : -1;
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, wait);
        if (got < 0) {
            return 0;
        }
        if (revert_at && vx_uptime() >= revert_at) {
            revert_display();
        }
        if (got == 0) {
            if (busy) {
                make_next_thumbnail();
            }
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_THEME: vx_settings_load(&desk, "desktop.conf"); break;
        case VX_GUI_RESIZE:
            if (e.width >= 700 && e.height >= 480) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
