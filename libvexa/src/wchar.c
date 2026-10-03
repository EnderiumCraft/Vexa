/* <wchar.h>: wide strings, and UTF-8 one character at a time. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

size_t wcslen(const wchar_t *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b) {
        a++, b++;
    }
    return *a < *b ? -1 : *a > *b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (; n && *a && *a == *b; n--) {
        a++, b++;
    }
    return n == 0 ? 0 : *a < *b ? -1 : *a > *b;
}

wchar_t *wcscpy(wchar_t *to, const wchar_t *from) {
    wchar_t *p = to;
    while ((*p++ = *from++)) {
    }
    return to;
}

wchar_t *wcsncpy(wchar_t *to, const wchar_t *from, size_t n) {
    size_t i = 0;
    for (; i < n && from[i]; i++) {
        to[i] = from[i];
    }
    for (; i < n; i++) {
        to[i] = 0;
    }
    return to;
}

wchar_t *wcscat(wchar_t *to, const wchar_t *from) {
    wcscpy(to + wcslen(to), from);
    return to;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (;; s++) {
        if (*s == c) {
            return (wchar_t *)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *found = NULL;
    for (;; s++) {
        if (*s == c) {
            found = s;
        }
        if (!*s) {
            return (wchar_t *)found;
        }
    }
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle) {
    size_t n = wcslen(needle);
    for (; *haystack; haystack++) {
        if (wcsncmp(haystack, needle, n) == 0) {
            return (wchar_t *)haystack;
        }
    }
    return n == 0 ? (wchar_t *)haystack : NULL;
}

wchar_t *wcsdup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *copy = malloc(n * sizeof(wchar_t));
    return copy ? wmemcpy(copy, s, n) : NULL;
}

wchar_t *wmemcpy(wchar_t *to, const wchar_t *from, size_t n) {
    return memcpy(to, from, n * sizeof(wchar_t));
}

wchar_t *wmemmove(wchar_t *to, const wchar_t *from, size_t n) {
    return memmove(to, from, n * sizeof(wchar_t));
}

wchar_t *wmemset(wchar_t *to, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        to[i] = c;
    }
    return to;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) {
            return (wchar_t *)s + i;
        }
    }
    return NULL;
}

int mbsinit(const mbstate_t *state) {
    (void)state;
    return 1;
}

/* A character only when all of its bytes are there: (size_t)-2 if they
 * aren't yet (nothing is kept between calls), (size_t)-1 if they're bad. */
size_t mbrtowc(wchar_t *out, const char *s, size_t n, mbstate_t *state) {
    (void)state;
    if (!s) {
        return 0;
    }
    if (n == 0) {
        return (size_t)-2;
    }
    const unsigned char *p = (const unsigned char *)s;
    unsigned c = p[0];
    size_t length;
    if (c < 0x80) {
        length = 1;
    } else if (c >= 0xc2 && c < 0xe0) {
        length = 2, c &= 0x1f;
    } else if (c >= 0xe0 && c < 0xf0) {
        length = 3, c &= 0x0f;
    } else if (c >= 0xf0 && c < 0xf5) {
        length = 4, c &= 0x07;
    } else {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (n < length) {
        for (size_t i = 1; i < n; i++) {
            if ((p[i] & 0xc0) != 0x80) {
                errno = EILSEQ;
                return (size_t)-1;
            }
        }
        return (size_t)-2;
    }
    for (size_t i = 1; i < length; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        c = c << 6 | (p[i] & 0x3f);
    }
    if ((length == 3 && c < 0x800) || (length == 4 && (c < 0x10000 || c > 0x10ffff)) ||
        (c >= 0xd800 && c < 0xe000)) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (out) {
        *out = (wchar_t)c;
    }
    return c ? length : 0;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *state) {
    return mbrtowc(NULL, s, n, state);
}

size_t wcrtomb(char *out, wchar_t wc, mbstate_t *state) {
    (void)state;
    char scratch[4];
    if (!out) {
        out = scratch;
        wc = 0;
    }
    unsigned c = (unsigned)wc;
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if ((c >= 0xd800 && c < 0xe000) || c > 0x10ffff) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

size_t mbsrtowcs(wchar_t *out, const char **s, size_t n, mbstate_t *state) {
    size_t count = 0;
    const char *p = *s;
    while (!out || count < n) {
        wchar_t c;
        size_t used = mbrtowc(&c, p, 4, state);
        if (used == (size_t)-1 || used == (size_t)-2) {
            *s = p;
            return (size_t)-1;
        }
        if (out) {
            out[count] = c;
        }
        if (used == 0) {
            if (out) {
                *s = NULL;
            }
            return count;
        }
        p += used;
        count++;
    }
    *s = p;
    return count;
}

size_t wcsrtombs(char *out, const wchar_t **s, size_t n, mbstate_t *state) {
    size_t count = 0;
    const wchar_t *p = *s;
    for (;; p++) {
        char bytes[4];
        size_t length = wcrtomb(bytes, *p, state);
        if (length == (size_t)-1) {
            *s = p;
            return (size_t)-1;
        }
        if (*p == 0) {
            if (out) {
                if (count < n) {
                    out[count] = '\0';
                }
                *s = NULL;
            }
            return count;
        }
        if (out) {
            if (count + length > n) {
                *s = p;
                return count;
            }
            memcpy(out + count, bytes, length);
        }
        count += length;
    }
}

wint_t btowc(int c) {
    return c >= 0 && c < 0x80 ? (wint_t)c : WEOF;
}

int wctob(wint_t c) {
    return c < 0x80 ? (int)c : -1;
}

/* Columns on a terminal: 0 for combining marks, 2 for wide (CJK, emoji). */
int wcwidth(wchar_t wc) {
    unsigned c = (unsigned)wc;
    if (c == 0) {
        return 0;
    }
    if (c < 32 || (c >= 0x7f && c < 0xa0)) {
        return -1;
    }
    if ((c >= 0x300 && c < 0x370) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c < 0xfe10)) {
        return 0;
    }
    if ((c >= 0x1100 && c < 0x1160) || (c >= 0x2e80 && c < 0xa4d0) || (c >= 0xac00 && c < 0xd7a4) ||
        (c >= 0xf900 && c < 0xfb00) || (c >= 0xff00 && c < 0xff61) || (c >= 0xffe0 && c < 0xffe7) ||
        (c >= 0x1f300 && c < 0x1fa00) || (c >= 0x20000 && c < 0x3fffe)) {
        return 2;
    }
    return 1;
}
