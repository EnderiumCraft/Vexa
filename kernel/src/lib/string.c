#include <stdint.h>
#include <vexa/string.h>

/* The compiler may emit calls to these even in freestanding code. */

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    if (s > d) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (s < d) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = s1;
    const uint8_t *b = s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}
