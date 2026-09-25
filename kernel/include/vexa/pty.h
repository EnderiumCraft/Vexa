#ifndef VEXA_PTY_H
#define VEXA_PTY_H

#include <stdbool.h>

struct file;
struct tty;

/* Adds /dev/ptmx and /dev/pts (dev/pty.c). */
void pty_init(void);
/* The terminal behind a pty file, master or /dev/pts/N (NULL otherwise). */
struct tty *pty_terminal(struct file *file);
bool pty_is_master(struct file *file);
/* Makes /dev/pts/N the calling process's controlling terminal (its /dev/tty)
 * and its group the foreground. False if the file isn't a /dev/pts/N. */
bool pty_make_controlling(struct file *file);

#endif
