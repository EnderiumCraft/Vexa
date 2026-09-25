#ifndef VEXA_KEYBOARD_H
#define VEXA_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

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
/* Sends every key to `consumer` (called from the keyboard interrupt) instead
 * of buffering it for keyboard_read. */
void keyboard_set_consumer(void (*consumer)(int key));

/* The PS/2 mouse, on the keyboard's controller (dev/ps2_mouse.c). */
void ps2_mouse_init(void);
void ps2_mouse_byte(uint8_t byte); /* A byte from the mouse, in an interrupt. */
void ps2_drain(void);              /* Reads every waiting byte, keyboard or mouse. */
/* The controller, for the drivers: a command, a data byte, waiting for a reply. */
bool ps2_command(uint8_t command);
bool ps2_write(uint8_t value);
bool ps2_wait_output(void);

#endif
