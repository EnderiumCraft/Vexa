#ifndef LIBVEXA_STDIO_H
#define LIBVEXA_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct vx_file FILE;

extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)
#define BUFSIZ 4096

FILE *fopen(const char *path, const char *mode); /* "r", "w", "a", "r+", "w+", "a+" */
FILE *fdopen(int handle, const char *mode);
int fclose(FILE *file);
int fflush(FILE *file); /* NULL flushes every open file. */
size_t fread(void *buffer, size_t size, size_t count, FILE *file);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *file);
int fgetc(FILE *file);
char *fgets(char *line, int size, FILE *file);
int fputc(int c, FILE *file);
int fputs(const char *text, FILE *file);
int feof(FILE *file);
int ferror(FILE *file);
int fileno(FILE *file);

#define getc fgetc
#define putc fputc
int getchar(void);
int putchar(int c);
int puts(const char *text);

/* Formats: %d %i %u %x %X %o %c %s %p %% with flags "-0+ ", widths and
 * precisions (also as *), and the length modifiers hh h l ll z j t. */
int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *file, const char *format, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *out, const char *format, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *out, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int vprintf(const char *format, va_list args);
int vfprintf(FILE *file, const char *format, va_list args);
int vsnprintf(char *out, size_t size, const char *format, va_list args);

#endif
