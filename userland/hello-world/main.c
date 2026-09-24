/* The first Vexa program: runs in user mode and talks to the kernel only
 * through system calls. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(void) {
    printf("Hello, world! This is Vexa process %ld, running in user mode.\n", vx_process_id());
    return 0;
}
