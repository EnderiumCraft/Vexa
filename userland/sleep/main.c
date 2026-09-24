/* sleep: wait a number of seconds (fractions allowed, like 0.5). */
#include <stdio.h>
#include <stdlib.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }
    char *rest;
    long ms = strtol(argv[1], &rest, 10) * 1000;
    if (*rest == '.') {
        long scale = 100;
        for (rest++; *rest >= '0' && *rest <= '9' && scale; rest++, scale /= 10) {
            ms += (*rest - '0') * scale;
        }
    }
    return vx_sleep(ms) ? 1 : 0;
}
