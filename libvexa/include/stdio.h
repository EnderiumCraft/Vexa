#ifndef LIBVEXA_STDIO_H
#define LIBVEXA_STDIO_H

/* Supports %s %c %d %i %u %x %p %% with the l and ll length modifiers. */
int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *text);

#endif
