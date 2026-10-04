#ifndef LIBVEXA_WCHAR_H
#define LIBVEXA_WCHAR_H

/* Wide characters: wchar_t is a Unicode code point (32 bits); multibyte
 * text is UTF-8. */
#include <stdarg.h>
#include <stddef.h>
#include <bits/types/mbstate_t.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int wint_t;

#define WEOF 0xffffffffu
#ifndef WCHAR_MIN
#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647
#endif

size_t wcslen(const wchar_t *s);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcscpy(wchar_t *to, const wchar_t *from);
wchar_t *wcsncpy(wchar_t *to, const wchar_t *from, size_t n);
wchar_t *wcscat(wchar_t *to, const wchar_t *from);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcsdup(const wchar_t *s);
wchar_t *wmemcpy(wchar_t *to, const wchar_t *from, size_t n);
wchar_t *wmemmove(wchar_t *to, const wchar_t *from, size_t n);
wchar_t *wmemset(wchar_t *to, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

int mbsinit(const mbstate_t *state);
size_t mbrtowc(wchar_t *out, const char *s, size_t n, mbstate_t *state);
size_t mbrlen(const char *s, size_t n, mbstate_t *state);
size_t wcrtomb(char *out, wchar_t c, mbstate_t *state);
size_t mbsrtowcs(wchar_t *out, const char **s, size_t n, mbstate_t *state);
size_t wcsrtombs(char *out, const wchar_t **s, size_t n, mbstate_t *state);
wint_t btowc(int c);
int wctob(wint_t c);
int wcwidth(wchar_t c);

#ifdef __cplusplus
}
#endif

#endif
