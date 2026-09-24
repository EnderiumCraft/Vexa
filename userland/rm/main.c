/* rm: remove files, and empty directories. -r removes directories with
 * everything in them. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

static int remove_tree(const char *path) {
    struct vx_stat st;
    if (vx_stat(path, &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
        int dir = vx_open(path, VX_OPEN_READ);
        if (dir >= 0) {
            struct vx_dir_entry entry;
            char child[512];
            /* Removing entries while listing shifts later ones, so start over each time. */
            for (;;) {
                long n = vx_read_dir(dir, &entry, 1);
                if (n <= 0) {
                    break;
                }
                if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
                    continue;
                }
                snprintf(child, sizeof(child), "%s/%s", path, entry.name);
                if (remove_tree(child)) {
                    vx_close(dir);
                    return 1;
                }
                vx_close(dir);
                dir = vx_open(path, VX_OPEN_READ);
                if (dir < 0) {
                    break;
                }
            }
            if (dir >= 0) {
                vx_close(dir);
            }
        }
    }
    long error = vx_remove(path);
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
