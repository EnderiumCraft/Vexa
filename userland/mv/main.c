/* mv: rename or move files and directories (on the same disk). */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mv from to, or mv path... directory\n");
        return 2;
    }
    const char *target = argv[argc - 1];
    struct vx_stat st;
    int into_directory = vx_stat(target, &st) == 0 && st.type == VX_TYPE_DIRECTORY;
    int status = 0;
    for (int i = 1; i < argc - 1; i++) {
        char path[512];
        if (into_directory) {
            const char *base = strrchr(argv[i], '/');
            snprintf(path, sizeof(path), "%s/%s", target, base ? base + 1 : argv[i]);
        } else {
            snprintf(path, sizeof(path), "%s", target);
        }
        long error = vx_rename(argv[i], path);
        if (error) {
            fprintf(stderr, "mv: %s: %s%s\n", argv[i], vx_strerror(error),
                    error == -VX_EXDEV ? " (use cp, then rm)" : "");
            status = 1;
        }
    }
    return status;
}
