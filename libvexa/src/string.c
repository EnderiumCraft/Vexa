#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The compiler may emit calls to memcpy/memset/memmove/memcmp on its own. */

/* rep movsb/stosb instead of byte loops: an optimizing compiler may turn such a
 * loop back into a call to memcpy/memset, which here would recurse forever. */

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    void *d = dest;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dest;
}

void *memset(void *s, int c, size_t n) {
    void *d = s;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    if (dest <= src || (const uint8_t *)src + n <= (uint8_t *)dest) {
        return memcpy(dest, src, n);
    }
    /* Overlapping with dest above src: copy backwards. */
    void *d = (uint8_t *)dest + n - 1;
    const void *s = (const uint8_t *)src + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = s1, *b = s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) {
        n++;
    }
    return n;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || !s1[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    return memcpy(dest, src, strlen(src) + 1);
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    size_t length = strlen(dest), i = 0;
    for (; i < n && src[i]; i++) {
        dest[length + i] = src[i];
    }
    dest[length + i] = '\0';
    return dest;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

char *strrchr(const char *s, int c) {
    const char *found = NULL;
    for (;; s++) {
        if (*s == (char)c) {
            found = s;
        }
        if (!*s) {
            return (char *)found;
        }
    }
}

char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, n) == 0) {
            return (char *)haystack;
        }
    }
    return n == 0 ? (char *)haystack : NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) {
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) {
        n++;
    }
    return n;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    return copy ? memcpy(copy, s, n) : NULL;
}

char *strndup(const char *s, size_t n) {
    n = strnlen(s, n);
    char *copy = malloc(n + 1);
    if (copy) {
        memcpy(copy, s, n);
        copy[n] = '\0';
    }
    return copy;
}
