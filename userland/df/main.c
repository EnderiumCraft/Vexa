/* df: the mounted file systems, and how full they are. */
#include <stdio.h>
#include <vexa/syscall.h>

static void size(char *out, size_t length, unsigned long long bytes) {
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(out, length, "%llu.%llu GiB", bytes >> 30, (bytes * 10 >> 30) % 10);
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, length, "%llu.%llu MiB", bytes >> 20, (bytes * 10 >> 20) % 10);
    } else {
        snprintf(out, length, "%llu KiB", bytes >> 10);
    }
}

int main(void) {
    static struct vx_mount_info mounts[32];
    long n = vx_mounts(mounts, 32);
    if (n < 0) {
        fprintf(stderr, "df: %s\n", vx_strerror(n));
        return 1;
    }
    printf("%-14s %-9s %11s %11s %5s  %s\n", "Mounted on", "Type", "Size", "Free", "Used", "From");
    for (long i = 0; i < n && i < 32; i++) {
        struct vx_mount_info *m = &mounts[i];
        char total[24] = "-", free[24] = "-", used[8] = "-";
        if (m->total) {
            size(total, sizeof(total), m->total);
            size(free, sizeof(free), m->free);
            snprintf(used, sizeof(used), "%llu%%", (m->total - m->free) * 100 / m->total);
        }
        printf("%-14s %-9s %11s %11s %5s  %s%s\n", m->path, m->type, total, free, used, m->source,
               m->read_only ? " (read-only)" : "");
    }
    return 0;
}
