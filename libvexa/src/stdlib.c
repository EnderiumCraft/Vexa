#include <stdlib.h>
#include <vexa/syscall.h>

void exit(int code) {
    vx_exit(code);
}
