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
    S_APPEARANCE, S_WALLPAPER, S_DESKTOP, S_DATE, S_INPUT, S_DISPLAY, S_LOCK, S_DEFAULTS,
    S_STARTUP, S_NETWORK, S_STORAGE, S_ABOUT, SECTION_COUNT
};

static const struct {
    const char *label, *keywords;
    uint32_t color;
    char symbol;
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
    {"Lock Screen", "screensaver password lock idle security", 0x5a6acf, 'K'},
    {"Default Apps", "open with file types extensions", 0xff5fa2, 'O'},
    {"Startup", "login start apps terminal", 0xf0524f, 'L'},
    {"Network", "computer name hostname address ip dns router internet", 0x4c8dff, 'N'},
    {"Storage", "disks space free used mounts", 0x8e8ea0, 'H'},
    {"About", "version system memory cpu restart shut down power off", 0x9b6bff, 'i'},
};

static enum section current = S_APPEARANCE;
static char search[40];

/* ---- Fields (typing) ---- */

static enum { FIELD_NONE, FIELD_SEARCH, FIELD_PICTURE, FIELD_NAME, FIELD_PASSWORD } field;
static char picture_path[256], computer_name[80], password[64];

/* ---- What was drawn where: clicking finds it here ---- */

enum hit_kind {
    H_SECTION, H_SEGMENT, H_TOGGLE, H_SLIDER, H_ACCENT, H_PICTURE, H_GRADIENT, H_PICTURE_FIELD,
    H_USE_PICTURE, H_ICON_APP, H_STARTUP_APP, H_ZONE, H_MODE, H_DEFAULT_APP, H_NAME_FIELD,
    H_SAVE_NAME, H_POWER, H_KEEP, H_REVERT, H_SEARCH_FIELD, H_TEST_AREA, H_PASSWORD_FIELD,
    H_SET_PASSWORD, H_REMOVE_PASSWORD, H_LOCK_NOW,
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

static struct hit *add_hit(int x, int y, int w, int h, enum hit_kind kind) {
    if (hit_count == MAX_HITS) {
        static struct hit spare;
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

/* Sets a desktop setting, saves, and tells the desktop. */
static void set(const char *key, const char *value) {
    vx_settings_set(&desk, key, value);
    vx_settings_save(&desk);
    changed(key, value);
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
    return &window->surface;
}

static void text(int x, int y, const char *t, uint32_t color) {
    vx_draw_text(S(), x, y, t, color, VX_TRANSPARENT);
}

static int big_text(int x, int y, const char *t, int size, uint32_t color) {
    return vx_text(S(), vx_font(VX_FACE_BOLD, size), x, y, t, color, VX_TRANSPARENT);
}

static void rounded(int x, int y, int w, int h, uint32_t color) {
    vx_fill(S(), x + 2, y, w - 4, h, color);
    vx_fill(S(), x, y + 2, w, h - 4, color);
    vx_fill(S(), x + 1, y + 1, w - 2, h - 2, color);
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
    rounded(x, y + 3, 40, 20, on ? VX_COLOR_ACCENT : VX_COLOR_BUTTON_HOT);
    rounded(on ? x + 22 : x + 2, y + 5, 16, 16, 0xffffff);
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
        vx_fill(S(), x, y + 1, w, 26, on ? VX_COLOR_SELECTED : VX_COLOR_BUTTON);
        vx_draw_outline(S(), x, y + 1, w, 26, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
        text(x + 10, y + 6, options[i].title, VX_COLOR_TEXT);
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
    vx_fill(S(), x, y + 12, w, 4, VX_COLOR_BUTTON_HOT);
    int at = x + (int)((long)(value - min) * w / (max - min));
    vx_fill(S(), x, y + 12, at - x, 4, VX_COLOR_ACCENT);
    rounded(at - 7, y + 6, 14, 16, 0xffffff);
    vx_draw_outline(S(), at - 7, y + 6, 14, 16, VX_COLOR_LINE);
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
    vx_fill(S(), x, y + 3, 16, 16, on ? VX_COLOR_ACCENT : VX_COLOR_VIEW);
    vx_draw_outline(S(), x, y + 3, 16, 16, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
    if (on) { /* A tick. */
        for (int i = 0; i < 4; i++) {
            vx_fill(S(), x + 3 + i, y + 10 + i, 2, 2, 0xffffff);
        }
        for (int i = 0; i < 7; i++) {
            vx_fill(S(), x + 6 + i, y + 13 - i, 2, 2, 0xffffff);
        }
    }
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
    rounded(x, y, w, 10, VX_COLOR_BUTTON_HOT);
    if (total) {
        int filled = (int)((unsigned long long)w * used / total);
        if (filled > 2) {
            rounded(x, y, filled, 10, VX_COLOR_ACCENT);
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

/* ---- Appearance ---- */

static void draw_preview(int x, int y, const char *theme, bool on) {
    struct vx_theme t;
    vx_theme_make(&t, theme, vx_settings_get(&desk, "accent", "purple"));
    vx_draw_outline(S(), x - 3, y - 3, 146, 96, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
    if (on) {
        vx_draw_outline(S(), x - 2, y - 2, 144, 94, VX_COLOR_ACCENT);
    }
    vx_fill(S(), x, y, 140, 90, t.panel);
    vx_fill(S(), x + 14, y + 16, 112, 14, t.title_focused);
    vx_fill(S(), x + 14, y + 30, 112, 50, t.window);
    vx_fill(S(), x + 20, y + 38, 60, 6, t.text);
    vx_fill(S(), x + 20, y + 50, 90, 4, t.dim);
    vx_fill(S(), x + 20, y + 58, 80, 4, t.dim);
    vx_fill(S(), x + 84, y + 66, 36, 10, t.accent);
}

static void draw_appearance(void) {
    int y = 64;
    heading("Look", y);
    y += 32;
    const char *theme = vx_settings_get(&desk, "theme", "dark");
    static const char *const themes[] = {"dark", "light"};
    for (int i = 0; i < 2; i++) {
        int x = LEFT + i * 180;
        draw_preview(x, y, themes[i], !strcmp(theme, themes[i]));
        text(x + 50, y + 100, i ? "Light" : "Dark", VX_COLOR_TEXT);
        struct hit *h = add_hit(x - 3, y - 3, 146, 120, H_SEGMENT);
        h->key = "theme";
        h->value = themes[i];
    }
    y += 140;
    heading("Accent color", y);
    y += 32;
    const char *accent = vx_settings_get(&desk, "accent", "purple");
    for (int i = 0; i < vx_accent_count; i++) {
        int x = LEFT + i * 62;
        bool on = !strcmp(accent, vx_accents[i].name);
        if (on) {
            rounded(x - 3, y - 3, 38, 38, VX_COLOR_TEXT);
        }
        rounded(x, y, 32, 32, vx_accents[i].color);
        vx_draw_text_fit(S(), x - 10, y + 40, 56, vx_accents[i].label,
                         on ? VX_COLOR_TEXT : VX_COLOR_DIM, VX_TRANSPARENT);
        struct hit *h = add_hit(x - 3, y - 3, 38, 60, H_ACCENT);
        h->index = i;
    }
    y += 72;
    note(y, "Vexa's apps, the desktop's panel, menus and title bars follow these at once.");
    note(y + 20, "The terminal stays dark; its cursor takes the accent color.");
}

/* ---- Wallpaper ---- */

#define MAX_PICTURES 12
static struct picture {
    char path[256];
    char name[64];
    struct vx_image *thumb;
    bool tried;
} pictures[MAX_PICTURES];
static int picture_count;

static void find_pictures(void) {
    picture_count = 0;
    int handle = vx_open(PICTURES, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    struct vx_dir_entry entries[16];
    long n;
    while ((n = vx_read_dir(handle, entries, 16)) > 0) {
        for (long i = 0; i < n && picture_count < MAX_PICTURES; i++) {
            const char *dot = strrchr(entries[i].name, '.');
            if (entries[i].type != VX_TYPE_FILE || !dot ||
                (strcmp(dot, ".png") && strcmp(dot, ".bmp") && strcmp(dot, ".ppm"))) {
                continue;
            }
            struct picture *p = &pictures[picture_count++];
            memset(p, 0, sizeof(*p));
            snprintf(p->path, sizeof(p->path), "%s/%s", PICTURES, entries[i].name);
            snprintf(p->name, sizeof(p->name), "%.*s", (int)(dot - entries[i].name), entries[i].name);
        }
    }
    vx_close(handle);
}

/* A picture's thumbnail (made when Settings has nothing else to do). */
static bool make_next_thumbnail(void) {
    for (int i = 0; i < picture_count; i++) {
        struct picture *p = &pictures[i];
        if (p->tried) {
            continue;
        }
        p->tried = true;
        struct vx_image *full = vx_image_load(p->path, 0);
        if (full) {
            struct vx_image *thumb = malloc(sizeof(*thumb));
            uint32_t *pixels = thumb ? malloc(112 * 70 * 4) : NULL;
            if (pixels) {
                thumb->surface = (struct vx_surface){pixels, 112, 70, 112};
                /* Filling the thumbnail, as the wallpaper fills the screen. */
                int iw = full->surface.width, ih = full->surface.height;
                int w = 112, h = ih * 112 / iw;
                if (h < 70) {
                    h = 70;
                    w = iw * 70 / ih;
                }
                vx_blit_scaled(&thumb->surface, (112 - w) / 2, (70 - h) / 2, w, h, &full->surface);
                p->thumb = thumb;
            } else {
                free(thumb);
            }
            vx_image_free(full);
        }
        return true;
    }
    return false;
}

static void draw_wallpaper(void) {
    int y = 64;
    const char *wallpaper = vx_settings_get(&desk, "wallpaper", "image");
    const char *image = vx_settings_get(&desk, "wallpaper_image", DESKTOP_DEFAULT_WALLPAPER);
    heading("Pictures", y);
    y += 30;
    for (int i = 0; i < picture_count; i++) {
        int x = LEFT + (i % 4) * 128, py = y + (i / 4) * 98;
        bool on = !strcmp(wallpaper, "image") && !strcmp(image, pictures[i].path);
        vx_fill(S(), x - 3, py - 3, 118, 76, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
        if (pictures[i].thumb) {
            vx_blit(S(), x, py, &pictures[i].thumb->surface, 0, 0, 112, 70);
        } else {
            vx_fill(S(), x, py, 112, 70, VX_COLOR_BUTTON);
            text(x + 20, py + 27, "Loading...", VX_COLOR_DIM);
        }
        vx_draw_text_fit(S(), x, py + 76, 112, pictures[i].name, on ? VX_COLOR_TEXT : VX_COLOR_DIM,
                         VX_TRANSPARENT);
        struct hit *h = add_hit(x - 3, py - 3, 118, 96, H_PICTURE);
        h->index = i;
    }
    y += ((picture_count + 3) / 4) * 98 + 4;
    heading("Gradients", y);
    y += 30;
    for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
        int x = LEFT + i * 96;
        bool on = !strcmp(wallpaper, desktop_wallpapers[i].name);
        vx_fill(S(), x - 3, y - 3, 86, 56, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
        for (int row = 0; row < 50; row++) {
            vx_fill(S(), x, y + row, 80, 1,
                    vx_mix(desktop_wallpapers[i].top, desktop_wallpapers[i].bottom, row * 255 / 49));
        }
        text(x, y + 56, desktop_wallpapers[i].label, on ? VX_COLOR_TEXT : VX_COLOR_DIM);
        struct hit *h = add_hit(x - 3, y - 3, 86, 76, H_GRADIENT);
        h->index = i;
    }
    y += 84;
    static const struct option modes[] = {
        {"fill", "Fill"}, {"fit", "Fit"}, {"center", "Center"}, {"tile", "Tile"}, {"stretch", "Stretch"},
    };
    segments(y, "A picture", "wallpaper_mode", modes, 5, "fill");
    y += ROW + 4;
    label(y, "Another picture");
    int fw = window->surface.width - CONTROL - 24 - 70;
    vx_draw_field(S(), CONTROL, y + 1, fw, field == FIELD_PICTURE ? picture_path : image,
                  field == FIELD_PICTURE);
    add_hit(CONTROL, y + 1, fw, 24, H_PICTURE_FIELD);
    vx_draw_button(S(), CONTROL + fw + 8, y + 1, 62, 24, "Use", false);
    add_hit(CONTROL + fw + 8, y + 1, 62, 24, H_USE_PICTURE);
    note(y + 32, "A PNG, BMP or PPM file; type its path and press Enter.");
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
    vx_fill(S(), LEFT, y, list_w, ZONE_ROWS * 22 + 4, VX_COLOR_VIEW);
    vx_draw_outline(S(), LEFT, y, list_w, ZONE_ROWS * 22 + 4, VX_COLOR_LINE);
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
            vx_fill(S(), LEFT + 2, ry, list_w - 4, 22, VX_COLOR_SELECTED);
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
    rounded(tx, y - 2, 100, 30, lit ? VX_COLOR_ACCENT : VX_COLOR_BUTTON);
    text(tx + 10, y + 5, lit ? "It works!" : "Try it here", lit ? 0xffffff : VX_COLOR_DIM);
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
        vx_fill(S(), x, ry, w, 26, on ? VX_COLOR_SELECTED : VX_COLOR_BUTTON);
        vx_draw_outline(S(), x, ry, w, 26, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
        char name[32];
        snprintf(name, sizeof(name), "%u x %u", modes.modes[i].width, modes.modes[i].height);
        text(x + 12, ry + 5, name, VX_COLOR_TEXT);
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
    toggle(y, "Open a terminal", "startup_terminal", true);
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

/* ---- Lock Screen ---- */

static void save_password(void) {
    field = FIELD_NONE;
    if (!password[0]) {
        return;
    }
    char hash[17];
    vx_password_hash(password, hash);
    memset(password, 0, sizeof(password));
    vx_settings_set(&desk, "lock_password", hash);
    vx_settings_save(&desk);
    changed("lock_password", "(set)");
    vx_desktop_reload();
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
    heading("Password", y);
    y += 28;
    bool set_already = vx_settings_get(&desk, "lock_password", "")[0];
    label(y, set_already ? "New password" : "Password");
    char dots[200] = "";
    size_t n = 0;
    for (const char *p = password; *p && n + 4 < sizeof(dots);) {
        vx_utf8_next(&p);
        n += (size_t)snprintf(dots + n, sizeof(dots) - n, "\xe2\x80\xa2"); /* A dot. */
    }
    vx_draw_field(S(), CONTROL, y + 1, 220, dots, field == FIELD_PASSWORD);
    add_hit(CONTROL, y + 1, 220, 24, H_PASSWORD_FIELD);
    vx_draw_button(S(), CONTROL + 228, y + 1, 60, 24, "Set", false);
    add_hit(CONTROL + 228, y + 1, 60, 24, H_SET_PASSWORD);
    if (set_already) {
        vx_draw_button(S(), CONTROL + 296, y + 1, 80, 24, "Remove", false);
        add_hit(CONTROL + 296, y + 1, 80, 24, H_REMOVE_PASSWORD);
    }
    y += ROW;
    note(y, set_already ? "A password is set: the lock screen asks for it."
                        : "No password: any key or click unlocks the lock screen.");
    y += 34;
    heading("Lock now", y);
    y += 28;
    vx_draw_button(S(), LEFT, y, 140, 28, "Lock Screen", false);
    add_hit(LEFT, y, 140, 28, H_LOCK_NOW);
    y += 38;
    note(y, "Anywhere: Super+L or Ctrl+Alt+L, or Lock Screen in the Vexa menu.");
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
        if (i == (int)current) {
            rounded(8, y, SIDEBAR - 16, 30, VX_COLOR_SELECTED);
        }
        rounded(16, y + 5, 20, 20, sections[i].color);
        char symbol[2] = {sections[i].symbol, 0};
        text(26 - vx_text_width(symbol) / 2, y + 7, symbol, 0xffffff);
        text(46, y + 7, sections[i].label, VX_COLOR_TEXT);
        struct hit *hit = add_hit(8, y, SIDEBAR - 16, 30, H_SECTION);
        hit->index = i;
        y += 34;
    }
    if (y == 52) {
        text(20, y + 6, "Nothing found", VX_COLOR_DIM);
    }
}

static void draw(void) {
    struct vx_surface *s = S();
    hit_count = 0;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
    draw_sidebar();
    big_text(LEFT, 14, sections[current].label, 24, VX_COLOR_TEXT);
    switch (current) {
    case S_APPEARANCE: draw_appearance(); break;
    case S_WALLPAPER: draw_wallpaper(); break;
    case S_DESKTOP: draw_desktop(); break;
    case S_DATE: draw_date(); break;
    case S_INPUT: draw_input(); break;
    case S_DISPLAY: draw_display(); break;
    case S_LOCK: draw_lock(); break;
    case S_DEFAULTS: draw_defaults(); break;
    case S_STARTUP: draw_startup(); break;
    case S_NETWORK: draw_network(); break;
    case S_STORAGE: draw_storage(); break;
    case S_ABOUT: draw_about(); break;
    case SECTION_COUNT: break;
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
    long error = vx_set_hostname(computer_name);
    if (error) {
        printf("settings: can't name the computer \"%s\": %s\n", computer_name, vx_strerror(error));
        return;
    }
    char text_line[96];
    int n = snprintf(text_line, sizeof(text_line), "%s\n", computer_name);
    vx_settings_write_file("hostname", text_line, (size_t)n);
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
    case H_SET_PASSWORD: save_password(); break;
    case H_REMOVE_PASSWORD:
        set("lock_password", "");
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
        if (e->key == VX_KEY_ENTER) {
            save_password();
        } else if (e->key == VX_KEY_ESC) {
            memset(password, 0, sizeof(password));
            field = FIELD_NONE;
        } else {
            vx_field_key(password, sizeof(password), e);
        }
        return;
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
    if (e->wheel && current == S_DATE) {
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
    app_count = vx_app_list(apps, MAX_APPS);
    find_pictures();
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
        bool busy = current == S_WALLPAPER && picture_count && !pictures[picture_count - 1].tried;
        long wait = busy ? 0 : (current == S_DATE || current == S_ABOUT || revert_at ||
                                current == S_INPUT) ? 500 : -1;
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
