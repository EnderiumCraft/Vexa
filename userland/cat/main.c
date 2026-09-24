/* cat: print files (or standard input, with no arguments or "-") one after another. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

static int copy(int from, const char *name) {
    char buffer[4096];
    long n;
    while ((n = vx_read(from, buffer, sizeof(buffer))) > 0) {
        for (long done = 0; done < n;) {
            long w = vx_write(1, buffer + done, n - done);
            if (w <= 0) {
                return 1;
            }
            done += w;
        }
    }
    if (n < 0 && n != -VX_EINTR) {
        fprintf(stderr, "cat: %s: %s\n", name, vx_strerror(n));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return copy(0, "standard input");
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            status |= copy(0, "standard input");
            continue;
        }
        int file = vx_open(argv[i], VX_OPEN_READ);
        if (file < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], vx_strerror(file));
            status = 1;
            continue;
        }
        status |= copy(file, argv[i]);
        vx_close(file);
    }
    return status;
}
