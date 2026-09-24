#ifndef VEXA_KEYBOARD_H
#define VEXA_KEYBOARD_H

#include <stdbool.h>

/* Special keys delivered as values above the ASCII range. */
#define KEY_UP 0x100
#define KEY_DOWN 0x101
#define KEY_LEFT 0x102
#define KEY_RIGHT 0x103

/* Sets up the PS/2 controller and keyboard. Returns false if none was found. */
bool keyboard_init(void);

/* Returns the next key (ASCII or KEY_*), or -1 if none is waiting. */
int keyboard_read(void);
/* Waits (sleeping, not spinning) for the next key. */
int keyboard_read_blocking(void);

#endif
