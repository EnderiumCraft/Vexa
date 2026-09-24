/* mkdir: make directories. -p makes missing parents too, and doesn't mind if
 * the directory already exists. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    int parents = argc > 1 && strcmp(argv[1], "-p") == 0;
    if (argc < 2 + parents) {
        fprintf(stderr, "usage: mkdir [-p] directory...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1 + parents; i < argc; i++) {
        if (parents) {
            char path[512];
            snprintf(path, sizeof(path), "%s", argv[i]);
            for (char *p = path + 1; *p; p++) {
                if (*p == '/') {
                    *p = '\0';
                    vx_mkdir(path);
                    *p = '/';
                }
            }
        }
        long error = vx_mkdir(argv[i]);
        if (error && !(parents && error == -VX_EEXIST)) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], vx_strerror(error));
            status = 1;
        }
    }
    return status;
}
