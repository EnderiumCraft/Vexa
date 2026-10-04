#ifndef LIBVEXA_STDLIB_H
#define LIBVEXA_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

__attribute__((noreturn)) void exit(int code);
__attribute__((noreturn)) void abort(void);

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
extern char **environ;

int atoi(const char *text);
long atol(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
int abs(int value);
long labs(long value);
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));

#define RAND_MAX 0x7fffffff
#define MB_CUR_MAX 4

typedef struct {
    int quot, rem;
} div_t;
typedef struct {
    long quot, rem;
} ldiv_t;
typedef struct {
    long long quot, rem;
} lldiv_t;

__attribute__((noreturn)) void _Exit(int code);
int atexit(void (*fn)(void));
long long atoll(const char *text);
double atof(const char *text);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
double strtod(const char *text, char **end);
float strtof(const char *text, char **end);
long double strtold(const char *text, char **end);
long long llabs(long long value);
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);
int rand(void);
void srand(unsigned seed);
long random(void);
void srandom(unsigned seed);
int rand_r(unsigned *seed);
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
void *aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void **out, size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size);
int system(const char *command);
char *realpath(const char *path, char *resolved);
int mkstemp(char *pattern);
char *mkdtemp(char *pattern);
char *mktemp(char *pattern);
int putenv(char *assignment);
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *out, const char *s, size_t n);
int wctomb(char *out, wchar_t c);
size_t mbstowcs(wchar_t *out, const char *s, size_t n);
size_t wcstombs(char *out, const wchar_t *s, size_t n);

#ifdef __cplusplus
}
#endif

#endif
