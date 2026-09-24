/* uptime: how long since boot, CPUs and memory. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(void) {
    struct vx_system_info info;
    if (vx_system_info(&info)) {
        return 1;
    }
    unsigned long long s = info.uptime_ms / 1000;
    printf("up %llu:%02llu:%02llu, %u CPUs, %llu of %llu MiB memory free\n", s / 3600,
           s / 60 % 60, s % 60, info.cpus, info.memory_free >> 20, info.memory_total >> 20);
    return 0;
}
