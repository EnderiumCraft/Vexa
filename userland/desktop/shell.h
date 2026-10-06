/* What the desktop's parts share: main.c (windows, the panel, input),
 * look.c (shadows, scaling, pointer shapes), icons.c (the icons on the
 * desktop, and the Desktop folder), switcher.c (Alt+Tab), search.c (Ctrl+Space),
 * clock.c (the calendar and the notifications), shot.c (screenshots) and
 * lock.c (the screensaver and the lock screen). */
#ifndef DESKTOP_SHELL_H
#define DESKTOP_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/app.h>
#include <vexa/gui.h>
#include <vexa/settings.h>
#include <vexa/time.h>
#include <vexa/users.h>

#define MAX_WINDOWS 32
#define PANEL_HEIGHT 26
#define TITLE_HEIGHT 28
#define BORDER 1
#define SHADOW 12 /* How far a window's shadow reaches. */

/* The folders a desktop has: the logged-in account's (main.c sets them). */
extern char home_folder[256], desktop_folder[300], pictures_folder[300], trash_folder[300];
#define HOME home_folder
#define DESKTOP_FOLDER desktop_folder
#define PICTURES_FOLDER pictures_folder
#define TRASH_FOLDER trash_folder

struct rect {
    int x, y, width, height;
};

/* Where a window snaps: the top (maximized), a half, or a quarter. */
enum snap {
    SNAP_NONE, SNAP_TOP, SNAP_LEFT, SNAP_RIGHT,
    SNAP_TOP_LEFT, SNAP_TOP_RIGHT, SNAP_BOTTOM_LEFT, SNAP_BOTTOM_RIGHT,
};

/* Windows open (grow and fade in), minimize to their panel button and come
 * back from it, and fade out when they close. */
enum animation { ANIM_NONE, ANIM_OPEN, ANIM_MINIMIZE, ANIM_RESTORE };

struct window {
    int id;
    int client; /* Index in clients[]. */
    char title[64];
    int x, y; /* The content's top-left corner on the screen. */
    struct vx_surface content;
    size_t mapped_size;
    int buffer_handle;
    bool resizable, minimized, maximized;
    bool modified;    /* Its program has changes that aren't saved (DESKTOP_MODIFIED). */
    bool popup;       /* A menu or tooltip: no frame, always on top, no keyboard. */
    bool undecorated; /* It draws its own title bar: no frame. */
    struct rect restore; /* Where it was before it was maximized or snapped (content). */
    enum snap snapped;
    int cursor; /* VX_CURSOR_* over its content. */
    enum animation animation;
    long animation_start;
};

extern struct vx_surface screen, wallpaper;
extern struct window *stack[MAX_WINDOWS]; /* Bottom to top. */
extern int window_count;
extern struct window *focused;
extern int pointer_x, pointer_y, buttons;
extern bool shift, ctrl, alt, super_key;
extern struct vx_settings config;
extern char setting_layout[16];

/* The apps in /apps, and their icons. */
#define MAX_APPS 32
extern struct vx_app apps[MAX_APPS];
extern struct vx_image *app_icons[MAX_APPS];
extern int app_count;

/* ---- main.c ---- */
void add_damage(struct rect r);
void damage_all(void);
bool inside(struct rect r, int x, int y);
struct rect frame_rect(const struct window *w);
struct rect panel_rect(void);
struct rect work_area(void);
struct window *window_at(int x, int y);
void activate(struct window *w);
void ask_to_close(struct window *w);
void add_note(const char *text);
void run_app(int i);
/* Opens a file or folder with its app (folders: Files). */
void open_path(const char *path);
/* Starts an app by its bundle's name ("Settings"), with an argument or NULL. */
void run_named(const char *stem, const char *argument);
/* Sends files dropped somewhere (a list file, see DESKTOP_DROP) to a window. */
void drop_on_window(struct window *w, const char *list, bool copy);
/* Draws a window, frame and all, with its top-left corner at (x, y). */
void draw_window_at(struct vx_surface *view, struct window *w, int x, int y);
/* The clock's text ("Sun 27 Sep  07:54"). */
bool clock_text(char *out, size_t size);
/* The local time now (in the zone Settings chose): false if not known. */
bool local_date(struct vx_date *d);
long now_ms(void);
/* Asks the main loop to come back soon (an animation is running). */
void want_frames(void);

/* ---- look.c ---- */
/* A soft shadow around (and a little below) a rectangle. */
void draw_shadow(struct vx_surface *view, struct rect r, int strength);
/* Darkens or tints a rectangle: `alpha` (0 to 255) of `color` over it. */
void blend_rect(struct vx_surface *view, struct rect r, uint32_t color, int alpha);
/* A rectangle with round corners (radius), filled; and its outline. */
void fill_rounded(struct vx_surface *view, struct rect r, int radius, uint32_t color);
void outline_rounded(struct vx_surface *view, struct rect r, int radius, uint32_t color);
/* Glass: what's behind a rectangle (already drawn), blurred, `tint_alpha` of
 * `tint` over it, and a shine on its top half (`shine`: 0 to 255); its top
 * and bottom corners round. */
void draw_glass(struct vx_surface *view, struct rect r, int top_radius, int bottom_radius,
                uint32_t tint, int tint_alpha, int shine);
/* A popup (menu, calendar, search...) of glass: shadow, glass, edge. */
void draw_glass_popup(struct vx_surface *view, struct rect r, int radius, int shadow);
/* A glossy ball of `color` (the title bar's buttons). */
void draw_orb(struct vx_surface *view, int cx, int cy, int radius, uint32_t color);
/* A glossy gel shape (vx_draw_gel without the edge), `alpha` of it. */
void fill_gel(struct vx_surface *view, struct rect r, int radius, uint32_t color, int alpha);
/* `from` drawn into a rectangle of `to`, made smaller smoothly (averaging) or
 * bigger, `alpha` of it over what's there. */
void blit_smooth(struct vx_surface *to, struct rect r, const struct vx_surface *from, int alpha);
/* A blurred, darker copy of a surface (for the lock screen). */
bool make_blurred(struct vx_surface *out, const struct vx_surface *from, int dim);
/* The pointer: its shape (VX_CURSOR_*, or one of the RESIZE ones below). */
enum { CURSOR_RESIZE_EW = VX_CURSOR_COUNT, CURSOR_RESIZE_NS, CURSOR_RESIZE_NWSE,
       CURSOR_RESIZE_NESW, CURSOR_SHAPES };
struct rect cursor_rect(int shape, int x, int y);
void draw_cursor(struct vx_surface *view, int ox, int oy, int shape, int x, int y);
/* Small pictures: a magnifier (search). */
void draw_magnifier(struct vx_surface *view, int x, int y, int size, uint32_t color);

/* ---- icons.c: the desktop's icons ---- */
/* Icons for files (Files' own), and an app's by its bundle name. */
struct vx_image *file_icon(const char *path, bool is_dir);
struct vx_image *app_named_icon(const char *stem);
void icons_load(void);           /* At start, and when the apps change. */
void icons_check(void);          /* Looks at the Desktop folder (every two seconds). */
void icons_draw(struct vx_surface *view, int ox, int oy);
void icons_draw_drag(struct vx_surface *view, int ox, int oy); /* Over the windows. */
struct rect icons_area(void);
/* The left button on the desktop (no window there), or let go after a
 * press there: true if it was used. */
bool icons_button(int bit, bool down, int double_click_ms);
void icons_pointer(void);
bool icons_dragging(void);
/* The icon under the pointer (for its menu): -1 if none. */
int icons_at(int x, int y);
/* Its menu's actions. */
void icons_menu(int icon, int action);
int icons_menu_items(int icon, const char **labels, int max);
/* Files dropped on the desktop from a window (a list file). */
void icons_drop(const char *list, bool copy, int x, int y);
/* New Folder on the desktop. */
void icons_new_folder(void);

/* ---- switcher.c: Alt+Tab ---- */
extern bool switcher_open;
void switcher_start(bool backwards);
void switcher_step(bool backwards);
void switcher_finish(bool choose);
void switcher_click(void);
void switcher_draw(struct vx_surface *view, int ox, int oy);
struct rect switcher_rect(void);
/* The most recently used windows first (focus moves a window to the front). */
void mru_touch(struct window *w);
void mru_forget(struct window *w);

/* ---- search.c: Ctrl+Space ---- */
extern bool search_open;
void search_show(void);
void search_hide(void);
void search_key(int key, int value, int character);
void search_button(bool down);
void search_pointer(void);
void search_draw(struct vx_surface *view, int ox, int oy);
struct rect search_rect(void);

/* ---- clock.c: the calendar and the notifications under the clock ---- */
extern bool clock_open;
extern int unread_notes;
void clock_toggle(void);
void clock_close(void);
void clock_button(bool down);
void clock_pointer(void);
void clock_draw(struct vx_surface *view, int ox, int oy);
struct rect clock_rect(void);
struct rect clock_button_rect(void); /* On the panel. */
void clock_remember(const char *text); /* A notification, for the list. */

/* ---- volume.c: the sound volume (the panel's speaker, the volume keys) ---- */
extern bool volume_open;
void volume_check(void); /* Looks at the sound core now and then. */
struct rect volume_button_rect(void); /* On the panel. */
void volume_draw_button(struct vx_surface *view, int ox, int oy);
void volume_toggle(void);
void volume_close(void);
struct rect volume_rect(void);
void volume_draw(struct vx_surface *view, int ox, int oy);
void volume_button(bool down);
void volume_pointer(void);
bool volume_sliding(void);
void volume_wheel(int wheel); /* Scrolling over the panel's speaker. */
bool volume_key(int key, int value); /* The volume keys: true if it was one. */
void volume_draw_osd(struct vx_surface *view, int ox, int oy);
long volume_osd_wait(void); /* Milliseconds the level bubble still shows, or -1. */

/* ---- shot.c: screenshots (PrintScreen) ---- */
enum shot_kind { SHOT_SCREEN, SHOT_WINDOW, SHOT_AREA };
extern bool shot_selecting;
void screenshot(enum shot_kind kind);
void shot_button(bool down);
void shot_pointer(void);
void shot_cancel(void);
void shot_draw(struct vx_surface *view, int ox, int oy);
void shot_tick(void); /* The flash fading. */

/* ---- lock.c: the screensaver and the lock screen ---- */
extern bool locked, saver_on;
/* Sessions (main.c): who's logged in. The login screen (lock.c) picks one
 * of `users` and calls session_start. */
extern struct vx_user session_user;
extern bool session_has_password;
void session_start(const struct vx_user *user);
void login_begin(const struct vx_user *users, int count);
void lock_now(void);
void lock_settings(void); /* Reads them (Settings changed something). */
/* Every input event goes here first: false if it's used up (waking the
 * screensaver, or typing a password). */
bool lock_input(void);
void lock_key(int key, int value, int character);
void lock_button(bool down);
void lock_tick(void); /* Starts the screensaver when it's time; moves it. */
long lock_wait_ms(void); /* How soon lock_tick wants to run. */
void lock_draw(struct vx_surface *view, int ox, int oy);
void lock_screen_changed(void); /* A new wallpaper or screen size. */

#endif
