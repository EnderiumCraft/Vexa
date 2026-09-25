#ifndef LIBVEXA_DESKTOP_H
#define LIBVEXA_DESKTOP_H

#include <stdint.h>

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

enum desktop_message_type {
    /* Program to desktop. */
    DESKTOP_CREATE = 1,  /* a = width, b = height, c = DESKTOP_* flags,
                            text = buffer file, then title */
    DESKTOP_PRESENT = 2, /* window; a, b, c, d = x, y, width, height */
    DESKTOP_TITLE = 3,   /* window; text = title */
    DESKTOP_DESTROY = 4, /* window */
    DESKTOP_BUFFER = 5,  /* window; a = width, b = height, text = the new buffer file */
    /* Desktop to program. */
    DESKTOP_CREATED = 16, /* window (0 if it failed) */
    DESKTOP_KEY = 17,     /* window; a = key, b = value, c = character */
    DESKTOP_POINTER = 18, /* window; a, b = x, y; c = buttons; d = wheel */
    DESKTOP_CLOSE = 19,   /* window */
    DESKTOP_FOCUS = 20,   /* window; a = 1 gained, 0 lost */
    DESKTOP_CONFIGURE = 21, /* window; a = width, b = height: please be this size */
    DESKTOP_RESIZED = 22,   /* window; a = width, b = height (0: the BUFFER failed) */
};

/* DESKTOP_CREATE flags. */
#define DESKTOP_RESIZABLE 0x1

struct desktop_message {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    char text[104]; /* For CREATE: the buffer file's path, a NUL, then the title. */
};

#endif
