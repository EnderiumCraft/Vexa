#ifndef VEXA_KPRINTF_H
#define VEXA_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Supports %s %c %d %i %u %x %X %p %%, the 'l' length modifier, and a
 * zero-padded width for numbers (%02u). */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list args);
/* Formats into a buffer (always NUL-terminated if size > 0); returns the
 * length the whole text would have. */
size_t ksnprintf(char *buffer, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t kvsnprintf(char *buffer, size_t size, const char *fmt, va_list args);
/* Writes `length` bytes as they are (e.g. text from a user program). */
void kwrite(const char *text, size_t length);

__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

#endif
