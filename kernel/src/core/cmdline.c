#include <stddef.h>
#include <vexa/cmdline.h>
#include <vexa/string.h>

#define CMDLINE_MAX 256

/* A copy: the bootloader's string goes away when its memory is reclaimed. */
static char kernel_cmdline[CMDLINE_MAX];

void cmdline_init(const char *cmdline) {
    size_t i = 0;
    for (; cmdline && cmdline[i] && i < CMDLINE_MAX - 1; i++) {
        kernel_cmdline[i] = cmdline[i];
    }
    kernel_cmdline[i] = '\0';
}

const char *cmdline_get(void) {
    return kernel_cmdline;
}

bool cmdline_value(const char *key, char *out, size_t size) {
    size_t length = strlen(key);
    const char *p = kernel_cmdline;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *word = p;
        while (*p && *p != ' ') {
            p++;
        }
        if ((size_t)(p - word) > length && memcmp(word, key, length) == 0 && word[length] == '=') {
            size_t n = (size_t)(p - word) - length - 1;
            n = n < size - 1 ? n : size - 1;
            memcpy(out, word + length + 1, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

bool cmdline_has(const char *option) {
    size_t length = strlen(option);
    const char *p = kernel_cmdline;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *word = p;
        while (*p && *p != ' ') {
            p++;
        }
        if ((size_t)(p - word) == length && memcmp(word, option, length) == 0) {
            return true;
        }
    }
    return false;
}
