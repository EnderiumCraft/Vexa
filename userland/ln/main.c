/* ln: make symbolic links. `ln -s target name`; with a directory as the last
 * argument, the links go inside it. (Vexa has no hard links yet, so -s is
 * required.) */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[1], "-s") != 0) {
        fprintf(stderr, "usage: ln -s target name, or ln -s target... directory\n");
        return 2;
    }
    const char *last = argv[argc - 1];
    struct vx_stat st;
    int into_directory = vx_stat(last, &st) == 0 && st.type == VX_TYPE_DIRECTORY;
    if (argc > 4 && !into_directory) {
        fprintf(stderr, "ln: %s is not a directory\n", last);
        return 1;
    }
    int status = 0;
    for (int i = 2; i < argc - 1; i++) {
        char path[512];
        if (into_directory) {
            const char *base = strrchr(argv[i], '/');
            snprintf(path, sizeof(path), "%s/%s", last, base ? base + 1 : argv[i]);
        } else {
            snprintf(path, sizeof(path), "%s", last);
        }
        long error = vx_symlink(argv[i], path);
        if (error) {
            fprintf(stderr, "ln: %s: %s\n", path, vx_strerror(error));
            status = 1;
        }
    }
    return status;
}
