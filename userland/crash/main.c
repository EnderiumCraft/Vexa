/* Misbehaves on purpose, to check that the kernel protects itself: it must
 * refuse a kernel pointer in a system call, then end this process (and only
 * this process) when it writes to address 0. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(void) {
    long result = vx_log((const char *)0xffffffff80000000, 16);
    if (result == -VX_EFAULT) {
        printf("crash: kernel pointer rejected, as it should be\n");
    } else {
        printf("crash: kernel pointer NOT rejected (result %ld)\n", result);
        return 1;
    }
    printf("crash: now writing to address 0...\n");
    *(volatile int *)0 = 1;
    printf("crash: still running after writing to address 0?!\n");
    return 1;
}
