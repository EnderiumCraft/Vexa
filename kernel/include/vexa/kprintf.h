#ifndef VEXA_KPRINTF_H
#define VEXA_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Supports %s %c %d %i %u %x %p %% and the 'l' length modifier. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list args);
/* Writes `length` bytes as they are (e.g. text from a user program). */
void kwrite(const char *text, size_t length);

__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

#endif
