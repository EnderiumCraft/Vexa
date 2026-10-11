/* Called when a function's stack canary is wrong. Programs built with
 * -fstack-protector (SpiderMonkey's are) link against this. */
#include <stdlib.h>
#include <vexa/syscall.h>

void __stack_chk_fail(void) {
    static const char message[] = "libvexa: stack smashing detected\n";
    vx_write(2, message, sizeof(message) - 1);
    abort();
}
