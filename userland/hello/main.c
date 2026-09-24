/* hello: say hi, and which version of Vexa is running. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(void) {
    struct vx_system_info info;
    vx_system_info(&info);
    printf("hi :)\nVexa %s\n", info.version);
    return 0;
}
