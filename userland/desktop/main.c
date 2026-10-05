/* desktop: Vexa's graphical desktop (the compositor).
 *
 * It takes the screen, the keyboard and the mouse, and draws programs'
 * windows (buffers they share with it) with a title bar you can drag them
 * by, buttons to minimize, maximize and close them, and a soft shadow;
 * resizable windows are resized by their left, right and bottom edges.
 * Windows grow in when they open, fade when they close, and fly to the
 * panel when minimized. A panel along the top has the Vexa menu (programs
 * to start), a button for each window, search, and the clock (a calendar
 * and the notifications under it).
 *
 * Clicking a window raises it and gives it the keyboard. The keys:
 *
 *     Alt+Tab, Alt+Shift+Tab   switch windows (pictures of them while Alt is held)
 *     Alt+F4                   close the window
 *     Super+Left/Right/Up/Down snap to a half, a quarter, maximize, restore
 *     Super+D                  show the desktop (minimize all; again: back)
 *     Ctrl+Space, Super+Space  search for apps, settings and files
 *     Super+L, Ctrl+Alt+L      lock the screen
 *     PrintScreen              a screenshot (Alt: the window; Shift: an area)
 *     Ctrl+Alt+T, F, E, X      Terminal, Files, Text Editor, an xterm (apps' shortcuts)
 *     Ctrl+Alt+Q               back to the text console (it asks first)
 *
 * Programs talk to it over the local socket /run/desktop (see
 * <vexa/desktop.h>; <vexa/gui.h> has the easy way). The other parts are
 * in look.c, icons.c, switcher.c, search.c, clock.c, shot.c and lock.c
 * (shell.h says what they share).
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/desktop.h>
#include <vexa/files.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/time.h>
#include "shell.h"

#define MAX_CLIENTS 32
#define MAX_CHILDREN 32
#define GRIP 6          /* How far past a resizable window's edge you can grab it. */
#define EDGE 4          /* How far inside it. */
#define BUTTON_WIDTH 26 /* The title bar's buttons: their slots. */
#define BUTTON_SIZE 20  /* The buttons themselves, square. */
#define CORNER 8        /* The title bar's round corners. */
#define MIN_WIDTH 120
#define MIN_HEIGHT 60
#define DOUBLE_CLICK_MS setting_double_click
#define SEARCH_BUTTON_WIDTH 28

/* How long animations take (milliseconds). */
#define OPEN_MS 170
#define MINIMIZE_MS 220
#define CLOSE_MS 150

/* The theme (Settings, Appearance: <vexa/gui.h>). */
#define COLOR_PANEL (vx_theme.panel)
#define COLOR_PANEL_LINE (vx_theme.line)
#define COLOR_PANEL_TEXT (vx_theme.text)
#define COLOR_PANEL_DIM (vx_theme.dim)
#define COLOR_BUTTON (vx_theme.button)
#define COLOR_BUTTON_HOT (vx_theme.selected)
#define COLOR_BUTTON_OFF (vx_theme.menu)
#define COLOR_TITLE (vx_theme.title)
#define COLOR_TITLE_FOCUSED (vx_theme.title_focused)
#define COLOR_TITLE_TEXT (vx_theme.title_text)
#define COLOR_BORDER (vx_theme.dark ? 0x080808 : 0x8c8c9c)
#define COLOR_OUTLINE (vx_theme.accent)
#define COLOR_MENU (vx_theme.menu)
#define COLOR_MENU_HOT (vx_theme.selected)

/* ---- Settings (DESKTOP_CONFIG: Settings writes it, <vexa/settings.h>) ---- */

struct vx_settings config;
static char setting_wallpaper[32] = "image";
static char setting_image[256] = DESKTOP_DEFAULT_WALLPAPER;
static char setting_wallpaper_mode[16] = "fill"; /* fill, fit, center, tile, stretch */
static int setting_clock = 24;                   /* Hours. */
static bool setting_clock_seconds, setting_clock_date = true, setting_clock_weekday = true;
static int setting_note_ms = 4000;      /* How long notifications stay. */
static bool setting_snapping = true;
static bool setting_animations = true;
static char setting_title_click[16] = "maximize"; /* A double click on a title bar. */
static int setting_pointer_speed = 5;   /* 1 to 10; 5 moves as the mouse says. */
static int setting_double_click = 500;  /* Milliseconds between the clicks. */
static bool setting_natural_scroll, setting_left_handed;
char setting_layout[16] = "us";         /* The keyboard layout. */
static int setting_key_delay = 500, setting_key_rate = 20;
static int setting_width, setting_height; /* The display's mode (0: as it started). */
static int setting_scale = 1;           /* 2: everything twice as big. */

struct client {
    int handle;
};

static struct vx_display_info display;
static uint32_t *frame;             /* The display's memory. */
struct vx_surface screen;           /* Composed here, then copied to `frame`. */
struct vx_surface wallpaper;        /* Drawn once. */
struct window *stack[MAX_WINDOWS];  /* Bottom to top. */
int window_count;
static int next_window_id = 1;
struct window *focused;
static struct client clients[MAX_CLIENTS];
static int listener, keyboard = -1, mouse = -1;
static int children[MAX_CHILDREN];

int pointer_x, pointer_y, buttons;
static struct rect damage; /* What must be drawn again (width 0: nothing). */
static bool quit;
static bool logging_out; /* quit, and vinit starts the desktop again for someone else. */
/* Started from the CD with the Installer: only the wallpaper and the Installer,
 * in the middle (no panel, no icons), until it closes. */
static bool installer_only;
/* Who's logged in, and their folders (see shell.h). */
struct vx_user session_user;
bool session_has_password;
char home_folder[256] = "/home", desktop_folder[300] = "/home/Desktop";
char pictures_folder[300] = "/home/Pictures", trash_folder[300] = "/home/.Trash";
static int boot_argc;
static char **boot_argv;
static bool frames_wanted; /* An animation: draw again soon. */
static bool hide_cursor;   /* For screenshots. */

/* What the left button is doing. */
static enum { IDLE, MOVING, RESIZING } drag;
static struct window *dragged;
static int drag_dx, drag_dy, drag_right; /* drag_right: the right edge, resizing by the left. */
static bool resize_left, resize_right, resize_bottom;
static struct rect outline; /* The size a window is being resized to (content). */
static long last_click_ms;
static struct window *last_click_window;
/* The window a button was pressed in: it gets the pointer until the
 * buttons are up, even outside it (dragging out of it). */
static struct window *grab;
static bool desktop_press; /* The left button went down on the desktop. */

/* The pointer's shape, and the title bar button under it. */
static int cursor_shape;
static struct window *hot_window;
static int hot_button = -1;

static bool menu_open;

/* Notifications: up to three at a time, each for a few seconds. */
#define MAX_NOTES 3
#define NOTE_WIDTH 340
#define NOTE_HEIGHT 52
static struct note {
    char text[104];
    long until; /* vx_uptime() */
} notes[MAX_NOTES];
static int note_count;

/* Dragging a window to an edge or a corner snaps it there (see snap_target). */
static enum snap snap;
static int menu_hot = -1;
static long shown_minute = -1;

bool shift, ctrl, alt, super_key;
static bool caps_lock, altgr;

long now_ms(void) {
    return vx_uptime();
}

void want_frames(void) {
    frames_wanted = true;
}

/* ---- Rectangles and damage ---- */

struct rect frame_rect(const struct window *w) {
    if (w->popup || w->undecorated) {
        return (struct rect){w->x, w->y, w->content.width, w->content.height};
    }
    return (struct rect){w->x - BORDER, w->y - TITLE_HEIGHT - BORDER,
                         w->content.width + 2 * BORDER,
                         w->content.height + TITLE_HEIGHT + 2 * BORDER};
}

/* What a window covers, with its shadow. */
static struct rect window_damage(const struct window *w) {
    struct rect r = frame_rect(w);
    if (w->popup || w->undecorated) {
        return r;
    }
    return (struct rect){r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW,
                         r.height + 2 * SHADOW + 4};
}

/* Where clicks reach the window: its frame, and for a resizable one a little
 * past its left, right and bottom edges. */
static struct rect hit_rect(const struct window *w) {
    struct rect r = frame_rect(w);
    if (w->resizable && !w->maximized && !w->popup && !w->undecorated) {
        r.x -= GRIP;
        r.width += 2 * GRIP;
        r.height += GRIP;
    }
    return r;
}

bool inside(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

void add_damage(struct rect r) {
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

void damage_all(void) {
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

struct rect panel_rect(void) {
    return (struct rect){0, 0, screen.width, PANEL_HEIGHT};
}

/* The space windows get when maximized. */
struct rect work_area(void) {
    return (struct rect){BORDER, PANEL_HEIGHT + TITLE_HEIGHT + BORDER,
                         screen.width - 2 * BORDER,
                         screen.height - PANEL_HEIGHT - TITLE_HEIGHT - 2 * BORDER};
}

/* ---- The menu ---- */

enum menu_action {
    RUN_APP,       /* An app from /apps (see <vexa/app.h>). */
    RUN_LINUX_APP, /* An X program from /linux/usr/share/applications, through xrun. */
    SEPARATOR, LOCK, LOGOUT, LEAVE, RESTART, POWER_OFF,
    SUBMENU        /* A group of apps (a category), shown beside the menu. */
};

/* The apps in /apps, and their icons. */
struct vx_app apps[MAX_APPS];
struct vx_image *app_icons[MAX_APPS];
int app_count;

#define MAX_MENU_ITEMS 28
#define MAX_ARGS 4
#define APPLICATIONS "/linux/usr/share/applications"

static struct menu_item {
    char label[48];
    const char *keys;
    enum menu_action action;
    int app; /* RUN_APP: which. */
    int group; /* SUBMENU: which. */
    char args[MAX_ARGS][64]; /* RUN_LINUX_APP: the program and its arguments. */
    int arg_count;
} menu_items[MAX_MENU_ITEMS];
static int menu_item_count;
#define MENU_ITEMS menu_item_count
#define MENU_WIDTH 250
#define GROUP_WIDTH 240

/* The menu's groups of apps, by category (Info.conf's "category="), each a
 * submenu: Linux programs are the last group. */
static const char *const groups[] = {"Accessories", "Games", "Graphics", "Internet",
                                     "Multimedia", "System", "Other", "Linux"};
#define GROUPS 8
#define GROUP_LINUX 7
#define GROUP_OTHER 6
#define MAX_GROUP_ITEMS 16
static struct menu_item group_items[GROUPS][MAX_GROUP_ITEMS];
static int group_count[GROUPS];
static int menu_group = -1; /* The open submenu, or -1. */
static int group_hot = -1;
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

/* The top of the menu's item `index`. */
static int menu_item_top(int index) {
    int top = menu_rect().y + 4;
    for (int i = 0; i < index; i++) {
        top += menu_item_height(i);
    }
    return top;
}

/* The open submenu, beside its item in the menu. */
static struct rect group_rect(void) {
    struct rect r = menu_rect();
    int parent = 0;
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (menu_items[i].action == SUBMENU && menu_items[i].group == menu_group) {
            parent = i;
        }
    }
    int height = 8 + MENU_ITEM_HEIGHT * (menu_group >= 0 ? group_count[menu_group] : 0);
    int y = menu_item_top(parent) - 4;
    if (y + height > screen.height - 4) {
        y = screen.height - 4 - height;
    }
    return (struct rect){r.x + r.width - 4, y, GROUP_WIDTH, height};
}

/* The menu and wherever a submenu can be. */
static struct rect menu_damage(void) {
    struct rect r = menu_rect();
    return (struct rect){r.x - SHADOW, r.y, r.width + GROUP_WIDTH + 2 * SHADOW,
                         screen.height - r.y};
}

/* The open submenu's item at a point, or -1. */
static int group_item_at(int x, int y) {
    if (!menu_open || menu_group < 0) {
        return -1;
    }
    struct rect r = group_rect();
    if (!inside(r, x, y) || y < r.y + 4) {
        return -1;
    }
    int i = (y - r.y - 4) / MENU_ITEM_HEIGHT;
    return i < group_count[menu_group] ? i : -1;
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
        menu_group = -1;
        group_hot = -1;
        add_damage(menu_damage());
        add_damage(panel_rect());
    }
}

static struct menu_item *add_item(struct menu_item *list, int *count, int max, const char *label,
                                  const char *keys, enum menu_action action) {
    if (*count == max) {
        return NULL;
    }
    struct menu_item *item = &list[(*count)++];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->keys = keys;
    item->action = action;
    return item;
}

static struct menu_item *add_menu_item(const char *label, const char *keys,
                                       enum menu_action action) {
    return add_item(menu_items, &menu_item_count, MAX_MENU_ITEMS, label, keys, action);
}

static struct menu_item *add_group_item(int group, const char *label, const char *keys,
                                        enum menu_action action) {
    return add_item(group_items[group], &group_count[group], MAX_GROUP_ITEMS, label, keys, action);
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
    struct menu_item *item = add_group_item(GROUP_LINUX, name, "", RUN_LINUX_APP);
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
        group_count[GROUP_LINUX]--;
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

/* An app's group in the menu. */
static int app_group(const struct vx_app *app) {
    if (app->is_linux && !app->category[0]) {
        return GROUP_LINUX;
    }
    for (int g = 0; g < GROUP_OTHER; g++) {
        if (!strcmp(app->category, groups[g])) {
            return g;
        }
    }
    return app->is_linux ? GROUP_LINUX : GROUP_OTHER;
}

static void build_menu(void) {
    load_apps();
    menu_item_count = 0;
    memset(group_count, 0, sizeof(group_count));
    menu_group = group_hot = -1;
    /* The apps (in their menu order) in their groups. */
    for (int i = 0; i < app_count; i++) {
        if (apps[i].menu) {
            struct menu_item *item = add_group_item(app_group(&apps[i]), apps[i].name,
                                                    apps[i].shortcut, RUN_APP);
            if (item) {
                item->app = i;
            }
        }
    }
    /* Linux programs, by name. */
    int first = group_count[GROUP_LINUX];
    int handle = vx_open(APPLICATIONS, VX_OPEN_READ);
    if (handle >= 0) {
        struct vx_dir_entry entries[16];
        long n;
        while ((n = vx_read_dir(handle, entries, 16)) > 0) {
            for (long i = 0; i < n; i++) {
                size_t length = strlen(entries[i].name);
                if (length > 8 && !strcmp(entries[i].name + length - 8, ".desktop") &&
                    group_count[GROUP_LINUX] < MAX_GROUP_ITEMS) {
                    char path[320];
                    snprintf(path, sizeof(path), "%s/%s", APPLICATIONS, entries[i].name);
                    add_linux_app(path);
                }
            }
        }
        vx_close(handle);
    }
    qsort(group_items[GROUP_LINUX] + first, (size_t)(group_count[GROUP_LINUX] - first),
          sizeof(menu_items[0]), compare_labels);
    for (int g = 0; g < GROUPS; g++) {
        struct menu_item *item = group_count[g] ? add_menu_item(groups[g], "", SUBMENU) : NULL;
        if (item) {
            item->group = g;
        }
    }
    add_menu_item("", "", SEPARATOR);
    add_menu_item("Lock Screen", "Super+L", LOCK);
    if (session_user.uid != 0) {
        char label[80];
        snprintf(label, sizeof(label), "Log Out %s...",
                 session_user.full_name[0] ? session_user.full_name : session_user.name);
        add_menu_item(label, "", LOGOUT);
    }
    add_menu_item("Restart...", "", RESTART);
    add_menu_item("Shut Down...", "", POWER_OFF);
    add_menu_item("Back to the console...", "Ctrl+Alt+Q", LEAVE);
}

/* ---- The panel ---- */

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

bool local_date(struct vx_date *d) {
    long now = vx_time();
    if (now <= 0) {
        return false;
    }
    const struct vx_zone *zone = vx_find_zone(vx_settings_get(&config, "time_zone", NULL));
    int offset = zone ? vx_zone_offset(zone, now) : vx_settings_int(&config, "utc_offset", 0);
    vx_date_of(now + offset * 60L, d);
    return true;
}

/* The clock's text, in the time zone and format from the settings: false
 * if the time isn't known. */
bool clock_text(char *out, size_t size) {
    struct vx_date d;
    if (!local_date(&d)) {
        return false;
    }
    char date[24] = "", time[24];
    if (setting_clock_weekday && setting_clock_date) {
        snprintf(date, sizeof(date), "%.3s %d %.3s  ", vx_weekday_names[d.weekday], d.day,
                 vx_month_names[d.month - 1]);
    } else if (setting_clock_weekday) {
        snprintf(date, sizeof(date), "%.3s  ", vx_weekday_names[d.weekday]);
    } else if (setting_clock_date) {
        snprintf(date, sizeof(date), "%d %.3s  ", d.day, vx_month_names[d.month - 1]);
    }
    int hour = setting_clock == 12 ? (d.hour % 12 ? d.hour % 12 : 12) : d.hour;
    if (setting_clock_seconds) {
        snprintf(time, sizeof(time), setting_clock == 12 ? "%d:%02d:%02d" : "%02d:%02d:%02d", hour,
                 d.minute, d.second);
    } else {
        snprintf(time, sizeof(time), setting_clock == 12 ? "%d:%02d" : "%02d:%02d", hour, d.minute);
    }
    snprintf(out, size, "%s%s%s", date, time, setting_clock == 12 ? (d.hour < 12 ? " am" : " pm") : "");
    return true;
}

/* The clock on the panel (and the dot for notifications not seen yet). */
struct rect clock_button_rect(void) {
    char clock[48] = "";
    clock_text(clock, sizeof(clock));
    int width = vx_text_width(clock) + 16 + (unread_notes ? 12 : 0);
    return (struct rect){screen.width - width - 4, 2, width, PANEL_HEIGHT - 4};
}

/* ---- The keyboard layout on the panel ----
 *
 * Its button shows the layout ("US"); a click lists them all; Alt+Shift
 * (pressed together, then let go) goes back to the one before. */
static const char *const layout_names[][2] = {
    {"us", "English (US)"}, {"gb", "English (UK)"}, {"de", "German"},
    {"fr", "French"},       {"es", "Spanish"},      {"dvorak", "Dvorak"},
};
#define LAYOUT_NAMES ((int)(sizeof(layout_names) / sizeof(layout_names[0])))

static const char *layout_label(void) {
    static char label[4];
    label[0] = (char)toupper((unsigned char)setting_layout[0]);
    label[1] = (char)toupper((unsigned char)setting_layout[1]);
    label[2] = '\0';
    return label;
}

static struct rect layout_button_rect(void) {
    struct rect v = volume_button_rect();
    int width = vx_text_width(layout_label()) + 16;
    return (struct rect){v.x - width - 2, 2, width, PANEL_HEIGHT - 4};
}

static void set_layout(const char *name) {
    if (strcmp(name, setting_layout)) {
        vx_settings_set(&config, "keyboard_layout_previous", setting_layout);
        snprintf(setting_layout, sizeof(setting_layout), "%s", name);
        vx_settings_set(&config, "keyboard_layout", setting_layout);
        vx_settings_save(&config);
    }
    printf("desktop: keyboard layout %s\n", setting_layout);
    fflush(stdout);
    add_damage(panel_rect());
}

/* The layout before this one (or the next in the list). */
static void switch_layout(void) {
    char previous[16];
    snprintf(previous, sizeof(previous), "%s",
             vx_settings_get(&config, "keyboard_layout_previous", ""));
    if (previous[0] && strcmp(previous, setting_layout)) {
        set_layout(previous);
        return;
    }
    int i = 0;
    while (i < LAYOUT_NAMES && strcmp(layout_names[i][0], setting_layout)) {
        i++;
    }
    set_layout(layout_names[(i + 1) % LAYOUT_NAMES][0]);
}

static struct rect search_button_rect(void) {
    struct rect c = layout_button_rect();
    return (struct rect){c.x - SEARCH_BUTTON_WIDTH - 2, 2, SEARCH_BUTTON_WIDTH, PANEL_HEIGHT - 4};
}

static struct rect task_button(int index, int count) {
    int left = MENU_BUTTON_WIDTH + 12;
    int room = search_button_rect().x - left - 12;
    int width = count ? room / count - 4 : 0;
    if (width > 180) {
        width = 180;
    }
    return (struct rect){left + index * (width + 4), 3, width, PANEL_HEIGHT - 6};
}

/* ---- Drawing ---- */

static uint32_t mix(uint32_t a, uint32_t b, int num, int den) {
    return vx_mix(a, b, num * 255 / den);
}

/* Reads the settings (at start, and when Settings says they changed). */
static void read_config(void) {
    vx_settings_load(&config, "desktop.conf");
    vx_theme_load();
    /* The theme showing (the automatic one is light or dark by the time),
     * for Linux programs: xrun picks GTK's theme by it. */
    FILE *shown = fopen("/run/theme", "w");
    if (shown) {
        fputs(vx_theme.dark ? "dark\n" : "light\n", shown);
        fclose(shown);
    }
#define TEXT_SETTING(var, key, fallback) \
    snprintf(var, sizeof(var), "%s", vx_settings_get(&config, key, fallback))
    TEXT_SETTING(setting_wallpaper, "wallpaper", "image");
    TEXT_SETTING(setting_image, "wallpaper_image", DESKTOP_DEFAULT_WALLPAPER);
    TEXT_SETTING(setting_wallpaper_mode, "wallpaper_mode", "fill");
    TEXT_SETTING(setting_title_click, "title_double_click", "maximize");
    TEXT_SETTING(setting_layout, "keyboard_layout", "us");
#undef TEXT_SETTING
    setting_clock = vx_settings_int(&config, "clock", 24) == 12 ? 12 : 24;
    setting_clock_seconds = vx_settings_bool(&config, "clock_seconds", false);
    setting_clock_date = vx_settings_bool(&config, "clock_date", true);
    setting_clock_weekday = vx_settings_bool(&config, "clock_weekday", true);
    setting_note_ms = vx_settings_int(&config, "notification_seconds", 4) * 1000;
    setting_note_ms = setting_note_ms < 1000 ? 1000 : setting_note_ms;
    setting_snapping = vx_settings_bool(&config, "snapping", true);
    setting_animations = vx_settings_bool(&config, "animations", true);
    setting_pointer_speed = vx_settings_int(&config, "pointer_speed", 5);
    setting_pointer_speed = setting_pointer_speed < 1 ? 1 : setting_pointer_speed > 10 ? 10
                                                                              : setting_pointer_speed;
    setting_double_click = vx_settings_int(&config, "double_click_ms", 500);
    setting_natural_scroll = vx_settings_bool(&config, "natural_scroll", false);
    setting_left_handed = vx_settings_bool(&config, "left_handed", false);
    setting_key_delay = vx_settings_int(&config, "key_delay", 500);
    setting_key_rate = vx_settings_int(&config, "key_rate", 20);
    setting_width = vx_settings_int(&config, "display_width", 0);
    setting_height = vx_settings_int(&config, "display_height", 0);
    setting_scale = vx_settings_int(&config, "display_scale", 1) == 2 ? 2 : 1;
    lock_settings();
}

/* Fills a surface with a vertical gradient. */
static void gradient(struct vx_surface *s, uint32_t top, uint32_t bottom) {
    for (int y = 0; y < s->height; y++) {
        uint32_t color = vx_mix(top, bottom, y * 255 / (s->height > 1 ? s->height - 1 : 1));
        uint32_t *row = s->pixels + (long)y * s->stride;
        for (int x = 0; x < s->width; x++) {
            row[x] = color;
        }
    }
}

static void make_wallpaper(void) {
    /* A picture, as the wallpaper mode says: covering the screen (fill),
     * all of it on the screen (fit), as it is (center), repeated (tile), or
     * stretched to the screen's shape... */
    struct vx_image *image = NULL;
    if (!strcmp(setting_wallpaper, "image") && setting_image[0]) {
        /* The default picture: the one in the theme's colors. */
        if (!strcmp(setting_image, DESKTOP_DEFAULT_WALLPAPER)) {
            char path[96];
            vx_theme_wallpaper(&vx_theme, path, sizeof(path));
            image = vx_image_load(path, 0);
        }
        if (!image) {
            image = vx_image_load(setting_image, 0);
        }
    }
    if (image) {
        const char *mode = setting_wallpaper_mode;
        int iw = image->surface.width, ih = image->surface.height;
        int sw = wallpaper.width, sh = wallpaper.height;
        gradient(&wallpaper, 0x202028, 0x08080c); /* Behind a picture that doesn't cover it. */
        if (!strcmp(mode, "tile")) {
            for (int y = 0; y < sh; y += ih) {
                for (int x = 0; x < sw; x += iw) {
                    vx_blit(&wallpaper, x, y, &image->surface, 0, 0, iw, ih);
                }
            }
        } else if (!strcmp(mode, "center")) {
            vx_blit(&wallpaper, (sw - iw) / 2, (sh - ih) / 2, &image->surface, 0, 0, iw, ih);
        } else if (!strcmp(mode, "stretch")) {
            blit_smooth(&wallpaper, (struct rect){0, 0, sw, sh}, &image->surface, 255);
        } else {
            bool fit = !strcmp(mode, "fit");
            int w = sw, h = iw ? ih * sw / iw : sh;
            if (fit ? h > sh : h < sh) {
                h = sh;
                w = iw * sh / ih;
            }
            blit_smooth(&wallpaper, (struct rect){(sw - w) / 2, (sh - h) / 2, w, h},
                        &image->surface, 255);
        }
        vx_image_free(image);
    } else {
        /* ... or a gradient, with the name large in the bottom right corner
         * (a little lighter than the gradient's top). */
        const struct desktop_wallpaper *choice = &desktop_wallpapers[0];
        for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
            if (!strcmp(desktop_wallpapers[i].name, setting_wallpaper)) {
                choice = &desktop_wallpapers[i];
            }
        }
        gradient(&wallpaper, choice->top, choice->bottom);
        uint32_t text_color = mix(choice->top, 0xffffff, 1, 8);
        const struct vx_font *big = vx_font(VX_FACE_BOLD, wallpaper.width >= 1024 ? 128 : 64);
        const char *name = "Vexa";
        int width = vx_text_width_font(big, name);
        int x = wallpaper.width - width - 120;
        int y = wallpaper.height - vx_font_height(big) - 48;
        vx_text(&wallpaper, big, x, y, name, text_color, VX_TRANSPARENT);
        char version[64];
        struct vx_system_info info;
        snprintf(version, sizeof(version), "version %s",
                 vx_system_info(&info) == 0 ? info.version : "?");
        vx_draw_text(&wallpaper, x + 8, y + vx_font_height(big), version, text_color,
                     VX_TRANSPARENT);
    }
    lock_screen_changed();
}

/* The title bar's buttons, right to left: close, maximize, minimize (a
 * window that can't be resized has close and minimize). */
static int title_button_at(const struct window *w, int x, int y) {
    if (w->popup || w->undecorated || y >= w->y || y < w->y - TITLE_HEIGHT) {
        return -1;
    }
    int right = w->x + w->content.width;
    int button = x >= right - BUTTON_WIDTH ? 0
                 : x >= right - 2 * BUTTON_WIDTH ? 1
                 : x >= right - 3 * BUTTON_WIDTH ? 2 : -1;
    if (!w->resizable && button == 2) {
        button = -1;
    }
    return x < w->x ? -1 : button;
}

static void draw_line(struct vx_surface *view, int x0, int y0, int x1, int y1, uint32_t color) {
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / (steps ? steps : 1);
        int y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        vx_fill(view, x, y, 1, 1, color);
    }
}

/* The title bar's glass: its tint, and how much of it. */
static uint32_t title_tint(bool on, int *alpha) {
    if (on) { /* The accent, as glass. */
        *alpha = vx_theme.dark ? 150 : 135;
        return vx_mix(COLOR_OUTLINE, vx_theme.dark ? 0x121214 : 0xffffff, 105);
    }
    *alpha = vx_theme.dark ? 205 : 215;
    return vx_theme.dark ? 0x2c2c2f : 0xe4e4ea;
}

/* Colors by hue (0 to 359), saturation and value (0 to 255). */
static void to_hsv(uint32_t c, int *h, int *sat, int *v) {
    int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = max - min;
    *v = max;
    *sat = max ? d * 255 / max : 0;
    if (!d) {
        *h = 0;
    } else if (max == r) {
        *h = (60 * (g - b) / d + 360) % 360;
    } else if (max == g) {
        *h = 60 * (b - r) / d + 120;
    } else {
        *h = 60 * (r - g) / d + 240;
    }
}

static uint32_t from_hsv(int h, int sat, int v) {
    h = (h % 360 + 360) % 360;
    int c = v * sat / 255, x = c * (60 - abs(h % 120 - 60)) / 60, m = v - c;
    int r = 0, g = 0, b = 0;
    switch (h / 60) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    return (uint32_t)(r + m) << 16 | (uint32_t)(g + m) << 8 | (uint32_t)(b + m);
}

/* `color` with its hue turned by `degrees`, or set to `hue`. */
static uint32_t turn_hue(uint32_t color, int degrees) {
    int h, sat, v;
    to_hsv(color, &h, &sat, &v);
    return from_hsv(h + degrees, sat, v);
}

static uint32_t with_hue(uint32_t color, int hue) {
    int h, sat, v;
    to_hsv(color, &h, &sat, &v);
    return from_hsv(hue, sat < 150 ? 150 : sat, v < 200 ? 200 : v);
}

/* A glyph on a title bar button: a cross, a bar, a plus or two boxes. */
static void draw_glyph(struct vx_surface *view, int button, bool maximized, int cx, int cy,
                       uint32_t color) {
    if (button == 0) {
        for (int t = 0; t < 2; t++) {
            draw_line(view, cx - 4 + t, cy - 4, cx + 3 + t, cy + 3, color);
            draw_line(view, cx + 3 + t, cy - 4, cx - 4 + t, cy + 3, color);
        }
    } else if (button == 1 && maximized) {
        vx_draw_outline(view, cx - 4, cy - 2, 7, 7, color);
        vx_draw_outline(view, cx - 3, cy - 1, 5, 5, color);
        vx_fill(view, cx - 2, cy - 5, 7, 2, color);
        vx_fill(view, cx + 3, cy - 5, 2, 7, color);
    } else if (button == 1) {
        vx_fill(view, cx - 4, cy - 1, 9, 2, color);
        vx_fill(view, cx - 1, cy - 4, 2, 9, color);
    } else {
        vx_fill(view, cx - 4, cy - 1, 9, 2, color);
    }
}

static void draw_title_bar(struct vx_surface *view, struct window *w, int x, int y) {
    /* x, y: the content's corner. */
    int width = w->content.width, top = y - TITLE_HEIGHT;
    bool on = w == focused;
    /* Glass: what's behind, blurred and tinted, with a shine on the top
     * half; the top corners round, a dark edge and a bright line inside it. */
    int alpha;
    uint32_t tint = title_tint(on, &alpha);
    draw_glass(view, (struct rect){x - BORDER, top - BORDER, width + 2 * BORDER, TITLE_HEIGHT + BORDER},
               CORNER + 1, 0, tint, alpha, on ? 120 : 85);
    outline_rounded(view, (struct rect){x - BORDER, top - BORDER, width + 2 * BORDER,
                                        TITLE_HEIGHT + 2 * CORNER},
                    CORNER + 1, COLOR_BORDER);
    blend_rect(view, (struct rect){x + CORNER - 2, top, width - 2 * CORNER + 4, 1}, 0xffffff,
               vx_theme.dark ? 70 : 150);
    blend_rect(view, (struct rect){x, y - 1, width, 1}, 0x000000, 50);
    int button_count = w->resizable ? 3 : 2;
    /* The title, in the middle of the room the buttons leave: light on dark
     * glass over a shadow, dark on light glass over a glow. */
    uint32_t text = vx_theme.dark ? (on ? 0xffffff : 0xa8a4b8) : (on ? 0x101420 : 0x6c6c78);
    uint32_t halo = vx_theme.dark ? 0x000000 : 0xffffff;
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 13);
    int room = width - 16 - button_count * BUTTON_WIDTH;
    int tw = vx_text_width_font(bold, w->title);
    int tx = tw < room ? x + 8 + (room - tw) / 2 : x + 8;
    int ty = top + (TITLE_HEIGHT - vx_font_height(bold)) / 2;
    char shown[80];
    if (tw <= room) {
        snprintf(shown, sizeof(shown), "%s", w->title);
    } else {
        static const char ellipsis[] = "\xe2\x80\xa6";
        size_t n = vx_text_fit_bytes(bold, w->title, room - vx_text_width_font(bold, ellipsis));
        snprintf(shown, sizeof(shown), "%.*s%s", (int)n, w->title, ellipsis);
    }
    if (on || !vx_theme.dark) {
        vx_text(view, bold, tx, ty + 1, shown, vx_mix(tint, halo, 200), VX_TRANSPARENT);
    }
    vx_text(view, bold, tx, ty, shown, text, VX_TRANSPARENT);
    /* The buttons, glossy rounded squares in the theme's colors: close (a
     * red as bright and strong as the accent) at the right end, then
     * maximize (the accent) and minimize (the accent's hue turned a little);
     * grey on windows in the
     * back. Their signs show on the window in front, brighter under the
     * pointer. */
    uint32_t colors[3] = {with_hue(COLOR_OUTLINE, 3), COLOR_OUTLINE,
                          turn_hue(COLOR_OUTLINE, 40)};
    bool lit = w == hot_window && hot_button >= 0;
    int bx = x + width - BUTTON_WIDTH, cy = top + TITLE_HEIGHT / 2;
    for (int i = 0; i < 3; i++) {
        if (i == 1 && !w->resizable) {
            continue;
        }
        uint32_t color = on || lit ? colors[i] : vx_theme.dark ? 0x5e5e63 : 0xc4c4cc;
        bool under = lit && hot_button == i;
        if (under) {
            color = vx_mix(color, 0xffffff, 40);
        }
        int cx = bx + BUTTON_WIDTH / 2;
        int left = cx - BUTTON_SIZE / 2, up = cy - BUTTON_SIZE / 2;
        vx_fill_rounded(view, left, up + 1, BUTTON_SIZE, BUTTON_SIZE, 6, 0x000000, 40); /* Shadow. */
        vx_draw_gel(view, left, up, BUTTON_SIZE, BUTTON_SIZE, 6, color);
        if (on || lit) {
            uint32_t sign = vx_mix(color, 0xffffff, lit ? 255 : 205);
            draw_glyph(view, i, w->maximized, cx, cy, sign);
        }
        bx -= BUTTON_WIDTH;
    }
}

/* A window, frame and all; (x, y) is where its frame's corner goes. */
void draw_window_at(struct vx_surface *view, struct window *w, int x, int y) {
    if (w->popup || w->undecorated) {
        vx_blit(view, x, y, &w->content, 0, 0, w->content.width, w->content.height);
        return;
    }
    int cx = x + BORDER, cy = y + BORDER + TITLE_HEIGHT;
    vx_fill(view, x, cy, w->content.width + 2 * BORDER, w->content.height + BORDER, COLOR_BORDER);
    draw_title_bar(view, w, cx, cy);
    vx_blit(view, cx, cy, &w->content, 0, 0, w->content.width, w->content.height);
}

static void draw_panel(struct vx_surface *view, int ox, int oy) {
    /* Glass across the top, a little darker than the windows', with a bright
     * line along its top and a dark one under it. */
    struct rect bar = {ox, oy, screen.width, PANEL_HEIGHT};
    draw_glass(view, bar, 0, 0, COLOR_PANEL, vx_theme.dark ? 175 : 165, 90);
    blend_rect(view, (struct rect){ox, oy, screen.width, 1}, 0xffffff, vx_theme.dark ? 40 : 140);
    blend_rect(view, (struct rect){ox, oy + PANEL_HEIGHT - 1, screen.width, 1}, 0x000000,
               vx_theme.dark ? 160 : 70);
    /* The Vexa menu's button: a gel of the accent, a white diamond and the name. */
    struct rect m = {ox + 3, oy + 3, MENU_BUTTON_WIDTH, PANEL_HEIGHT - 6};
    vx_draw_gel(view, m.x, m.y, m.width, m.height, m.height / 2,
                menu_open ? vx_mix(COLOR_OUTLINE, 0x000000, 50) : COLOR_OUTLINE);
    for (int i = 0; i < 5; i++) {
        vx_fill(view, ox + 15 - i, oy + 8 + i, 2 * i + 1, 1, 0xffffff);
        vx_fill(view, ox + 15 - i, oy + 16 - i, 2 * i + 1, 1, 0xffffff);
    }
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 13);
    vx_text(view, bold, ox + 27, oy + 6, "Vexa", vx_mix(COLOR_OUTLINE, 0x000000, 120),
            VX_TRANSPARENT);
    vx_text(view, bold, ox + 27, oy + 5, "Vexa", 0xffffff, VX_TRANSPARENT);

    /* The windows: glassy buttons, the focused one a gel of the accent. */
    struct window *list[MAX_WINDOWS];
    int n = windows_by_id(list);
    for (int i = 0; i < n; i++) {
        struct rect b = task_button(i, n);
        struct window *w = list[i];
        struct rect r = {ox + b.x, oy + b.y, b.width, b.height};
        bool on = w == focused && !w->minimized;
        if (on) {
            vx_draw_gel(view, r.x, r.y, r.width, r.height, 6, COLOR_OUTLINE);
        } else {
            fill_gel(view, r, 6, vx_theme.dark ? 0x58585d : 0xffffff, w->minimized ? 40 : 90);
            outline_rounded(view, r, 6, vx_mix(COLOR_PANEL, 0x000000, w->minimized ? 40 : 80));
        }
        uint32_t color = on ? 0xffffff : w->minimized ? COLOR_PANEL_DIM : COLOR_PANEL_TEXT;
        if (on) { /* (White over a darker copy: it reads on the light top.) */
            vx_draw_text_fit(view, r.x + 8, r.y + 3, b.width - 14, w->title,
                             vx_mix(COLOR_OUTLINE, 0x000000, 120), VX_TRANSPARENT);
        }
        vx_draw_text_fit(view, r.x + 8, r.y + 2, b.width - 14, w->title, color, VX_TRANSPARENT);
    }

    /* Search, and the clock (with a dot for notifications not seen yet). */
    struct rect s = search_button_rect();
    if (search_open) {
        fill_gel(view, (struct rect){ox + s.x, oy + s.y, s.width, s.height}, 6, COLOR_OUTLINE, 255);
    }
    draw_magnifier(view, ox + s.x + 7, oy + s.y + 4, 14, search_open ? 0xffffff : COLOR_PANEL_TEXT);
    struct rect c = clock_button_rect();
    if (clock_open) {
        fill_gel(view, (struct rect){ox + c.x, oy + c.y, c.width, c.height}, 6, COLOR_OUTLINE, 255);
    }
    char text[48];
    if (clock_text(text, sizeof(text))) {
        vx_draw_text(view, ox + c.x + 8, oy + 5, text, clock_open ? 0xffffff : COLOR_PANEL_TEXT,
                     VX_TRANSPARENT);
    }
    if (unread_notes) {
        draw_orb(view, ox + c.x + c.width - 10, oy + 12, 4, COLOR_OUTLINE);
    }
    volume_draw_button(view, ox, oy);
    struct rect k = layout_button_rect();
    vx_draw_text(view, ox + k.x + 8, oy + 5, layout_label(), COLOR_PANEL_TEXT, VX_TRANSPARENT);
}

/* One item of the menu or a submenu, at `top` in the panel `r`. */
static void draw_menu_item(struct vx_surface *view, int ox, int oy, struct rect r, int top,
                           const struct menu_item *item, bool lit) {
    int h = item->action == SEPARATOR ? MENU_SEPARATOR_HEIGHT : MENU_ITEM_HEIGHT;
    if (item->action == SEPARATOR) {
        vx_fill(view, ox + r.x + 8, oy + top + h / 2, r.width - 16, 1, COLOR_PANEL_LINE);
        return;
    }
    if (lit) {
        vx_draw_gel(view, ox + r.x + 4, oy + top, r.width - 8, h, 6, COLOR_OUTLINE);
    }
    int text_x = ox + r.x + 12;
    if (item->action == RUN_APP && app_icons[item->app]) {
        vx_blit_alpha(view, text_x, oy + top + 4, 16, 16, &app_icons[item->app]->surface);
    }
    if (item->action == RUN_APP || item->action == RUN_LINUX_APP) {
        text_x += 24;
    }
    if (lit) {
        vx_draw_text(view, text_x, oy + top + 5, item->label, vx_mix(COLOR_OUTLINE, 0x000000, 120),
                     VX_TRANSPARENT);
    }
    vx_draw_text(view, text_x, oy + top + 4, item->label, lit ? 0xffffff : COLOR_PANEL_TEXT,
                 VX_TRANSPARENT);
    uint32_t dim = lit ? vx_mix(COLOR_OUTLINE, 0xffffff, 190) : COLOR_PANEL_DIM;
    if (item->action == SUBMENU) {
        /* An arrow: the submenu opens to the right. */
        int ax = ox + r.x + r.width - 18, ay = oy + top + h / 2;
        for (int i = 0; i < 5; i++) {
            vx_fill(view, ax + i, ay - 4 + i, 1, 9 - 2 * i, dim);
        }
        return;
    }
    int keys = vx_text_width(item->keys);
    vx_draw_text(view, ox + r.x + r.width - keys - 12, oy + top + 4, item->keys, dim,
                 VX_TRANSPARENT);
}

static void draw_menu(struct vx_surface *view, int ox, int oy) {
    struct rect r = menu_rect();
    draw_glass_popup(view, (struct rect){ox + r.x, oy + r.y, r.width, r.height}, 8, 140);
    int top = r.y + 4;
    for (int i = 0; i < MENU_ITEMS; i++) {
        bool open = menu_items[i].action == SUBMENU && menu_items[i].group == menu_group;
        draw_menu_item(view, ox, oy, r, top, &menu_items[i], i == menu_hot || open);
        top += menu_item_height(i);
    }
    if (menu_group >= 0) {
        struct rect g = group_rect();
        draw_glass_popup(view, (struct rect){ox + g.x, oy + g.y, g.width, g.height}, 8, 140);
        for (int i = 0; i < group_count[menu_group]; i++) {
            draw_menu_item(view, ox, oy, g, g.y + 4 + i * MENU_ITEM_HEIGHT,
                           &group_items[menu_group][i], i == group_hot);
        }
    }
}

/* ---- The desktop's right-click menu (and the questions it asks) ---- */

enum popup_action {
    POP_TERMINAL, POP_FILES, POP_NEW_FOLDER, POP_SETTINGS, POP_ABOUT, POP_ICON, POP_RESTART,
    POP_POWER_OFF, POP_LEAVE, POP_LOGOUT, POP_LAYOUT, POP_NONE
};

#define MAX_POPUP 10
static struct vx_menu_item popup_items[MAX_POPUP];
static enum popup_action popup_actions[MAX_POPUP];
static int popup_arguments[MAX_POPUP];
static int popup_count, popup_x, popup_y, popup_hot = -1, popup_icon = -1;
static bool popup_open;

static struct rect popup_rect(void) {
    int w, h;
    vx_menu_size(popup_items, popup_count, &w, &h);
    return (struct rect){popup_x, popup_y, w + 3, h + 3}; /* With its shadow. */
}

static void popup_add(const char *label, const char *keys, enum popup_action action,
                      int argument) {
    popup_items[popup_count] = (struct vx_menu_item){label, keys, false};
    popup_arguments[popup_count] = argument;
    popup_actions[popup_count++] = action;
}

static void place_popup(int x, int y) {
    int w, h;
    vx_menu_size(popup_items, popup_count, &w, &h);
    popup_x = x + w + 4 > screen.width ? screen.width - w - 4 : x;
    popup_y = y + h + 4 > screen.height ? screen.height - h - 4 : y;
    popup_hot = -1;
    popup_open = true;
    add_damage(popup_rect());
}

/* On an icon: its menu; elsewhere: the desktop's. */
static void open_popup(int icon) {
    popup_count = 0;
    popup_icon = icon;
    if (icon >= 0) {
        const char *labels[8];
        int n = icons_menu_items(icon, labels, 8);
        for (int i = 0; i < n; i++) {
            if (i == n - 1 && n > 2) {
                popup_add(NULL, NULL, POP_NONE, 0);
            }
            popup_add(labels[i], NULL, POP_ICON, i);
        }
    } else {
        popup_add("New Terminal", "Ctrl+Alt+T", POP_TERMINAL, 0);
        popup_add("Open Files", "Ctrl+Alt+F", POP_FILES, 0);
        popup_add("New Folder", NULL, POP_NEW_FOLDER, 0);
        popup_add(NULL, NULL, POP_NONE, 0);
        popup_add("Change Wallpaper...", NULL, POP_SETTINGS, 0);
        popup_add("About Vexa", NULL, POP_ABOUT, 0);
    }
    place_popup(pointer_x, pointer_y);
    printf("desktop: menu at %d,%d\n", pointer_x, pointer_y);
}

static void close_popup(void) {
    if (popup_open) {
        popup_open = false;
        add_damage(popup_rect());
    }
}

/* Restart, Shut Down and leaving the desktop ask first, with a menu. */
static void ask(enum popup_action action, int x, int y) {
    close_popup();
    popup_count = 0;
    popup_icon = -1;
    popup_add(action == POP_RESTART     ? "Restart Now"
              : action == POP_POWER_OFF ? "Shut Down Now"
              : action == POP_LOGOUT    ? "Log Out Now"
                                        : "Leave the Desktop",
              NULL, action, 0);
    popup_add(NULL, NULL, POP_NONE, 0);
    popup_add("Cancel", NULL, POP_NONE, 0);
    place_popup(x, y);
    printf("desktop: asking before %s\n", action == POP_RESTART     ? "restarting"
                                          : action == POP_POWER_OFF ? "turning off"
                                          : action == POP_LOGOUT    ? "logging out"
                                                                    : "leaving");
}

/* ---- Notifications ---- */

static struct rect note_rect(int i) {
    return (struct rect){screen.width - NOTE_WIDTH - 12, PANEL_HEIGHT + 12 + i * (NOTE_HEIGHT + 10),
                         NOTE_WIDTH, NOTE_HEIGHT};
}

static struct rect notes_area(void) {
    return (struct rect){screen.width - NOTE_WIDTH - 12 - SHADOW, PANEL_HEIGHT + 12 - SHADOW,
                         NOTE_WIDTH + 2 * SHADOW, MAX_NOTES * (NOTE_HEIGHT + 10) + 2 * SHADOW};
}

static void launch_argv(const char *const *argv, int argc);

void add_note(const char *text) {
    if (note_count == MAX_NOTES) {
        memmove(notes, notes + 1, (MAX_NOTES - 1) * sizeof(notes[0]));
        note_count--;
    }
    struct note *note = &notes[note_count++];
    strncpy(note->text, text, sizeof(note->text) - 1);
    note->text[sizeof(note->text) - 1] = '\0';
    note->until = vx_uptime() + setting_note_ms;
    add_damage(notes_area());
    clock_remember(note->text);
    printf("desktop: notification \"%s\"\n", note->text);
    if (vx_settings_bool(&config, "notification_sound", false)) {
        static const char *const chime[] = {"/bin/play", "--chime"};
        launch_argv(chime, 2);
    }
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
    for (int i = 0; i < note_count; i++) {
        struct rect r = note_rect(i);
        r.x += ox, r.y += oy;
        draw_shadow(view, r, 140);
        fill_rounded(view, r, 10, COLOR_MENU);
        outline_rounded(view, r, 10, COLOR_PANEL_LINE);
        vx_fill(view, r.x + 10, r.y + 12, 3, r.height - 24, COLOR_OUTLINE);
        /* Two lines: "Title: text" puts the title on the first. */
        char first[128], second[128] = "";
        const char *colon = strstr(notes[i].text, ": ");
        if (colon && colon - notes[i].text < 40) {
            snprintf(first, sizeof(first), "%.*s", (int)(colon - notes[i].text), notes[i].text);
            snprintf(second, sizeof(second), "%s", colon + 2);
        } else {
            /* Too long for one line: the rest on the second. */
            size_t n = vx_text_fit_bytes(vx_font_ui(), notes[i].text, r.width - 36);
            snprintf(first, sizeof(first), "%.*s", (int)n, notes[i].text);
            snprintf(second, sizeof(second), "%s", notes[i].text + n);
        }
        vx_text(view, vx_font(VX_FACE_BOLD, 13), r.x + 22, r.y + 9, first, COLOR_PANEL_TEXT,
                VX_TRANSPARENT);
        vx_draw_text_fit(view, r.x + 22, r.y + 27, r.width - 34, second, COLOR_PANEL_DIM,
                         VX_TRANSPARENT);
    }
}

/* ---- Snapping ---- */

/* Where a window dragged to here would go: the top edge maximizes, the
 * sides take half the screen, and the corners a quarter. */
static enum snap snap_target(void) {
    if (!dragged || !dragged->resizable || !setting_snapping) {
        return SNAP_NONE;
    }
    int corner = 48;
    bool left = pointer_x <= 1, right = pointer_x >= screen.width - 2;
    bool top = pointer_y <= PANEL_HEIGHT + 1, bottom = pointer_y >= screen.height - 2;
    if ((left && pointer_y <= PANEL_HEIGHT + corner) || (top && pointer_x <= corner)) {
        return SNAP_TOP_LEFT;
    }
    if ((right && pointer_y <= PANEL_HEIGHT + corner) || (top && pointer_x >= screen.width - corner)) {
        return SNAP_TOP_RIGHT;
    }
    if ((left && pointer_y >= screen.height - corner) || (bottom && pointer_x <= corner)) {
        return SNAP_BOTTOM_LEFT;
    }
    if ((right && pointer_y >= screen.height - corner) ||
        (bottom && pointer_x >= screen.width - corner)) {
        return SNAP_BOTTOM_RIGHT;
    }
    return top ? SNAP_TOP : left ? SNAP_LEFT : right ? SNAP_RIGHT : SNAP_NONE;
}

/* The content rectangle a snapped window gets. */
static struct rect snap_rect(enum snap where) {
    struct rect area = work_area();
    if (where == SNAP_TOP || where == SNAP_NONE) {
        return area;
    }
    int half_w = screen.width / 2;
    int frame_h = screen.height - PANEL_HEIGHT; /* With the title bars. */
    int half_h = frame_h / 2;
    bool left = where == SNAP_LEFT || where == SNAP_TOP_LEFT || where == SNAP_BOTTOM_LEFT;
    bool quarter = where != SNAP_LEFT && where != SNAP_RIGHT;
    bool lower = where == SNAP_BOTTOM_LEFT || where == SNAP_BOTTOM_RIGHT;
    struct rect r;
    r.x = left ? BORDER : half_w + BORDER;
    r.width = (left ? half_w : screen.width - half_w) - 2 * BORDER;
    int frame_top = PANEL_HEIGHT + (lower ? half_h : 0);
    int frame_height = quarter ? (lower ? frame_h - half_h : half_h) : frame_h;
    r.y = frame_top + TITLE_HEIGHT + BORDER;
    r.height = frame_height - TITLE_HEIGHT - 2 * BORDER;
    return r;
}

static struct rect snap_damage(enum snap where) {
    struct rect r = snap_rect(where);
    return (struct rect){r.x - BORDER - 2, r.y - TITLE_HEIGHT - BORDER - 2,
                         r.width + 2 * BORDER + 4, r.height + TITLE_HEIGHT + 2 * BORDER + 4};
}

/* ---- Animations ---- */

/* A window closing: a picture of it, fading. */
#define MAX_GHOSTS 4
static struct ghost {
    struct vx_surface picture;
    struct rect place;
    long start;
} ghosts[MAX_GHOSTS];
static int ghost_count;

/* A surface to draw a window into (for scaling it), kept for next time. */
static struct vx_surface scratch;
static size_t scratch_size;

static bool scratch_for(int width, int height) {
    size_t size = (size_t)width * height * 4;
    if (size > scratch_size) {
        if (scratch.pixels) {
            vx_unmap(scratch.pixels, scratch_size);
        }
        scratch.pixels = vx_map(size, VX_MAP_WRITE);
        scratch_size = scratch.pixels ? size : 0;
    }
    scratch.width = scratch.stride = width;
    scratch.height = height;
    return scratch.pixels != NULL;
}

/* 0 to 256, eased out (fast, then slow). */
static int ease(long elapsed, long duration) {
    if (elapsed >= duration) {
        return 256;
    }
    long t = elapsed * 256 / duration;
    long inverse = 256 - t;
    return (int)(256 - inverse * inverse * inverse / (256 * 256));
}

static struct rect lerp_rect(struct rect a, struct rect b, int t) {
    return (struct rect){a.x + (b.x - a.x) * t / 256, a.y + (b.y - a.y) * t / 256,
                         a.width + (b.width - a.width) * t / 256,
                         a.height + (b.height - a.height) * t / 256};
}

static struct rect task_button_of(struct window *w) {
    struct window *list[MAX_WINDOWS];
    int n = windows_by_id(list);
    for (int i = 0; i < n; i++) {
        if (list[i] == w) {
            return task_button(i, n);
        }
    }
    return (struct rect){screen.width / 2, 0, 1, 1};
}

static long animation_length(enum animation a) {
    return a == ANIM_OPEN ? OPEN_MS : MINIMIZE_MS;
}

/* Where an animating window is drawn now, and how solid. */
static struct rect animation_place(struct window *w, int *alpha) {
    struct rect f = frame_rect(w);
    int t = ease(now_ms() - w->animation_start, animation_length(w->animation));
    if (w->animation == ANIM_OPEN) {
        struct rect small = {f.x + f.width * 4 / 100, f.y + f.height * 4 / 100,
                             f.width * 92 / 100, f.height * 92 / 100};
        *alpha = t > 255 ? 255 : t;
        return lerp_rect(small, f, t);
    }
    struct rect button = task_button_of(w);
    if (w->animation == ANIM_RESTORE) {
        t = 256 - t;
    }
    *alpha = 255 - t * 180 / 256;
    return lerp_rect(f, button, t);
}

static void start_animation(struct window *w, enum animation a) {
    if (!setting_animations || w->popup) {
        w->animation = ANIM_NONE;
        return;
    }
    w->animation = a;
    w->animation_start = now_ms();
    want_frames();
}

static void animate(void) {
    long now = now_ms();
    for (int i = 0; i < window_count; i++) {
        struct window *w = stack[i];
        if (w->animation == ANIM_NONE) {
            continue;
        }
        add_damage(window_damage(w));
        if (w->animation != ANIM_OPEN) {
            add_damage(task_button_of(w));
            struct rect f = frame_rect(w), b = task_button_of(w);
            int x0 = f.x < b.x ? f.x : b.x, y0 = b.y;
            int x1 = f.x + f.width > b.x + b.width ? f.x + f.width : b.x + b.width;
            add_damage((struct rect){x0, y0, x1 - x0, f.y + f.height - y0});
        }
        if (now - w->animation_start >= animation_length(w->animation)) {
            w->animation = ANIM_NONE;
        } else {
            want_frames();
        }
    }
    for (int i = 0; i < ghost_count; i++) {
        add_damage(ghosts[i].place);
        if (now - ghosts[i].start >= CLOSE_MS) {
            vx_unmap(ghosts[i].picture.pixels,
                     (size_t)ghosts[i].picture.stride * ghosts[i].picture.height * 4);
            ghosts[i--] = ghosts[--ghost_count];
        } else {
            want_frames();
        }
    }
}

/* A picture of a window that's going, to fade out. */
static void add_ghost(struct window *w) {
    if (!setting_animations || w->popup || w->minimized || ghost_count == MAX_GHOSTS) {
        return;
    }
    struct rect f = frame_rect(w);
    struct ghost *g = &ghosts[ghost_count];
    g->picture.pixels = vx_map((size_t)f.width * f.height * 4, VX_MAP_WRITE);
    if (!g->picture.pixels) {
        return;
    }
    g->picture.width = g->picture.stride = f.width;
    g->picture.height = f.height;
    /* Behind round corners: what's on the screen there now. */
    vx_blit(&g->picture, 0, 0, &screen, f.x, f.y, f.width, f.height);
    draw_window_at(&g->picture, w, 0, 0);
    g->place = f;
    g->start = now_ms();
    ghost_count++;
    want_frames();
}

static void draw_animated(struct vx_surface *view, int ox, int oy, struct window *w) {
    int alpha;
    struct rect place = animation_place(w, &alpha);
    struct rect f = frame_rect(w);
    if (!scratch_for(f.width, f.height)) {
        return;
    }
    /* Behind its round corners: the wallpaper (close enough, briefly). */
    vx_blit(&scratch, 0, 0, &wallpaper, f.x, f.y, f.width, f.height);
    draw_window_at(&scratch, w, 0, 0);
    place.x += ox, place.y += oy;
    blit_smooth(view, place, &scratch, alpha);
}

/* ---- Composing the screen ---- */

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
    if (locked || saver_on) {
        lock_draw(&view, ox, oy);
    } else {
        vx_blit(&view, 0, 0, &wallpaper, area.x, area.y, area.width, area.height);
        if (!installer_only) {
            icons_draw(&view, ox, oy);
        }
        for (int i = 0; i < window_count; i++) {
            struct window *w = stack[i];
            if (w->animation != ANIM_NONE) {
                draw_animated(&view, ox, oy, w);
                continue;
            }
            if (w->minimized) {
                continue;
            }
            struct rect f = frame_rect(w);
            if (!w->popup && !w->undecorated) {
                draw_shadow(&view, (struct rect){f.x + ox, f.y + oy, f.width, f.height},
                            w == focused ? 200 : 130);
            }
            draw_window_at(&view, w, f.x + ox, f.y + oy);
        }
        for (int i = 0; i < ghost_count; i++) {
            struct ghost *g = &ghosts[i];
            long t = now_ms() - g->start;
            int alpha = t >= CLOSE_MS ? 0 : (int)(255 * (CLOSE_MS - t) / CLOSE_MS);
            int shrink = (int)(t >= CLOSE_MS ? 4 : 4 * t / CLOSE_MS);
            struct rect p = {g->place.x + g->place.width * shrink / 200 + ox,
                             g->place.y + g->place.height * shrink / 200 + oy,
                             g->place.width * (100 - shrink) / 100,
                             g->place.height * (100 - shrink) / 100};
            blit_smooth(&view, p, &g->picture, alpha);
        }
        if (drag == RESIZING) {
            struct rect r = {ox + outline.x - BORDER, oy + outline.y - TITLE_HEIGHT - BORDER,
                             outline.width + 2 * BORDER, outline.height + TITLE_HEIGHT + 2 * BORDER};
            vx_draw_outline(&view, r.x, r.y, r.width, r.height, COLOR_OUTLINE);
            vx_draw_outline(&view, r.x + 1, r.y + 1, r.width - 2, r.height - 2, COLOR_OUTLINE);
        }
        if (drag == MOVING && snap != SNAP_NONE) {
            /* Where it would go: a glassy outline. */
            struct rect s = snap_damage(snap);
            s.x += ox + 4, s.y += oy + 4, s.width -= 8, s.height -= 8;
            blend_rect(&view, s, COLOR_OUTLINE, 70);
            outline_rounded(&view, s, 10, COLOR_OUTLINE);
        }
        draw_notes(&view, ox, oy);
        if (area.y < PANEL_HEIGHT && !installer_only) {
            draw_panel(&view, ox, oy);
        }
        if (menu_open) {
            draw_menu(&view, ox, oy);
        }
        if (clock_open) {
            clock_draw(&view, ox, oy);
        }
        if (volume_open) {
            volume_draw(&view, ox, oy);
        }
        volume_draw_osd(&view, ox, oy);
        if (popup_open) {
            vx_draw_menu(&view, ox + popup_x, oy + popup_y, popup_items, popup_count, popup_hot);
        }
        icons_draw_drag(&view, ox, oy);
        if (switcher_open) {
            switcher_draw(&view, ox, oy);
        }
        if (search_open) {
            search_draw(&view, ox, oy);
        }
    }
    shot_draw(&view, ox, oy);
    if (!hide_cursor && !saver_on) {
        draw_cursor(&view, ox, oy, cursor_shape, pointer_x, pointer_y);
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
    if (setting_scale == 2) { /* Each pixel as four. */
        for (int row = area.y; row < area.y + area.height; row++) {
            uint32_t *from = screen.pixels + (long)row * screen.stride + area.x;
            uint32_t *to = (uint32_t *)((char *)frame + (long)row * 2 * display.pitch) + area.x * 2;
            for (int i = 0; i < area.width; i++) {
                uint32_t p = from[i];
                if (!native) {
                    p = ((p >> 16) & 0xff) << display.red_shift |
                        ((p >> 8) & 0xff) << display.green_shift | (p & 0xff) << display.blue_shift;
                }
                to[2 * i] = to[2 * i + 1] = p;
            }
            memcpy((char *)to + display.pitch, to, (size_t)area.width * 8);
        }
        return;
    }
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

/* Glass blurs what's behind it, so drawing part of a glass area again would
 * blur only that part: damage that touches one takes all of it. */
static bool touches(struct rect a, struct rect b) {
    return a.width > 0 && b.width > 0 && a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

static bool take_glass(struct rect r) {
    if (!touches(damage, r)) {
        return false;
    }
    struct rect before = damage;
    add_damage(r);
    return damage.x != before.x || damage.y != before.y || damage.width != before.width ||
           damage.height != before.height;
}

static void grow_damage_for_glass(void) {
    if (locked || saver_on) {
        return;
    }
    for (int round = 0; round < 4; round++) {
        bool grew = take_glass(panel_rect());
        for (int i = 0; i < window_count; i++) {
            struct window *w = stack[i];
            if (!w->minimized && !w->popup && !w->undecorated && w->animation == ANIM_NONE) {
                struct rect f = frame_rect(w);
                grew |= take_glass((struct rect){f.x, f.y, f.width, TITLE_HEIGHT + BORDER});
            }
        }
        if (menu_open) {
            grew |= take_glass(menu_rect());
            if (menu_group >= 0) {
                grew |= take_glass(group_rect());
            }
        }
        if (clock_open) {
            grew |= take_glass(clock_rect());
        }
        if (volume_open) {
            grew |= take_glass(volume_rect());
        }
        if (volume_osd_wait() >= 0) {
            grew |= take_glass((struct rect){(screen.width - 180) / 2, screen.height - 300, 180, 180});
        }
        if (search_open) {
            grew |= take_glass(search_rect());
        }
        if (switcher_open) {
            grew |= take_glass(switcher_rect());
        }
        if (!grew) {
            break;
        }
    }
}

static void redraw_damage(void) {
    grow_damage_for_glass();
    if (damage.width > 0) {
        compose(damage);
        show(damage);
        damage.width = 0;
    }
}

bool capture_screen(struct vx_surface *out) {
    hide_cursor = true;
    compose((struct rect){0, 0, screen.width, screen.height});
    hide_cursor = false;
    for (int y = 0; y < screen.height && y < out->height; y++) {
        memcpy(out->pixels + (long)y * out->stride, screen.pixels + (long)y * screen.stride,
               (size_t)(screen.width < out->width ? screen.width : out->width) * 4);
    }
    damage_all();
    return true;
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
        add_damage(window_damage(focused));
    }
    focused = w;
    if (w) {
        struct desktop_message m = {.type = DESKTOP_FOCUS, .window = (uint32_t)w->id, .a = 1};
        send_to(w->client, &m);
        add_damage(window_damage(w));
        mru_touch(w);
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
    add_damage(window_damage(w));
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

void activate(struct window *w) {
    if (w->minimized) {
        w->minimized = false;
        send_state(w);
        start_animation(w, ANIM_RESTORE);
        add_damage(window_damage(w));
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
    start_animation(w, ANIM_MINIMIZE);
    add_damage(window_damage(w));
    add_damage(panel_rect());
    if (focused == w) {
        set_focus(top_window());
    }
}

void ask_to_close(struct window *w) {
    struct desktop_message m = {.type = DESKTOP_CLOSE, .window = (uint32_t)w->id};
    send_to(w->client, &m);
    printf("desktop: asked window %d to close\n", w->id);
}

/* Asks the window's program for a new size (it answers with a new buffer). */
static void configure(struct window *w, int width, int height) {
    struct desktop_message m = {.type = DESKTOP_CONFIGURE, .window = (uint32_t)w->id,
                                .a = width, .b = height};
    send_to(w->client, &m);
}

static void toggle_maximized(struct window *w) {
    if (!w->resizable) {
        return;
    }
    add_damage(window_damage(w));
    if (w->maximized) {
        w->maximized = false;
        w->x = w->restore.x;
        w->y = w->restore.y;
        configure(w, w->restore.width, w->restore.height);
    } else {
        w->maximized = true;
        if (w->snapped == SNAP_NONE) {
            w->restore = (struct rect){w->x, w->y, w->content.width, w->content.height};
        }
        struct rect area = work_area();
        if (w->undecorated) { /* Its own title bar goes where ours would. */
            area.y -= TITLE_HEIGHT;
            area.height += TITLE_HEIGHT;
        }
        w->x = area.x;
        w->y = area.y;
        configure(w, area.width, area.height);
    }
    w->snapped = SNAP_NONE;
    add_damage(window_damage(w));
    send_moved(w);
    send_state(w);
    printf("desktop: %s window %d\n", w->maximized ? "maximized" : "restored", w->id);
}

static const char *const snap_names[] = {
    "nowhere", "top", "left", "right", "top left", "top right", "bottom left", "bottom right",
};

/* Puts a window in a half or a quarter of the screen (SNAP_TOP: maximized;
 * SNAP_NONE: back where it was). */
static void snap_window(struct window *w, enum snap where) {
    if (!w->resizable) {
        return;
    }
    if (where == SNAP_TOP) {
        if (!w->maximized) {
            toggle_maximized(w);
        }
        return;
    }
    if (where == SNAP_NONE) {
        if (w->maximized) {
            toggle_maximized(w);
        } else if (w->snapped != SNAP_NONE) {
            add_damage(window_damage(w));
            w->snapped = SNAP_NONE;
            w->x = w->restore.x;
            w->y = w->restore.y;
            configure(w, w->restore.width, w->restore.height);
            send_moved(w);
            printf("desktop: window %d back where it was\n", w->id);
        }
        return;
    }
    struct rect r = snap_rect(where);
    add_damage(window_damage(w));
    if (!w->maximized && w->snapped == SNAP_NONE) {
        w->restore = (struct rect){w->x, w->y, w->content.width, w->content.height};
    }
    if (w->maximized) {
        w->maximized = false;
        send_state(w);
    }
    w->snapped = where;
    w->x = r.x;
    w->y = r.y;
    configure(w, r.width, r.height);
    add_damage(window_damage(w));
    send_moved(w);
    printf("desktop: snapped window %d to the %s\n", w->id, snap_names[where]);
}

static struct window *find_window(int client, uint32_t id) {
    for (int i = 0; i < window_count; i++) {
        if (stack[i]->id == (int)id && stack[i]->client == client) {
            return stack[i];
        }
    }
    return NULL;
}

struct window *window_at(int x, int y) {
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

static void update_cursor(void);

static void destroy_window(struct window *w) {
    add_ghost(w);
    add_damage(window_damage(w));
    add_damage(panel_rect());
    int at = 0;
    while (stack[at] != w) {
        at++;
    }
    memmove(stack + at, stack + at + 1, (size_t)(window_count - at - 1) * sizeof(stack[0]));
    window_count--;
    mru_forget(w);
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
    if (grab == w) {
        grab = NULL;
    }
    if (hot_window == w) {
        hot_window = NULL;
        hot_button = -1;
    }
    if (focused == w) {
        focused = NULL;
        set_focus(top_window());
    }
    vx_unmap(w->content.pixels, w->mapped_size);
    vx_close(w->buffer_handle);
    printf("desktop: closed window %d \"%s\"\n", w->id, w->title);
    if (installer_only && !strcmp(w->title, "Installer")) {
        installer_only = false; /* The desktop, now: the panel and the icons. */
        damage_all();
        printf("desktop: the whole desktop now\n");
    }
    free(w);
    update_cursor();
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
        if (installer_only && !w->popup) { /* In the middle of the screen. */
            w->x = screen.width > width ? (screen.width - width) / 2 : BORDER;
            w->y = screen.height > height + TITLE_HEIGHT
                       ? (screen.height - height - TITLE_HEIGHT) / 2 + TITLE_HEIGHT
                       : TITLE_HEIGHT + BORDER;
        }
        stack[window_count++] = w;
        raise_window(w);
        if (!w->popup) {
            start_animation(w, ANIM_OPEN);
        }
        add_damage(window_damage(w));
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
        add_damage(window_damage(w));
        vx_unmap(w->content.pixels, w->mapped_size);
        vx_close(w->buffer_handle);
        w->content = (struct vx_surface){pixels, m->a, m->b, m->a};
        w->buffer_handle = handle;
        w->mapped_size = size;
        add_damage(window_damage(w));
        reply.a = m->a;
        reply.b = m->b;
        printf("desktop: window %d is now %dx%d\n", w->id, m->a, m->b);
    }
    send_to(client, &reply);
}

static struct rect outline_damage(void) {
    return (struct rect){outline.x - BORDER - 2, outline.y - TITLE_HEIGHT - BORDER - 2,
                         outline.width + 2 * BORDER + 4,
                         outline.height + TITLE_HEIGHT + 2 * BORDER + 4};
}

/* Starts dragging a window by the pointer. A maximized or snapped one first
 * goes back to its size from before, under the pointer. */
static void start_move(struct window *w) {
    if (w->maximized || w->snapped != SNAP_NONE) {
        int offset_y = pointer_y - w->y;
        add_damage(window_damage(w));
        if (w->maximized) {
            toggle_maximized(w);
        } else {
            w->snapped = SNAP_NONE;
            configure(w, w->restore.width, w->restore.height);
        }
        w->x = pointer_x - w->restore.width / 2;
        w->y = pointer_y - offset_y;
        add_damage(window_damage(w));
    }
    drag = MOVING;
    dragged = w;
    drag_dx = pointer_x - w->x;
    drag_dy = pointer_y - w->y;
}

static void start_resize(struct window *w, bool left, bool right, bool bottom) {
    drag = RESIZING;
    dragged = w;
    resize_left = left;
    resize_right = right && !left;
    resize_bottom = bottom;
    drag_dx = left ? w->x - pointer_x : w->x + w->content.width - pointer_x;
    drag_dy = w->y + w->content.height - pointer_y;
    drag_right = w->x + w->content.width;
    outline = (struct rect){w->x, w->y, w->content.width, w->content.height};
    add_damage(outline_damage());
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
            grab = NULL;
            start_move(w);
            printf("desktop: window %d moves itself\n", w->id);
        }
        break;
    case DESKTOP_WM_RESIZE:
        if ((buttons & 1) && drag == IDLE && w->resizable) {
            grab = NULL;
            start_resize(w, argument & 4, argument & 1, argument & 2);
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

void drop_on_window(struct window *w, const char *list, bool copy) {
    struct desktop_message m = {.type = DESKTOP_DROP, .window = (uint32_t)w->id,
                                .a = pointer_x - w->x, .b = pointer_y - w->y, .c = copy};
    snprintf(m.text, sizeof(m.text), "%s", list);
    send_to(w->client, &m);
    activate(w);
    printf("desktop: dropped files on window %d\n", w->id);
}

/* Files dragged out of a window and let go: into the window under the
 * pointer, or onto the desktop. */
static void dragged_out(struct window *from, struct desktop_message *m) {
    m->text[sizeof(m->text) - 1] = '\0';
    if (strncmp(m->text, "/tmp/.drag-", 11) != 0) {
        return;
    }
    struct window *to = window_at(pointer_x, pointer_y);
    if (to && to != from && !to->popup) {
        drop_on_window(to, m->text, m->a);
    } else if (!to && pointer_y >= PANEL_HEIGHT) {
        printf("desktop: files dropped on the desktop\n");
        icons_drop(m->text, m->a, pointer_x, pointer_y);
    } else {
        vx_remove(m->text);
    }
}

static bool apply_display(void);
static void fit_windows(void);
static void apply_key_repeat(void);

/* What the wallpaper is made from (the default picture's colors too). */
static void wallpaper_key(char *out, size_t size) {
    bool scheme = !strcmp(setting_image, DESKTOP_DEFAULT_WALLPAPER);
    snprintf(out, size, "%s|%s|%s|%d|%x", setting_wallpaper, setting_image, setting_wallpaper_mode,
             scheme && vx_theme.dark, scheme ? vx_theme.accent : 0);
}

/* Settings changed (or someone logged in): reads them again and applies them. */
static void reload_settings(void) {
    /* The wallpaper is made again only if it changed (a big picture
     * takes a while to read). */
    char before[600], after[600];
    wallpaper_key(before, sizeof(before));
    read_config();
    wallpaper_key(after, sizeof(after));
    bool resized = apply_display();
    if (resized) {
        /* Something on the screen at once: reading a big picture takes a while. */
        gradient(&wallpaper, 0x202028, 0x08080c);
        fit_windows();
        damage_all();
        redraw_damage();
    }
    if (resized || strcmp(before, after)) {
        make_wallpaper();
    }
    apply_key_repeat();
    set_menu(false);
    close_popup();
    build_menu();
    icons_load();
    /* Every program reads the theme again, and draws itself. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].handle >= 0) {
            struct desktop_message theme = {.type = DESKTOP_THEME};
            send_to(i, &theme);
        }
    }
    damage_all();
    printf("desktop: settings reloaded (%s, %s)\n", vx_theme.dark ? "dark" : "light",
           vx_settings_get(&config, "accent", VX_ACCENT_DEFAULT));
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
            add_damage(window_damage(w));
            w->x = m.a;
            w->y = w->popup || m.b >= PANEL_HEIGHT + TITLE_HEIGHT + BORDER
                       ? m.b : PANEL_HEIGHT + TITLE_HEIGHT + BORDER;
            add_damage(window_damage(w));
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
    case DESKTOP_CURSOR:
        if (w && m.a >= 0 && m.a < VX_CURSOR_COUNT) {
            w->cursor = m.a;
            update_cursor();
        }
        break;
    case DESKTOP_DRAG:
        if (w) {
            dragged_out(w, &m);
        }
        break;
    case DESKTOP_NOTIFY:
        m.text[sizeof(m.text) - 1] = '\0';
        add_note(m.text);
        break;
    case DESKTOP_LOCK:
        lock_now();
        break;
    case DESKTOP_CLIPBOARD_SET:
        /* Everyone hears of it (Xvexa hands it to X programs), but the one
         * that set it. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].handle >= 0 && i != client) {
                struct desktop_message clip = {.type = DESKTOP_CLIPBOARD};
                send_to(i, &clip);
            }
        }
        printf("desktop: clipboard changed%s\n", m.a ? " (from X)" : "");
        break;
    case DESKTOP_RELOAD:
        reload_settings();
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

void run_app(int i) {
    int process = vx_app_open(&apps[i], NULL);
    if (process < 0) {
        printf("desktop: can't start %s: %s\n", apps[i].name, vx_strerror(process));
        return;
    }
    add_child(process);
}

/* An app by its bundle's name ("Terminal" for Terminal.vxapp), or -1. */
static int app_named(const char *stem) {
    for (int i = 0; i < app_count; i++) {
        const char *file = strrchr(apps[i].bundle, '/');
        size_t n = strlen(stem);
        if (file && !strncmp(file + 1, stem, n) && !strcmp(file + 1 + n, VX_APP_EXTENSION)) {
            return i;
        }
    }
    return -1;
}

void run_named(const char *stem, const char *argument) {
    int i = app_named(stem);
    if (i < 0) {
        printf("desktop: no %s app\n", stem);
        return;
    }
    int process = vx_app_open(&apps[i], argument);
    if (process >= 0) {
        add_child(process);
    }
}

void open_path(const char *path) {
    struct vx_stat st;
    size_t length = strlen(path);
    bool bundle = length > 6 && !strcmp(path + length - 6, VX_APP_EXTENSION);
    if (bundle) {
        struct vx_app app;
        if (vx_app_load(path, &app) == 0) {
            int process = vx_app_open(&app, NULL);
            if (process >= 0) {
                add_child(process);
            }
        }
        return;
    }
    if (vx_stat(path, &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
        run_named("Files", path);
        return;
    }
    struct vx_app app;
    if (vx_app_for_file(path, &app) == 0) {
        int process = vx_app_open(&app, path);
        if (process >= 0) {
            add_child(process);
        }
    } else {
        char note[200];
        snprintf(note, sizeof(note), "No app opens %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        add_note(note);
    }
}

static void run_popup_item(int index) {
    switch (popup_actions[index]) {
    case POP_TERMINAL: run_named("Terminal", NULL); break;
    case POP_FILES: run_named("Files", NULL); break;
    case POP_NEW_FOLDER: icons_new_folder(); break;
    case POP_SETTINGS: run_named("Settings", "Wallpaper"); break;
    case POP_ABOUT: run_named("About", NULL); break;
    case POP_LAYOUT: set_layout(layout_names[popup_arguments[index]][0]); break;
    case POP_ICON: icons_menu(popup_icon, popup_arguments[index]); break;
    case POP_RESTART:
    case POP_POWER_OFF:
        printf("desktop: %s\n", popup_actions[index] == POP_RESTART ? "restarting" : "turning off");
        fflush(stdout);
        vx_power(popup_actions[index] == POP_RESTART ? VX_POWER_RESTART : VX_POWER_OFF);
        break;
    case POP_LEAVE: quit = true; break;
    case POP_LOGOUT: logging_out = quit = true; break;
    case POP_NONE: break;
    }
}

/* The names in /apps, as one number: when it changes, an app was added
 * (copied in: installed) or removed, and the menu and icons are made again. */
static unsigned long apps_signature(void) {
    unsigned long hash = 5381;
    int handle = vx_open(VX_APPS_DIR, VX_OPEN_READ);
    if (handle < 0) {
        return 0;
    }
    struct vx_dir_entry entries[16];
    long n;
    while ((n = vx_read_dir(handle, entries, 16)) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned long h = 5381; /* Each name's hash, added: the order doesn't matter. */
            for (const char *c = entries[i].name; *c; c++) {
                h = h * 33 + (unsigned char)*c;
            }
            hash += h;
        }
    }
    vx_close(handle);
    return hash;
}

static unsigned long apps_seen;
static long apps_checked_ms;
static void check_devices(void);

static void check_apps(void) {
    long now = vx_uptime();
    if (now - apps_checked_ms < 2000) {
        return;
    }
    apps_checked_ms = now;
    check_devices();
    unsigned long signature = apps_signature();
    if (signature == apps_seen) {
        return;
    }
    bool first = apps_seen == 0;
    apps_seen = signature;
    if (first) {
        return;
    }
    close_popup();
    set_menu(false);
    build_menu();
    icons_load();
    printf("desktop: apps changed: %d apps\n", app_count);
}

/* USB devices plugged in and out (every two seconds): a notification each;
 * a USB drive says where it's mounted. */
#define MAX_USB_SEEN 64
static unsigned usb_seen[MAX_USB_SEEN];
static char usb_seen_names[MAX_USB_SEEN][64];
static int usb_seen_count = -1; /* (-1: not looked yet; what's there at the start isn't news.) */
static unsigned long long devices_generation;

static void check_devices(void) {
    unsigned long long now = 0;
    vx_device_list(NULL, 0, &now);
    if (now == devices_generation) {
        return;
    }
    devices_generation = now;
    static struct vx_device_info list[256];
    long n = vx_device_list(list, 256, NULL);
    n = n < 0 ? 0 : n > 256 ? 256 : n;
    unsigned current[MAX_USB_SEEN];
    int current_count = 0;
    for (long i = 0; i < n && current_count < MAX_USB_SEEN; i++) {
        if (list[i].bus == VX_BUS_USB && list[i].kind != VX_DEVICE_USB_HUB) {
            current[current_count++] = list[i].id;
        }
    }
    if (usb_seen_count >= 0) {
        for (int c = 0; c < current_count; c++) {
            bool known = false;
            for (int j = 0; j < usb_seen_count; j++) {
                known = known || usb_seen[j] == current[c];
            }
            if (known) {
                continue;
            }
            const struct vx_device_info *d = NULL;
            for (long i = 0; i < n; i++) {
                if (list[i].id == current[c]) {
                    d = &list[i];
                }
            }
            /* A disk under it, mounted: where. */
            const char *mounted = NULL;
            for (long i = 0; d && i < n && !mounted; i++) {
                if (list[i].kind == VX_DEVICE_DISK && list[i].parent == d->id) {
                    const char *at = strstr(list[i].details, " at /");
                    mounted = at ? at + 4 : NULL;
                }
            }
            char text[128];
            if (mounted) {
                snprintf(text, sizeof(text), "Connected: %s, in %s", d->name, mounted);
            } else {
                snprintf(text, sizeof(text), "Connected: %s", d ? d->name : "a USB device");
            }
            add_note(text);
        }
        for (int j = 0; j < usb_seen_count; j++) {
            bool still = false;
            for (int c = 0; c < current_count; c++) {
                still = still || usb_seen[j] == current[c];
            }
            if (!still) {
                char text[128];
                snprintf(text, sizeof(text), "Disconnected: %s", usb_seen_names[j]);
                add_note(text);
            }
        }
    }
    for (int c = 0; c < current_count; c++) {
        usb_seen[c] = current[c];
        usb_seen_names[c][0] = '\0';
        for (long i = 0; i < n; i++) {
            if (list[i].id == current[c]) {
                snprintf(usb_seen_names[c], sizeof(usb_seen_names[c]), "%s", list[i].name);
            }
        }
    }
    usb_seen_count = current_count;
}

static void run_menu_item(const struct menu_item *item) {
    switch (item->action) {
    case LEAVE: ask(POP_LEAVE, 31, PANEL_HEIGHT + 4); return;
    case LOGOUT: ask(POP_LOGOUT, 31, PANEL_HEIGHT + 4); return;
    case RESTART: ask(POP_RESTART, pointer_x, pointer_y); return;
    case POWER_OFF: ask(POP_POWER_OFF, pointer_x, pointer_y); return;
    case LOCK: lock_now(); return;
    case RUN_APP: run_app(item->app); return;
    case RUN_LINUX_APP: break;
    case SEPARATOR: case SUBMENU: return;
    }
    const char *argv[MAX_ARGS + 1] = {"/linux/usr/bin/xrun"};
    for (int i = 0; i < item->arg_count; i++) {
        argv[i + 1] = item->args[i];
    }
    launch_argv(argv, item->arg_count + 1);
}

/* ---- The keyboard: layouts ----
 *
 * What a key types, by key code (Linux's, 0 to 86): a US keyboard, and the
 * other layouts' differences from it, in Unicode. Some keys are "dead": an
 * accent that goes on the next letter (^ then e: ê). X programs get the
 * whole layout from XKB instead. */
#define KEYS 87
static const char keymap[KEYS] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 "
                                 "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\\";
static const char keymap_shift[KEYS] =
    "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 "
    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0|";

/* Dead keys, in Unicode's private area. */
enum { DEAD_GRAVE = 0xe000, DEAD_ACUTE, DEAD_CIRCUMFLEX, DEAD_DIAERESIS, DEAD_TILDE };

struct key_change {
    unsigned char key;
    uint16_t normal, shifted, altgr; /* 0: nothing. */
};

static const struct key_change layout_gb[] = {
    {3, '2', '"', 0}, {4, '3', 0xa3, 0}, {5, '4', '$', 0x20ac}, {40, '\'', '@', 0},
    {41, '`', 0xac, 0xa6}, {43, '#', '~', 0}, {86, '\\', '|', 0}, {0, 0, 0, 0},
};
static const struct key_change layout_de[] = {
    {3, '2', '"', 0xb2}, {4, '3', 0xa7, 0xb3}, {7, '6', '&', 0}, {8, '7', '/', '{'},
    {9, '8', '(', '['}, {10, '9', ')', ']'}, {11, '0', '=', '}'}, {12, 0xdf, '?', '\\'},
    {13, DEAD_ACUTE, DEAD_GRAVE, 0}, {16, 'q', 'Q', '@'}, {18, 'e', 'E', 0x20ac},
    {21, 'z', 'Z', 0}, {26, 0xfc, 0xdc, 0}, {27, '+', '*', '~'}, {39, 0xf6, 0xd6, 0},
    {40, 0xe4, 0xc4, 0}, {41, DEAD_CIRCUMFLEX, 0xb0, 0}, {43, '#', '\'', 0}, {44, 'y', 'Y', 0},
    {50, 'm', 'M', 0xb5}, {51, ',', ';', 0}, {52, '.', ':', 0}, {53, '-', '_', 0},
    {86, '<', '>', '|'}, {0, 0, 0, 0},
};
static const struct key_change layout_fr[] = {
    {2, '&', '1', 0}, {3, 0xe9, '2', '~'}, {4, '"', '3', '#'}, {5, '\'', '4', '{'},
    {6, '(', '5', '['}, {7, '-', '6', '|'}, {8, 0xe8, '7', '`'}, {9, '_', '8', '\\'},
    {10, 0xe7, '9', '^'}, {11, 0xe0, '0', '@'}, {12, ')', 0xb0, ']'}, {13, '=', '+', '}'},
    {16, 'a', 'A', 0}, {17, 'z', 'Z', 0}, {18, 'e', 'E', 0x20ac},
    {26, DEAD_CIRCUMFLEX, DEAD_DIAERESIS, 0}, {27, '$', 0xa3, 0xa4}, {30, 'q', 'Q', 0},
    {39, 'm', 'M', 0}, {40, 0xf9, '%', 0}, {41, 0xb2, 0, 0}, {43, '*', 0xb5, 0},
    {44, 'w', 'W', 0}, {50, ',', '?', 0}, {51, ';', '.', 0}, {52, ':', '/', 0},
    {53, '!', 0xa7, 0}, {86, '<', '>', 0}, {0, 0, 0, 0},
};
static const struct key_change layout_es[] = {
    {2, '1', '!', '|'}, {3, '2', '"', '@'}, {4, '3', 0xb7, '#'}, {5, '4', '$', '~'},
    {6, '5', '%', 0x20ac}, {7, '6', '&', 0xac}, {8, '7', '/', 0}, {9, '8', '(', 0},
    {10, '9', ')', 0}, {11, '0', '=', 0}, {12, '\'', '?', 0}, {13, 0xa1, 0xbf, 0},
    {18, 'e', 'E', 0x20ac}, {26, DEAD_GRAVE, DEAD_CIRCUMFLEX, '['}, {27, '+', '*', ']'},
    {39, 0xf1, 0xd1, 0}, {40, DEAD_ACUTE, DEAD_DIAERESIS, '{'}, {41, 0xba, 0xaa, '\\'},
    {43, 0xe7, 0xc7, '}'}, {51, ',', ';', 0}, {52, '.', ':', 0}, {53, '-', '_', 0},
    {86, '<', '>', 0}, {0, 0, 0, 0},
};
static const struct key_change layout_dvorak[] = {
    {12, '[', '{', 0}, {13, ']', '}', 0}, {16, '\'', '"', 0}, {17, ',', '<', 0},
    {18, '.', '>', 0}, {19, 'p', 'P', 0}, {20, 'y', 'Y', 0},  {21, 'f', 'F', 0},
    {22, 'g', 'G', 0}, {23, 'c', 'C', 0}, {24, 'r', 'R', 0},  {25, 'l', 'L', 0},
    {26, '/', '?', 0}, {27, '=', '+', 0}, {31, 'o', 'O', 0},  {32, 'e', 'E', 0},
    {33, 'u', 'U', 0}, {34, 'i', 'I', 0}, {35, 'd', 'D', 0},  {36, 'h', 'H', 0},
    {37, 't', 'T', 0}, {38, 'n', 'N', 0}, {39, 's', 'S', 0},  {40, '-', '_', 0},
    {44, ';', ':', 0}, {45, 'q', 'Q', 0}, {46, 'j', 'J', 0},  {47, 'k', 'K', 0},
    {48, 'x', 'X', 0}, {49, 'b', 'B', 0}, {50, 'm', 'M', 0},  {51, 'w', 'W', 0},
    {52, 'v', 'V', 0}, {53, 'z', 'Z', 0}, {0, 0, 0, 0},
};

static const struct {
    const char *name;
    const struct key_change *changes;
} layouts[] = {
    {"us", NULL}, {"gb", layout_gb}, {"de", layout_de}, {"fr", layout_fr},
    {"es", layout_es}, {"dvorak", layout_dvorak},
};

/* What a key types in the chosen layout (without Ctrl and Caps Lock). */
static uint32_t layout_char(int key, bool shifted, bool with_altgr) {
    if (key < 0 || key >= KEYS) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        if (strcmp(layouts[i].name, setting_layout) || !layouts[i].changes) {
            continue;
        }
        for (const struct key_change *c = layouts[i].changes; c->key; c++) {
            if (c->key == key) {
                return with_altgr ? c->altgr : shifted ? c->shifted : c->normal;
            }
        }
    }
    if (with_altgr) {
        return 0;
    }
    return (unsigned char)(shifted ? keymap_shift[key] : keymap[key]);
}

/* An accent and a letter: one character (or 0 if they don't go together). */
static uint32_t compose_dead(uint32_t dead, uint32_t c) {
    static const char letters[] = "aeiouyAEIOUYn" "N";
    static const uint16_t made[5][14] = {
        /* grave */ {0xe0, 0xe8, 0xec, 0xf2, 0xf9, 0, 0xc0, 0xc8, 0xcc, 0xd2, 0xd9, 0, 0, 0},
        /* acute */ {0xe1, 0xe9, 0xed, 0xf3, 0xfa, 0xfd, 0xc1, 0xc9, 0xcd, 0xd3, 0xda, 0xdd, 0, 0},
        /* circumflex */ {0xe2, 0xea, 0xee, 0xf4, 0xfb, 0, 0xc2, 0xca, 0xce, 0xd4, 0xdb, 0, 0, 0},
        /* diaeresis */ {0xe4, 0xeb, 0xef, 0xf6, 0xfc, 0xff, 0xc4, 0xcb, 0xcf, 0xd6, 0xdc, 0, 0, 0},
        /* tilde */ {0xe3, 0, 0, 0xf5, 0, 0, 0xc3, 0, 0, 0xd5, 0, 0, 0xf1, 0xd1},
    };
    const char *at = c < 128 && c ? strchr(letters, (int)c) : NULL;
    return at ? made[dead - DEAD_GRAVE][at - letters] : 0;
}

static uint32_t accent_alone(uint32_t dead) {
    static const uint16_t alone[5] = {'`', 0xb4, '^', 0xa8, '~'};
    return alone[dead - DEAD_GRAVE];
}

static uint32_t pending_dead; /* A dead key waiting for its letter. */

static int character_of(int key) {
    uint32_t c = layout_char(key, shift, altgr);
    if (caps_lock) {
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) {
            c -= 0x20;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) {
            c += 0x20;
        }
    }
    if (ctrl && !altgr && c < 128 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        return (int)(c & 0x1f);
    }
    if (c >= DEAD_GRAVE && c <= DEAD_TILDE) {
        if (pending_dead) { /* Twice: the accent itself. */
            uint32_t accent = accent_alone(pending_dead);
            pending_dead = 0;
            return (int)accent;
        }
        pending_dead = c;
        return 0;
    }
    if (pending_dead && c) {
        uint32_t dead = pending_dead;
        pending_dead = 0;
        if (c == ' ') {
            return (int)accent_alone(dead);
        }
        uint32_t made = compose_dead(dead, c);
        return (int)(made ? made : c);
    }
    return (int)c;
}

/* ---- The keyboard: shortcuts ---- */

/* Super+Left/Right/Up/Down: halves, then quarters (Left then Up: the top
 * left quarter), maximized, back. */
static void super_arrow(int key) {
    struct window *w = focused;
    if (!w || !w->resizable) {
        return;
    }
    enum snap s = w->maximized ? SNAP_TOP : w->snapped;
    enum snap next = s;
    switch (key) {
    case VX_KEY_LEFT:
        next = s == SNAP_RIGHT ? SNAP_NONE : s == SNAP_TOP_RIGHT ? SNAP_TOP_LEFT
               : s == SNAP_BOTTOM_RIGHT ? SNAP_BOTTOM_LEFT : SNAP_LEFT;
        break;
    case VX_KEY_RIGHT:
        next = s == SNAP_LEFT ? SNAP_NONE : s == SNAP_TOP_LEFT ? SNAP_TOP_RIGHT
               : s == SNAP_BOTTOM_LEFT ? SNAP_BOTTOM_RIGHT : SNAP_RIGHT;
        break;
    case VX_KEY_UP:
        next = s == SNAP_LEFT ? SNAP_TOP_LEFT : s == SNAP_RIGHT ? SNAP_TOP_RIGHT
               : s == SNAP_BOTTOM_LEFT ? SNAP_LEFT : s == SNAP_BOTTOM_RIGHT ? SNAP_RIGHT
               : SNAP_TOP;
        break;
    case VX_KEY_DOWN:
        if (s == SNAP_NONE) {
            minimize(w);
            return;
        }
        next = s == SNAP_LEFT ? SNAP_BOTTOM_LEFT : s == SNAP_RIGHT ? SNAP_BOTTOM_RIGHT
               : s == SNAP_TOP_LEFT ? SNAP_LEFT : s == SNAP_TOP_RIGHT ? SNAP_RIGHT : SNAP_NONE;
        break;
    }
    if (next != s) {
        snap_window(w, next);
    }
}

/* Super+D: everything minimized (the desktop), and again: back. */
static struct window *shown_before[MAX_WINDOWS];
static int shown_before_count;

static void show_desktop(void) {
    int visible = 0;
    for (int i = 0; i < window_count; i++) {
        visible += !stack[i]->minimized && !stack[i]->popup;
    }
    if (visible) {
        shown_before_count = 0;
        for (int i = 0; i < window_count; i++) {
            if (!stack[i]->minimized && !stack[i]->popup) {
                shown_before[shown_before_count++] = stack[i];
            }
        }
        for (int i = 0; i < shown_before_count; i++) {
            minimize(shown_before[i]);
        }
        printf("desktop: showing the desktop\n");
        return;
    }
    for (int i = 0; i < shown_before_count; i++) {
        for (int k = 0; k < window_count; k++) {
            if (stack[k] == shown_before[i]) {
                activate(stack[k]);
            }
        }
    }
    shown_before_count = 0;
}

static void close_overlays(void) {
    set_menu(false);
    close_popup();
    clock_close();
    volume_close();
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
    case VX_KEY_LEFTALT: alt = down; break;
    case VX_KEY_RIGHTALT: altgr = down; break; /* AltGr: the third level, not Alt. */
    case VX_KEY_LEFTMETA:
    case 126: super_key = down; break;
    case VX_KEY_CAPSLOCK:
        if (value == 1) {
            caps_lock = !caps_lock;
        }
        break;
    }
    if (!lock_input()) {
        return;
    }
    if (volume_key(key, value)) { /* (Even on the lock screen.) */
        return;
    }
    if (locked) {
        lock_key(key, value, down ? character_of(key) : 0);
        return;
    }
    /* Alt+Shift: Shift and Alt pressed together with no other key, then let go. */
    static bool layout_chord;
    bool is_shift = key == VX_KEY_LEFTSHIFT || key == VX_KEY_RIGHTSHIFT;
    if (value == 1 && ((is_shift && alt) || (key == VX_KEY_LEFTALT && shift))) {
        layout_chord = true;
    } else if (value == 1 && !is_shift && key != VX_KEY_LEFTALT) {
        layout_chord = false;
    } else if (value == 0 && layout_chord && (is_shift || key == VX_KEY_LEFTALT)) {
        layout_chord = false;
        switch_layout();
    }
    if (key == 99 && value == 1) { /* PrintScreen */
        screenshot(alt ? SHOT_WINDOW : shift ? SHOT_AREA : SHOT_SCREEN);
        return;
    }
    if (shot_selecting) {
        if (key == VX_KEY_ESC && value == 1) {
            shot_cancel();
        }
        return;
    }
    if (switcher_open) {
        if (key == VX_KEY_LEFTALT && !down) {
            switcher_finish(true);
        } else if (key == VX_KEY_TAB && down) {
            switcher_step(shift);
        } else if (key == VX_KEY_ESC && down) {
            switcher_finish(false);
        }
        return;
    }
    if (alt && !ctrl && key == VX_KEY_TAB && down) {
        switcher_start(shift);
        return;
    }
    if (search_open) {
        search_key(key, value, down ? character_of(key) : 0);
        return;
    }
    if ((ctrl || super_key) && key == VX_KEY_SPACE && value == 1) {
        close_overlays();
        search_show();
        return;
    }
    if (super_key && down) {
        int letter = (int)layout_char(key, false, false);
        if (key == VX_KEY_LEFT || key == VX_KEY_RIGHT || key == VX_KEY_UP || key == VX_KEY_DOWN) {
            super_arrow(key);
            return;
        }
        if (letter == 'l' && value == 1) {
            lock_now();
            return;
        }
        if (letter == 'd' && value == 1) {
            show_desktop();
            return;
        }
    }
    if (alt && !ctrl && key == 62 && value == 1) { /* F4 */
        if (focused) {
            ask_to_close(focused);
        }
        return;
    }
    if (ctrl && alt && value == 1) {
        int letter = (int)layout_char(key, false, false);
        /* Apps' shortcuts ("Ctrl+Alt+T"). */
        for (int i = 0; i < app_count; i++) {
            const char *s = apps[i].shortcut;
            if (!strncmp(s, "Ctrl+Alt+", 9) && s[9] && !s[10] && letter == (s[9] | 0x20)) {
                run_app(i);
                return;
            }
        }
        if (letter == 'q') {
            ask(POP_LEAVE, screen.width / 2 - 90, screen.height / 3);
            return;
        }
        if (letter == 'l') {
            lock_now();
            return;
        }
    }
    if (popup_open && value && (key == VX_KEY_UP || key == VX_KEY_DOWN || key == VX_KEY_ENTER)) {
        /* The menu by keyboard: Up and Down choose, Enter runs (a question's
         * first answer, if nothing was chosen). */
        if (key == VX_KEY_ENTER) {
            int item = popup_hot >= 0 ? popup_hot : 0;
            close_popup();
            printf("desktop: menu item \"%s\"\n", popup_items[item].label);
            run_popup_item(item);
            return;
        }
        int step = key == VX_KEY_DOWN ? 1 : popup_count - 1;
        int hot = popup_hot < 0 ? (key == VX_KEY_DOWN ? popup_count - 1 : 0) : popup_hot;
        do {
            hot = (hot + step) % popup_count;
        } while (!popup_items[hot].label);
        popup_hot = hot;
        add_damage(popup_rect());
        return;
    }
    if (key == VX_KEY_ESC && value == 1 && (menu_open || popup_open || clock_open || volume_open)) {
        close_overlays();
        return;
    }
    if (focused) {
        struct desktop_message m = {.type = DESKTOP_KEY, .window = (uint32_t)focused->id,
                                    .a = key, .b = value, .c = down ? character_of(key) : 0};
        send_to(focused->client, &m);
    }
}

/* ---- The pointer ---- */

/* Buttons pressed on the desktop's own things (the panel, menus): a window
 * that appears under the pointer before they're let go doesn't see them. */
static int desktop_buttons;

static void send_pointer(struct window *w, int wheel) {
    struct desktop_message m = {.type = DESKTOP_POINTER, .window = (uint32_t)w->id,
                                .a = pointer_x - w->x, .b = pointer_y - w->y,
                                .c = buttons & ~desktop_buttons,
                                .d = wheel};
    send_to(w->client, &m);
}

/* Which edges of a resizable window the pointer is on (for resizing). */
static bool on_edges(struct window *w, int x, int y, bool *left, bool *right, bool *bottom) {
    if (!w->resizable || w->maximized || w->popup || w->undecorated) {
        return false;
    }
    struct rect f = frame_rect(w);
    if (y < f.y + TITLE_HEIGHT / 2) {
        return false; /* The title bar is for moving. */
    }
    *left = x < f.x + EDGE;
    *right = x >= f.x + f.width - EDGE;
    *bottom = y >= f.y + f.height - EDGE;
    return *left || *right || *bottom;
}

static int edge_cursor(bool left, bool right, bool bottom) {
    return bottom && right ? CURSOR_RESIZE_NWSE : bottom && left ? CURSOR_RESIZE_NESW
           : bottom ? CURSOR_RESIZE_NS : CURSOR_RESIZE_EW;
}

static void set_cursor(int shape) {
    if (shape != cursor_shape) {
        add_damage(cursor_rect(cursor_shape, pointer_x, pointer_y));
        cursor_shape = shape;
        add_damage(cursor_rect(cursor_shape, pointer_x, pointer_y));
    }
}

static void update_cursor(void) {
    if (shot_selecting) {
        set_cursor(VX_CURSOR_CROSS);
        return;
    }
    if (locked || saver_on || search_open || switcher_open) {
        set_cursor(VX_CURSOR_ARROW);
        return;
    }
    if (drag == RESIZING) {
        set_cursor(edge_cursor(resize_left, resize_right, resize_bottom));
        return;
    }
    if (drag == MOVING) {
        set_cursor(VX_CURSOR_ARROW);
        return;
    }
    if (grab) {
        set_cursor(grab->cursor);
        return;
    }
    struct window *w = menu_open || popup_open || clock_open || volume_open
                           ? NULL : window_at(pointer_x, pointer_y);
    bool left, right, bottom;
    if (w && on_edges(w, pointer_x, pointer_y, &left, &right, &bottom)) {
        set_cursor(edge_cursor(left, right, bottom));
    } else if (w && pointer_y >= w->y && pointer_x >= w->x && pointer_x < w->x + w->content.width &&
               pointer_y < w->y + w->content.height) {
        set_cursor(w->cursor);
    } else {
        set_cursor(VX_CURSOR_ARROW);
    }
}

/* A click on the panel. */
static void panel_click(void) {
    if (pointer_x < MENU_BUTTON_WIDTH + 4) {
        clock_close();
        set_menu(!menu_open);
        return;
    }
    set_menu(false);
    if (inside(clock_button_rect(), pointer_x, pointer_y)) {
        volume_close();
        clock_toggle();
        return;
    }
    clock_close();
    if (inside(volume_button_rect(), pointer_x, pointer_y)) {
        volume_toggle();
        return;
    }
    volume_close();
    if (inside(layout_button_rect(), pointer_x, pointer_y)) {
        popup_count = 0;
        popup_icon = -1;
        for (int i = 0; i < LAYOUT_NAMES; i++) {
            popup_add(layout_names[i][1], strcmp(layout_names[i][0], setting_layout) ? NULL : "\u2713",
                      POP_LAYOUT, i);
        }
        place_popup(layout_button_rect().x, PANEL_HEIGHT + 2);
        return;
    }
    if (inside(search_button_rect(), pointer_x, pointer_y)) {
        search_show();
        return;
    }
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
    int button = title_button_at(w, pointer_x, pointer_y);
    if (button == 0) {
        ask_to_close(w);
        return;
    }
    if (w->resizable && (button == 1 || button == 2)) {
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
    if (last_click_window == w && now - last_click_ms < DOUBLE_CLICK_MS &&
        strcmp(setting_title_click, "none")) {
        last_click_window = NULL;
        if (!strcmp(setting_title_click, "minimize")) {
            minimize(w);
        } else {
            toggle_maximized(w);
        }
        return;
    }
    last_click_window = w;
    last_click_ms = now;
    start_move(w);
}

static void end_drag(void) {
    struct window *d = dragged;
    if (drag == MOVING && d && snap != SNAP_NONE) {
        /* Snapped: maximized, a half or a quarter of the screen. */
        add_damage(snap_damage(snap));
        enum snap where = snap;
        snap = SNAP_NONE;
        snap_window(d, where);
    } else if (drag == MOVING && d) {
        send_moved(d);
        printf("desktop: moved window %d to %d,%d\n", d->id, d->x, d->y);
    } else if (drag == RESIZING && d) {
        add_damage(outline_damage());
        if (outline.width != d->content.width || outline.height != d->content.height) {
            configure(d, outline.width, outline.height);
        }
        if (outline.x != d->x) {
            add_damage(window_damage(d));
            d->x = outline.x;
            send_moved(d);
        }
        d->snapped = SNAP_NONE;
    }
    drag = IDLE;
    dragged = NULL;
}

static void button_event(int bit, bool down) {
    int before = buttons;
    buttons = down ? buttons | bit : buttons & ~bit;
    if (!lock_input()) {
        return;
    }
    if (locked) {
        lock_button(down && bit == 1);
        return;
    }
    bool left_down = bit == 1 && down && !(before & 1);
    bool right_down = bit == 2 && down && !(before & 2);
    if (!down) {
        desktop_buttons &= ~bit;
    } else if (pointer_y < PANEL_HEIGHT || menu_open || popup_open || search_open || clock_open ||
               volume_open) {
        desktop_buttons |= bit;
    }
    if (left_down) {
        printf("desktop: left button at %d,%d\n", pointer_x, pointer_y);
    }
    if (shot_selecting) {
        if (bit == 1) {
            shot_button(down);
        } else if (right_down) {
            shot_cancel();
        }
        update_cursor();
        return;
    }
    if (switcher_open) {
        if (left_down) {
            switcher_click();
        }
        return;
    }
    if (search_open) {
        if (left_down || right_down) {
            search_button(true);
        }
        return;
    }
    if ((left_down || right_down) && popup_open) {
        /* A click on an item runs it; anywhere else it just closes the menu. */
        int item = vx_menu_item_at(popup_items, popup_count, popup_x, popup_y, pointer_x, pointer_y);
        close_popup();
        if (item >= 0) {
            printf("desktop: menu item \"%s\"\n", popup_items[item].label);
            run_popup_item(item);
        }
        return;
    }
    if (bit == 1 && !down && volume_sliding()) {
        volume_button(false);
        return;
    }
    if ((left_down || right_down) && volume_open) {
        if (inside(volume_rect(), pointer_x, pointer_y)) {
            if (left_down) {
                volume_button(true);
            }
            return;
        }
        volume_close();
        if (inside(volume_button_rect(), pointer_x, pointer_y)) {
            return; /* Its own button: closed, not opened again. */
        }
    }
    if ((left_down || right_down) && clock_open) {
        if (inside(clock_rect(), pointer_x, pointer_y)) {
            clock_button(true);
            return;
        }
        clock_close();
        if (inside(clock_button_rect(), pointer_x, pointer_y)) {
            return; /* Its own button: closed, not opened again. */
        }
    }
    if ((left_down || right_down) && installer_only && !window_at(pointer_x, pointer_y)) {
        return; /* (Nothing but the wallpaper there.) */
    }
    if (right_down && pointer_y >= PANEL_HEIGHT && !window_at(pointer_x, pointer_y)) {
        set_menu(false);
        open_popup(icons_at(pointer_x, pointer_y));
        return;
    }
    if (left_down && menu_open && pointer_y >= PANEL_HEIGHT) {
        /* A click on an item runs it (or opens its submenu); anywhere else it
         * just closes the menu. */
        int sub = group_item_at(pointer_x, pointer_y);
        if (sub >= 0) {
            struct menu_item chosen = group_items[menu_group][sub];
            set_menu(false);
            run_menu_item(&chosen);
            return;
        }
        if (menu_group >= 0 && inside(group_rect(), pointer_x, pointer_y)) {
            return;
        }
        int item = menu_item_at(pointer_x, pointer_y);
        if (item >= 0 && menu_items[item].action == SUBMENU) {
            menu_group = menu_items[item].group;
            group_hot = -1;
            add_damage(menu_damage());
            return;
        }
        if (item >= 0 || !inside(menu_rect(), pointer_x, pointer_y)) {
            set_menu(false);
        }
        if (item >= 0) {
            struct menu_item chosen = menu_items[item];
            run_menu_item(&chosen);
        }
        return;
    }
    if (left_down && pointer_y < PANEL_HEIGHT) {
        panel_click();
        return;
    }
    struct window *w = window_at(pointer_x, pointer_y);
    if (left_down && !w) {
        desktop_press = true;
        icons_button(1, true, DOUBLE_CLICK_MS);
        return;
    }
    if (bit == 1 && !down && desktop_press) {
        desktop_press = false;
        icons_button(1, false, DOUBLE_CLICK_MS);
        update_cursor();
        return;
    }
    if (left_down && w) {
        activate(w);
        bool left, right, bottom;
        if (on_edges(w, pointer_x, pointer_y, &left, &right, &bottom)) {
            start_resize(w, left, right, bottom);
            update_cursor();
            return;
        }
        if (pointer_y < w->y && !w->popup && !w->undecorated) {
            title_click(w);
            update_cursor();
            return;
        }
    }
    if (bit == 1 && !down && drag != IDLE) {
        end_drag();
        update_cursor();
        return;
    }
    /* The window the pointer is in (or the one a button went down in). */
    if (down && !(before & 7) && w && drag == IDLE && pointer_y >= w->y &&
        pointer_x < w->x + w->content.width && pointer_y < w->y + w->content.height) {
        grab = w;
    }
    if (grab) {
        send_pointer(grab, 0);
        if (!buttons) {
            grab = NULL;
            update_cursor();
        }
        return;
    }
    if (w && drag == IDLE && pointer_y >= w->y && pointer_x < w->x + w->content.width &&
        pointer_y < w->y + w->content.height) {
        send_pointer(w, 0);
    }
}

static void pointer_moved(int dx, int dy, int wheel) {
    if ((dx || dy || wheel) && !lock_input()) {
        return;
    }
    if (locked) {
        return;
    }
    if (wheel && !dx && !dy && inside(volume_button_rect(), pointer_x, pointer_y)) {
        volume_wheel(wheel); /* (Scrolling over the panel's speaker.) */
        return;
    }
    if (dx || dy) {
        add_damage(cursor_rect(cursor_shape, pointer_x, pointer_y));
        pointer_x += dx;
        pointer_y += dy;
        pointer_x = pointer_x < 0 ? 0 : pointer_x >= screen.width ? screen.width - 1 : pointer_x;
        pointer_y = pointer_y < 0 ? 0 : pointer_y >= screen.height ? screen.height - 1 : pointer_y;
        add_damage(cursor_rect(cursor_shape, pointer_x, pointer_y));
        if (shot_selecting) {
            shot_pointer();
            update_cursor();
            return;
        }
        if (search_open) {
            search_pointer();
        }
        if (clock_open) {
            clock_pointer();
        }
        if (volume_open) {
            volume_pointer();
        }
        if (desktop_press) {
            icons_pointer();
        }
        if (drag == MOVING && dragged) {
            add_damage(window_damage(dragged));
            dragged->x = pointer_x - drag_dx;
            dragged->y = pointer_y - drag_dy;
            int top = dragged->undecorated ? PANEL_HEIGHT : PANEL_HEIGHT + TITLE_HEIGHT + BORDER;
            if (dragged->y < top) {
                dragged->y = top; /* Keep the title bar out from under the panel. */
            }
            add_damage(window_damage(dragged));
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
            if (resize_left) {
                int x = pointer_x + drag_dx;
                x = drag_right - x < MIN_WIDTH ? drag_right - MIN_WIDTH : x;
                outline.x = x;
                outline.width = drag_right - x;
            } else if (resize_right) {
                outline.width = pointer_x + drag_dx - outline.x;
                outline.width = outline.width < MIN_WIDTH ? MIN_WIDTH : outline.width;
            }
            if (resize_bottom) {
                outline.height = pointer_y + drag_dy - outline.y;
                outline.height = outline.height < MIN_HEIGHT ? MIN_HEIGHT : outline.height;
            }
            add_damage(outline_damage());
        }
        if (menu_open) {
            /* Over the open submenu, its items light up; over the menu, an
             * item with a submenu opens it, and any other closes it. */
            if (menu_group >= 0 && inside(group_rect(), pointer_x, pointer_y)) {
                int hot = group_item_at(pointer_x, pointer_y);
                if (hot != group_hot) {
                    group_hot = hot;
                    add_damage(menu_damage());
                }
            } else {
                int hot = menu_item_at(pointer_x, pointer_y);
                if (hot != menu_hot) {
                    menu_hot = hot;
                    if (hot >= 0) {
                        int group = menu_items[hot].action == SUBMENU ? menu_items[hot].group : -1;
                        if (group != menu_group) {
                            menu_group = group;
                            group_hot = -1;
                        }
                    }
                    add_damage(menu_damage());
                }
            }
        }
        if (popup_open) {
            int hot = vx_menu_item_at(popup_items, popup_count, popup_x, popup_y, pointer_x,
                                      pointer_y);
            if (hot != popup_hot) {
                popup_hot = hot;
                add_damage(popup_rect());
            }
        }
        /* The title bar's buttons light up under the pointer. */
        struct window *under = drag == IDLE && !grab ? window_at(pointer_x, pointer_y) : NULL;
        int button = under ? title_button_at(under, pointer_x, pointer_y) : -1;
        if (button < 0) {
            under = NULL;
        }
        if (under != hot_window || button != hot_button) {
            if (hot_window) {
                add_damage(frame_rect(hot_window));
            }
            hot_window = under;
            hot_button = button;
            if (hot_window) {
                add_damage(frame_rect(hot_window));
            }
        }
        update_cursor();
        if (drag != IDLE || desktop_press) {
            return;
        }
    }
    if (search_open || switcher_open || shot_selecting) {
        return;
    }
    if (grab) {
        send_pointer(grab, wheel);
        return;
    }
    struct window *w = window_at(pointer_x, pointer_y);
    if (w && pointer_y >= w->y && pointer_x < w->x + w->content.width &&
        pointer_y < w->y + w->content.height) {
        send_pointer(w, wheel);
    }
}

/* The mouse's motion at the chosen speed (5 is as it comes), keeping the
 * fractions for the next time; the wheel the chosen way round. */
static void pointer_input(int dx, int dy, int wheel) {
    static int rest_x, rest_y;
    if (setting_pointer_speed != 5) {
        rest_x += dx * setting_pointer_speed;
        rest_y += dy * setting_pointer_speed;
        dx = rest_x / 5;
        dy = rest_y / 5;
        rest_x -= dx * 5;
        rest_y -= dy * 5;
    }
    pointer_moved(dx, dy, setting_natural_scroll ? -wheel : wheel);
}

/* A tablet's position (0 to VX_ABS_MAX across the screen), where the pointer
 * goes: motion as it is, not at the chosen speed. */
static void pointer_to(int abs_x, int abs_y) {
    int x = abs_x < 0 ? pointer_x : (int)((long)abs_x * (screen.width - 1) / VX_ABS_MAX);
    int y = abs_y < 0 ? pointer_y : (int)((long)abs_y * (screen.height - 1) / VX_ABS_MAX);
    pointer_moved(x - pointer_x, y - pointer_y, 0);
}

static void read_input(int handle) {
    struct vx_input_event events[64];
    long n = vx_read(handle, events, sizeof(events));
    int dx = 0, dy = 0, wheel = 0, abs_x = -1, abs_y = -1;
    for (long i = 0; i < n / (long)sizeof(events[0]); i++) {
        struct vx_input_event *e = &events[i];
        if (abs_x >= 0 || abs_y >= 0) {
            if (e->type != VX_EV_ABS) { /* (A position comes before what it goes with.) */
                pointer_to(abs_x, abs_y);
                abs_x = abs_y = -1;
            }
        }
        if (e->type == VX_EV_KEY && e->code >= VX_BTN_LEFT && e->code <= VX_BTN_MIDDLE) {
            pointer_input(dx, dy, wheel);
            dx = dy = wheel = 0;
            int bit = e->code == VX_BTN_LEFT ? 1 : e->code == VX_BTN_RIGHT ? 2 : 4;
            if (setting_left_handed && bit != 4) {
                bit ^= 3; /* The buttons swapped. */
            }
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
        } else if (e->type == VX_EV_ABS) {
            if (e->code == VX_ABS_X) {
                abs_x = e->value;
            } else if (e->code == VX_ABS_Y) {
                abs_y = e->value;
            }
        } else if (e->type == VX_EV_SYN && (dx || dy || wheel)) {
            pointer_input(dx, dy, wheel);
            dx = dy = wheel = 0;
        }
    }
}

/* ---- The display: its mode, and scaling ---- */

static int display_handle = -1;
static unsigned boot_width, boot_height; /* The mode the desktop started in. */
static size_t frame_size;

/* The screen buffers, for the size the desktop works in (the display's,
 * or half of it when everything is twice as big). */
static bool size_buffers(void) {
    int w = (int)display.width / setting_scale, h = (int)display.height / setting_scale;
    if (screen.pixels && w == screen.width && h == screen.height) {
        return true;
    }
    if (screen.pixels) {
        size_t old = (size_t)screen.width * screen.height * 4;
        vx_unmap(screen.pixels, old);
        vx_unmap(wallpaper.pixels, old);
    }
    screen.width = wallpaper.width = w;
    screen.height = wallpaper.height = h;
    screen.stride = wallpaper.stride = w;
    size_t size = (size_t)w * h * 4;
    screen.pixels = vx_map(size, VX_MAP_WRITE);
    wallpaper.pixels = vx_map(size, VX_MAP_WRITE);
    return screen.pixels && wallpaper.pixels;
}

/* Puts the display in the mode the settings ask for (the one it started
 * in if none): true if the size the desktop works in changed. */
static bool apply_display(void) {
    unsigned want_w = setting_width > 0 ? (unsigned)setting_width : boot_width;
    unsigned want_h = setting_height > 0 ? (unsigned)setting_height : boot_height;
    if (want_w != display.width || want_h != display.height) {
        struct vx_display_mode mode = {want_w, want_h};
        long error = vx_control(display_handle, VX_DISPLAY_SET_MODE, &mode, sizeof(mode));
        if (error) {
            printf("desktop: can't show %ux%u: %s\n", want_w, want_h, vx_strerror(error));
        } else {
            if (frame) {
                vx_unmap(frame, frame_size);
            }
            vx_control(display_handle, VX_DISPLAY_INFO, &display, sizeof(display));
            frame = vx_map_file(display_handle, 0, display.size, VX_MAP_WRITE);
            frame_size = display.size;
            printf("desktop: display now %ux%u\n", display.width, display.height);
        }
    }
    int old_w = screen.width, old_h = screen.height;
    if (!size_buffers()) {
        fprintf(stderr, "desktop: out of memory for a %ux%u screen\n", display.width, display.height);
        quit = true;
    }
    return old_w != screen.width || old_h != screen.height;
}

/* After the screen's size changed: windows back on it, maximized and
 * snapped ones as big as it now is. */
static void fit_windows(void) {
    struct rect area = work_area();
    for (int i = 0; i < window_count; i++) {
        struct window *w = stack[i];
        if (w->maximized) {
            w->x = area.x;
            w->y = area.y;
            configure(w, area.width, area.height);
            send_moved(w);
            continue;
        }
        if (w->snapped != SNAP_NONE) {
            struct rect r = snap_rect(w->snapped);
            w->x = r.x;
            w->y = r.y;
            configure(w, r.width, r.height);
            send_moved(w);
            continue;
        }
        int max_x = screen.width - 60, max_y = screen.height - 30;
        if (w->x > max_x || w->y > max_y) {
            w->x = w->x > max_x ? max_x : w->x;
            w->y = w->y > max_y ? max_y : w->y;
            send_moved(w);
        }
    }
    pointer_x = pointer_x >= screen.width ? screen.width - 1 : pointer_x;
    pointer_y = pointer_y >= screen.height ? screen.height - 1 : pointer_y;
    lock_screen_changed();
}

/* How a held key repeats. */
static void apply_key_repeat(void) {
    struct vx_key_repeat repeat = {(unsigned)setting_key_delay, (unsigned)setting_key_rate};
    if (keyboard >= 0) {
        vx_control(keyboard, VX_INPUT_SET_REPEAT, &repeat, sizeof(repeat));
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
        int grab_it = 1;
        if (vx_control(handle, VX_INPUT_INFO, &info, sizeof(info)) == 0 &&
            (info.capabilities & capability) &&
            vx_control(handle, VX_INPUT_GRAB, &grab_it, sizeof(grab_it)) == 0) {
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
    display_handle = handle;
    boot_width = display.width;
    boot_height = display.height;
    frame = vx_map_file(handle, 0, display.size, VX_MAP_WRITE);
    frame_size = display.size;
    read_config();
    apply_display();
    if (!frame || !screen.pixels || !wallpaper.pixels) {
        fprintf(stderr, "desktop: out of memory\n");
        return false;
    }
    make_wallpaper();
    build_menu();
    keyboard = open_input(VX_INPUT_KEYS);
    mouse = open_input(VX_INPUT_POINTER);
    apply_key_repeat();

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
    damage_all();
    return true;
}

static bool listed_app(const char *list, const struct vx_app *app) {
    const char *file = strrchr(app->bundle, '/');
    file = file ? file + 1 : app->bundle;
    size_t n = strlen(file) - strlen(VX_APP_EXTENSION);
    for (const char *p = list; *p;) {
        size_t m = strcspn(p, ",");
        if (m == n && !strncmp(p, file, n)) {
            return true;
        }
        p += m + (p[m] == ',');
    }
    return false;
}

/* ---- Sessions: who uses the desktop ---- */

/* Starts `user`'s session: the desktop becomes them (when it runs as root),
 * with their settings, folders and startup apps. */
void session_start(const struct vx_user *user) {
    struct vx_credentials me = {0};
    vx_credentials(NULL, &me);
    session_user = *user;
    if (me.uid == 0 && user->uid != 0) {
        session_has_password = vx_user_has_password(user->name);
        /* Its socket is theirs too, theirs alone (other accounts can't
         * connect to their desktop); the desktop takes it away when it ends. */
        vx_chown(DESKTOP_SOCKET, user->uid, user->gid, 0);
        vx_chmod(DESKTOP_SOCKET, 0600, 0);
        long error = vx_become_user(user);
        if (error) {
            fprintf(stderr, "desktop: can't become %s: %s\n", user->name, vx_strerror(error));
        }
    } else if (me.uid == 0) {
        session_has_password = vx_user_has_password(user->name);
    } else {
        session_has_password = !vx_password_check(user->name, "");
    }
    vx_chdir(user->home);
    snprintf(home_folder, sizeof(home_folder), "%s", user->home);
    vx_home_path(desktop_folder, sizeof(desktop_folder), "Desktop");
    vx_home_path(pictures_folder, sizeof(pictures_folder), "Pictures");
    vx_home_path(trash_folder, sizeof(trash_folder), VX_TRASH_NAME);
    /* The folders the desktop uses (vinit makes them; a desktop started
     * another way still has them). */
    vx_mkdir(HOME);
    vx_mkdir(DESKTOP_FOLDER);
    vx_mkdir(PICTURES_FOLDER);
    vx_mkdir(TRASH_FOLDER);
    reload_settings(); /* Theirs now (see <vexa/settings.h>). */
    printf("desktop: session for %s (user %u)\n", user->name, user->uid);
    /* The first program: a terminal, unless told otherwise (or Settings
     * says not to); then the apps Settings chose to start with the desktop.
     * (--boot: started by vinit when the computer starts, where a terminal
     * isn't wanted unless Settings asks for one; then maybe a program.) */
    bool boot = boot_argc > 1 && strcmp(boot_argv[1], "--boot") == 0;
    if (boot_argc > (boot ? 2 : 1)) {
        const char *first = boot_argv[boot ? 2 : 1];
        size_t n = strlen(first);
        if (boot && n >= 9 && !strcmp(first + n - 9, "installer")) {
            installer_only = true;
            damage_all();
        }
        launch(first);
    } else if (vx_settings_bool(&config, "startup_terminal", !boot)) {
        launch("/bin/term");
    }
    const char *startup = vx_settings_get(&config, "startup_apps", "");
    for (int i = 0; i < app_count && startup[0]; i++) {
        if (listed_app(startup, &apps[i])) {
            printf("desktop: starting %s (at startup)\n", apps[i].name);
            run_app(i);
        }
    }
}

/* At the start: someone to log in. A desktop a user started is theirs; with
 * one account and no password, it's that one's; otherwise the login screen
 * asks. */
static void begin(void) {
    struct vx_credentials me = {0};
    vx_credentials(NULL, &me);
    struct vx_user user;
    if (me.uid == 0) {
        vx_remove(DESKTOP_CLIPBOARD_FILE); /* The last session's (whoever's it was). */
    }
    if (me.uid != 0) {
        if (vx_user_by_id(me.uid, &user)) {
            user = (struct vx_user){.uid = me.uid, .gid = me.gid, .home = "/tmp"};
            snprintf(user.name, sizeof(user.name), "%u", me.uid);
        }
        session_start(&user);
        return;
    }
    static struct vx_user users[VX_USERS_MAX];
    int n = vx_users(users, VX_USERS_MAX);
    if (n == 0) {
        if (vx_user_by_id(0, &user)) {
            user = (struct vx_user){.name = "root", .home = "/root"};
        }
        session_start(&user);
    } else if (n == 1 && !vx_user_has_password(users[0].name)) {
        printf("desktop: logging in %s (no password)\n", users[0].name);
        session_start(&users[0]);
    } else {
        login_begin(users, n);
    }
}

/* Logging out: everything the session started goes too. */
static void end_session(void) {
    struct vx_process_info list[256];
    long n = vx_process_list(list, 256);
    long me = vx_process_id();
    for (long i = 0; i < n && i < 256; i++) {
        if (list[i].uid == session_user.uid && list[i].id != (unsigned int)me && list[i].state == 0) {
            vx_kill(list[i].id, VX_SIGTERM);
        }
    }
    printf("desktop: %s logged out\n", session_user.name);
}

int main(int argc, char **argv) {
    if (!setup()) {
        return 1;
    }
    printf("desktop: started on a %dx%d screen%s%s\n", screen.width, screen.height,
           keyboard >= 0 ? ", keyboard" : "", mouse >= 0 ? ", mouse" : "");
    fflush(stdout);
    boot_argc = argc;
    boot_argv = argv;
    begin();

    while (!quit) {
        long tick = setting_clock_seconds ? vx_time() : vx_time() / 60;
        if (tick != shown_minute) { /* The clock. */
            shown_minute = tick;
            /* The automatic theme: dark when night falls, light in the morning. */
            if (!strcmp(vx_settings_get(&config, "theme", "light"), "auto") &&
                vx_theme.dark != vx_theme_night()) {
                reload_settings();
            }
            add_damage(panel_rect());
            if (clock_open) {
                add_damage(clock_rect());
            }
        }
        frames_wanted = false;
        volume_check(); /* (At most twice a second.) */
        animate();
        shot_tick();
        lock_tick();
        check_apps(); /* (At most every two seconds.) */
        icons_check();
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
        long most = setting_clock_seconds ? 250 : 500;
        wait = wait < 0 || wait > most ? most : wait + 1;
        long lock_wait = lock_wait_ms();
        wait = lock_wait < wait ? lock_wait : wait;
        long osd_wait = volume_osd_wait();
        wait = osd_wait >= 0 && osd_wait + 1 < wait ? osd_wait + 1 : wait;
        if (frames_wanted) {
            wait = 16; /* About 60 frames a second. */
        }
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
    vx_remove(DESKTOP_SOCKET);
    if (logging_out) {
        end_session();
        return DESKTOP_EXIT_LOGOUT; /* vinit starts the desktop again. */
    }
    printf("desktop: back to the console\n");
    return 0; /* Closing the display brings the console back. */
}
