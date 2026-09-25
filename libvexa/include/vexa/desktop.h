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
 */

#define DESKTOP_SOCKET "/run/desktop"

enum desktop_message_type {
    /* Program to desktop. */
    DESKTOP_CREATE = 1,  /* a = width, b = height, text = buffer file, then title */
    DESKTOP_PRESENT = 2, /* window; a, b, c, d = x, y, width, height */
    DESKTOP_TITLE = 3,   /* window; text = title */
    DESKTOP_DESTROY = 4, /* window */
    /* Desktop to program. */
    DESKTOP_CREATED = 16, /* window (0 if it failed) */
    DESKTOP_KEY = 17,     /* window; a = key, b = value, c = character */
    DESKTOP_POINTER = 18, /* window; a, b = x, y; c = buttons; d = wheel */
    DESKTOP_CLOSE = 19,   /* window */
    DESKTOP_FOCUS = 20,   /* window; a = 1 gained, 0 lost */
};

struct desktop_message {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    char text[104]; /* For CREATE: the buffer file's path, a NUL, then the title. */
};

#endif
