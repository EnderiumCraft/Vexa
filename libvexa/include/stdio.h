#ifndef LIBVEXA_STDIO_H
#define LIBVEXA_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vx_file FILE;

extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 1024
#define FOPEN_MAX 64
#define L_tmpnam 32
#define TMP_MAX 10000
#define P_tmpdir "/tmp"
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef long fpos_t;

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

int getc(FILE *file); /* (Also macros, below.) */
int putc(int c, FILE *file);
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
int vsprintf(char *out, const char *format, va_list args);
int dprintf(int fd, const char *format, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *format, va_list args);
int asprintf(char **out, const char *format, ...) __attribute__((format(printf, 2, 3)));
int vasprintf(char **out, const char *format, va_list args);

int scanf(const char *format, ...) __attribute__((format(scanf, 1, 2)));
int fscanf(FILE *file, const char *format, ...) __attribute__((format(scanf, 2, 3)));
int sscanf(const char *text, const char *format, ...) __attribute__((format(scanf, 2, 3)));
int vscanf(const char *format, va_list args);
int vfscanf(FILE *file, const char *format, va_list args);
int vsscanf(const char *text, const char *format, va_list args);

int fseek(FILE *file, long offset, int whence);
int fseeko(FILE *file, off_t offset, int whence);
long ftell(FILE *file);
off_t ftello(FILE *file);
void rewind(FILE *file);
int fgetpos(FILE *file, fpos_t *position);
int fsetpos(FILE *file, const fpos_t *position);
int ungetc(int c, FILE *file);
void clearerr(FILE *file);
int setvbuf(FILE *file, char *buffer, int mode, size_t size);
void setbuf(FILE *file, char *buffer);
FILE *freopen(const char *path, const char *mode, FILE *file);
FILE *tmpfile(void);
char *tmpnam(char *out);
int remove(const char *path);
int rename(const char *from, const char *to);
void perror(const char *what);
ssize_t getline(char **line, size_t *size, FILE *file);

/* Files that aren't files: a buffer that grows (open_memstream), a fixed one
 * (fmemopen), or any I/O functions (fopencookie, as in glibc). */
typedef struct {
    ssize_t (*read)(void *cookie, char *buffer, size_t size);
    ssize_t (*write)(void *cookie, const char *buffer, size_t size);
    int (*seek)(void *cookie, off_t *offset, int whence);
    int (*close)(void *cookie);
} cookie_io_functions_t;
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io);
FILE *open_memstream(char **bufp, size_t *sizep);
FILE *fmemopen(void *buffer, size_t size, const char *mode);
ssize_t getdelim(char **line, size_t *size, int delimiter, FILE *file);
FILE *popen(const char *command, const char *mode);
int pclose(FILE *file);
void flockfile(FILE *file);
void funlockfile(FILE *file);
int getc_unlocked(FILE *file);
int putc_unlocked(int c, FILE *file);
int getchar_unlocked(void);
int putchar_unlocked(int c);
#define getc_unlocked fgetc
#define putc_unlocked fputc
#define getchar_unlocked getchar
#define putchar_unlocked putchar

#ifdef __cplusplus
}
#endif

#endif
