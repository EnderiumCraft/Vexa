#include <stdint.h>
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
