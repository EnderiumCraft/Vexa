/* rm: remove files, and empty directories. -r removes directories with
 * everything in them. */
#include <stdio.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/syscall.h>

static int remove_tree(const char *path) {
    long error = vx_remove_tree(path);
    if (error) {
        fprintf(stderr, "rm: %s: %s\n", path, vx_strerror(error));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int recursive = argc > 1 && (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "-rf") == 0);
    if (argc < 2 + recursive) {
        fprintf(stderr, "usage: rm [-r] path...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1 + recursive; i < argc; i++) {
        if (recursive) {
            status |= remove_tree(argv[i]);
            continue;
        }
        long error = vx_remove(argv[i]);
        if (error) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], vx_strerror(error));
            status = 1;
        }
    }
    return status;
}
