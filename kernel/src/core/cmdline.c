#include <stddef.h>
#include <vexa/cmdline.h>
#include <vexa/string.h>

static const char *kernel_cmdline = "";

void cmdline_init(const char *cmdline) {
    if (cmdline) {
        kernel_cmdline = cmdline;
    }
}

const char *cmdline_get(void) {
    return kernel_cmdline;
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
