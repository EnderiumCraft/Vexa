#ifndef VEXA_TTY_H
#define VEXA_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Terminals: the console (keyboard in, screen out) and pseudo-terminals
 * (dev/pty.c), with Unix-style line editing in between. In "canonical" mode
 * (the default) a program reading it gets a whole line once Enter is pressed; Backspace and Ctrl+U edit the line, Ctrl+D
 * ends input, and Ctrl+C sends SIGINT to the foreground process group. Raw
 * mode hands over each key as it comes.
 *
 * The flag bits and control characters are the Linux termios ones, so the
 * Linux subsystem can pass them straight through.
 */

/* Local modes (c_lflag). */
#define TTY_ISIG 0x0001
#define TTY_ICANON 0x0002
#define TTY_ECHO 0x0008
#define TTY_ECHOE 0x0010
#define TTY_ECHOK 0x0020
#define TTY_ECHONL 0x0040
#define TTY_IEXTEN 0x8000
/* Input modes (c_iflag). */
#define TTY_ICRNL 0x0100
/* Output modes (c_oflag). */
#define TTY_OPOST 0x0001
#define TTY_ONLCR 0x0004

/* Control characters (c_cc indexes). */
#define TTY_VINTR 0
#define TTY_VQUIT 1
#define TTY_VERASE 2
#define TTY_VKILL 3
#define TTY_VEOF 4
#define TTY_VTIME 5
#define TTY_VMIN 6
#define TTY_VSUSP 10
#define TTY_NCCS 32

struct tty_settings {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[TTY_NCCS];
};

/* A terminal: the console, or the program side of a pseudo-terminal. */
struct tty;

/* Where a terminal's output (and echo) goes: the screen, or a pty's
 * master side. Called with no locks held; must not sleep. */
typedef void (*tty_output_fn)(struct tty *tty, const char *text, size_t length);

/* The console terminal (keyboard in, screen out). */
extern struct tty *console_tty;

void tty_init(void);
/* Takes the keyboard away from the kernel monitor. */
void tty_attach_keyboard(void);

/* A new terminal with the default settings; `owner` is the creator's. */
struct tty *tty_create(tty_output_fn output, void *owner, uint16_t rows, uint16_t columns);
void tty_destroy(struct tty *tty);
void *tty_owner(struct tty *tty);
/* Typed input: one byte, as if from a keyboard (line editing, echo, Ctrl+C). */
void tty_receive(struct tty *tty, char c);
/* The other side is gone: reads return 0 from now on, and the foreground
 * group gets SIGHUP. */
void tty_hang_up(struct tty *tty);

/* Kernel buffers. Reading may wait, unless `nonblocking` (-VX_EAGAIN). */
int64_t tty_read(struct tty *tty, void *buffer, size_t size, bool nonblocking);
int64_t tty_write(struct tty *tty, const void *buffer, size_t size);
void tty_get_settings(struct tty *tty, struct tty_settings *settings);
void tty_set_settings(struct tty *tty, const struct tty_settings *settings);
uint32_t tty_foreground(struct tty *tty);
void tty_set_foreground(struct tty *tty, uint32_t group);
void tty_window_size(struct tty *tty, uint16_t *rows, uint16_t *columns);
/* Sets the size and sends SIGWINCH to the foreground group if it changed. */
void tty_set_window_size(struct tty *tty, uint16_t rows, uint16_t columns);
size_t tty_bytes_ready(struct tty *tty);
uint32_t tty_poll(struct tty *tty); /* OBJECT_* bits */

#endif
