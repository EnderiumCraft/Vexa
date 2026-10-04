/* chmod: new permission bits: chmod MODE FILE... (MODE in octal, like 644,
 * or symbolic, like u+x, go-w, a=r). The owner or root only. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

/* Applies a symbolic mode ("u+x,go-w") to `mode`; false if it isn't one. */
static bool symbolic(const char *text, unsigned int *mode) {
    for (const char *p = text; *p;) {
        unsigned int who = 0;
        for (; *p && strchr("ugoa", *p); p++) {
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        }
        if (!who) {
            who = 07777;
        }
        char op = *p++;
        if (op != '+' && op != '-' && op != '=') {
            return false;
        }
        unsigned int bits = 0;
        for (; *p && *p != ','; p++) {
            switch (*p) {
            case 'r': bits |= 0444; break;
            case 'w': bits |= 0222; break;
            case 'x': bits |= 0111; break;
            case 's': bits |= 06000; break;
            case 't': bits |= 01000; break;
            default: return false;
            }
        }
        bits &= who;
        *mode = op == '+' ? *mode | bits : op == '-' ? *mode & ~bits : (*mode & ~who) | bits;
        if (*p == ',') {
            p++;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: chmod MODE FILE...\n");
        return 2;
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        struct vx_stat st;
        long error = vx_stat(argv[i], &st);
        unsigned int mode = st.mode;
        char *end;
        unsigned long octal = strtoul(argv[1], &end, 8);
        if (!error && !*end) {
            mode = (unsigned int)octal & 07777;
        } else if (!error && !symbolic(argv[1], &mode)) {
            fprintf(stderr, "chmod: %s: not a mode\n", argv[1]);
            return 2;
        }
        error = error ? error : vx_chmod(argv[i], mode, 0);
        if (error) {
            fprintf(stderr, "chmod: %s: %s\n", argv[i], vx_strerror(error));
            status = 1;
        }
    }
    return status;
}
