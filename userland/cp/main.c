/* cp: copy a file, or several files into a directory. -r copies
 * directories with everything in them. */
#include <stdio.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/syscall.h>

static int copy(const char *from, const char *to) {
    int in = vx_open(from, VX_OPEN_READ);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", from, vx_strerror(in));
        return 1;
    }
    int out = vx_open(to, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", to, vx_strerror(out));
        vx_close(in);
        return 1;
    }
    char buffer[8192];
    long n;
    int status = 0;
    while ((n = vx_read(in, buffer, sizeof(buffer))) > 0) {
        if (vx_write(out, buffer, n) != n) {
            fprintf(stderr, "cp: writing %s failed\n", to);
            status = 1;
            break;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: %s: %s\n", from, vx_strerror(n));
        status = 1;
    }
    vx_close(in);
    vx_close(out);
    return status;
}

int main(int argc, char **argv) {
    int recursive = argc > 1 && strcmp(argv[1], "-r") == 0;
    argv += recursive;
    argc -= recursive;
    if (argc < 3) {
        fprintf(stderr, "usage: cp [-r] from to, or cp [-r] file... directory\n");
        return 2;
    }
    const char *target = argv[argc - 1];
    struct vx_stat st;
    int into_directory = vx_stat(target, &st) == 0 && st.type == VX_TYPE_DIRECTORY;
    if (argc > 3 && !into_directory) {
        fprintf(stderr, "cp: %s is not a directory\n", target);
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc - 1; i++) {
        char path[512];
        if (into_directory) {
            const char *base = strrchr(argv[i], '/');
            snprintf(path, sizeof(path), "%s/%s", target, base ? base + 1 : argv[i]);
        } else {
            snprintf(path, sizeof(path), "%s", target);
        }
        if (vx_stat(argv[i], &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
            long error = recursive ? vx_copy_tree(argv[i], path) : -VX_EISDIR;
            if (error) {
                fprintf(stderr, "cp: %s: %s%s\n", argv[i], vx_strerror(error),
                        recursive ? "" : " (use -r)");
                status = 1;
            }
            continue;
        }
        status |= copy(argv[i], path);
    }
    return status;
}
