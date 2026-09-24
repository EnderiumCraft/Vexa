/* ps: list processes. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(void) {
    static struct vx_process_info list[256];
    long n = vx_process_list(list, 256);
    if (n < 0) {
        fprintf(stderr, "ps: %s\n", vx_strerror(n));
        return 1;
    }
    printf("   id  parent  group  state    name\n");
    for (long i = n - 1; i >= 0; i--) {
        printf("%5u  %6u  %5u  %-7s  %s\n", list[i].id, list[i].parent, list[i].group,
               list[i].state == 0 ? "running" : "exited", list[i].name);
    }
    return 0;
}
