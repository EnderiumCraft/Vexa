#ifndef LIBVEXA_DESKTOP_H
#define LIBVEXA_DESKTOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The desktop protocol: what programs and the desktop (the compositor) say to
 * each other over the local socket DESKTOP_SOCKET, as fixed-size messages.
 * <vexa/gui.h> wraps it; only the desktop itself needs this.
 *
 * A window's pixels live in a file the program makes under /run/shm and maps;
 * the desktop maps the same file, so presenting copies nothing.
 *
 * Resizing: a window created with DESKTOP_RESIZABLE may get a CONFIGURE (the
 * user dragged its edge, or maximized it); the program then makes a buffer of
 * that size (or another it can live with) and sends BUFFER, which the
 * desktop answers with RESIZED. A program can also resize on its own the
 * same way.
 */

#define DESKTOP_SOCKET "/run/desktop"
/* The desktop's exit code when someone logs out: vinit starts it again. */
#define DESKTOP_EXIT_LOGOUT 3
/* The clipboard's text (UTF-8), shared by every program and X. */
#define DESKTOP_CLIPBOARD_FILE "/run/clipboard"

enum desktop_message_type {
    /* Program to desktop. */
    DESKTOP_CREATE = 1,  /* a = width, b = height, c = DESKTOP_* flags,
                            text = buffer file, then title */
    DESKTOP_PRESENT = 2, /* window; a, b, c, d = x, y, width, height */
    DESKTOP_TITLE = 3,   /* window; text = title */
    DESKTOP_DESTROY = 4, /* window */
    DESKTOP_BUFFER = 5,  /* window; a = width, b = height, text = the new buffer file */
    DESKTOP_MOVE = 6,    /* window; a, b = where its content goes on the screen */
    DESKTOP_INFO = 7,    /* answered by INFO_REPLY */
    DESKTOP_NOTIFY = 8,  /* text = a notification to show for a few seconds */
    DESKTOP_RELOAD = 9,  /* read DESKTOP_CONFIG again */
    DESKTOP_WM = 10,     /* window; a = DESKTOP_WM_*, b = its argument: what a program
                            asks of a window manager (X programs, through Xvexa) */
    DESKTOP_CURSOR = 11, /* window; a = VX_CURSOR_*: the pointer's shape over it */
    DESKTOP_DRAG = 12,   /* window; a = 1 to copy (not move); text = a file listing
                            the paths, one per line: files dragged out of the window
                            and let go where the pointer is now */
    DESKTOP_LOCK = 13,   /* lock the screen now */
    DESKTOP_CLIPBOARD_SET = 14, /* the clipboard changed: it's in DESKTOP_CLIPBOARD_FILE
                                   (a = 1: Xvexa set it, from an X program) */
    /* Desktop to program. */
    DESKTOP_CREATED = 16, /* window (0 if it failed) */
    DESKTOP_KEY = 17,     /* window; a = key, b = value, c = character */
    DESKTOP_POINTER = 18, /* window; a, b = x, y; c = buttons; d = wheel */
    DESKTOP_CLOSE = 19,   /* window */
    DESKTOP_FOCUS = 20,   /* window; a = 1 gained, 0 lost */
    DESKTOP_CONFIGURE = 21, /* window; a = width, b = height: please be this size */
    DESKTOP_RESIZED = 22,   /* window; a = width, b = height (0: the BUFFER failed) */
    DESKTOP_MOVED = 23,     /* window; a, b = where its content is on the screen now */
    DESKTOP_INFO_REPLY = 24, /* a, b = the screen's width and height */
    DESKTOP_STATE = 25,     /* window; a = 1 if maximized, b = 1 if minimized */
    DESKTOP_THEME = 26,     /* the theme (or other settings) changed: read them again */
    DESKTOP_DROP = 27,      /* window; a, b = x, y; c = 1 to copy; text = the list file:
                               files dropped on the window (see DRAG) */
    DESKTOP_CLIPBOARD = 28, /* the clipboard changed (Xvexa hands it to X programs) */
    DESKTOP_MODIFIED = 29,  /* window; a = 1 if it has changes that aren't saved, 0 if not
                               (the desktop asks before logging out, restarting or
                               shutting down with such windows open) */
};

/* DESKTOP_WM requests. MOVE and RESIZE start dragging the window with the
 * pointer, as if by its title bar or edges (b: 1 right edge, 2 bottom, 4 left). */
enum {
    DESKTOP_WM_MAXIMIZE = 1, DESKTOP_WM_RESTORE, DESKTOP_WM_TOGGLE_MAXIMIZED,
    DESKTOP_WM_MINIMIZE, DESKTOP_WM_MOVE, DESKTOP_WM_RESIZE, DESKTOP_WM_ACTIVATE,
};

/* DESKTOP_CREATE flags. */
#define DESKTOP_RESIZABLE 0x1
/* A menu or tooltip: no frame, above other windows, never takes the
 * keyboard, and not in the panel. It stays where DESKTOP_MOVE puts it. */
#define DESKTOP_POPUP 0x2
/* A window that draws its own title bar (GTK's client-side decorations):
 * no frame, but otherwise like any other. */
#define DESKTOP_UNDECORATED 0x4

struct desktop_message {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    char text[104]; /* For CREATE: the buffer file's path, a NUL, then the title. */
};


/* ---- Settings (/etc/desktop.conf, "key=value" lines) ----
 *
 *     wallpaper=image         one of desktop_wallpapers[], or "image"
 *     wallpaper_image=/path   a PNG, BMP or PPM file (for wallpaper=image)
 *     clock=24                24 or 12 (hours)
 *     utc_offset=120          minutes east of UTC
 *
 * The settings app writes it and sends DESKTOP_RELOAD.
 */
#define DESKTOP_CONFIG "/etc/desktop.conf"

/* The wallpaper without a DESKTOP_CONFIG: this picture (or, if it can't be
 * read, the first gradient). It stands for the default wallpaper in the
 * theme's colors, which the desktop shows (see vx_theme_wallpaper()). */
#define DESKTOP_DEFAULT_WALLPAPER "/share/pictures/glass.png"

struct desktop_wallpaper {
    const char *name, *label;
    uint32_t top, bottom; /* A vertical gradient. */
};

static const struct desktop_wallpaper desktop_wallpapers[] = {
    {"dusk", "Dusk", 0x2a1850, 0x0b0613},
    {"ocean", "Ocean", 0x0f3b5c, 0x05121e},
    {"forest", "Forest", 0x1d4a2f, 0x06140c},
    {"sunset", "Sunset", 0x7a2e3b, 0x1a0b16},
    {"graphite", "Graphite", 0x3a3d45, 0x111216},
    {"midnight", "Midnight", 0x1d2a4a, 0x060912},
    {"ember", "Ember", 0x8c3b14, 0x1c0a04},
    {"sand", "Sand", 0xb39b76, 0x4a3b27},
    {"rose", "Rose", 0xb0607e, 0x2c1220},
    {"black", "Black", 0x1c1c1e, 0x000000},
};
#define DESKTOP_WALLPAPER_COUNT (int)(sizeof(desktop_wallpapers) / sizeof(desktop_wallpapers[0]))

#ifdef __cplusplus
}
#endif

#endif
