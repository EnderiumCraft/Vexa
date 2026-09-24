/* kill: send a signal to processes (SIGTERM unless given, e.g. kill -9 12). */
#include <stdio.h>
#include <stdlib.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    int signal = VX_SIGTERM, first = 1;
    if (argc > 1 && argv[1][0] == '-') {
        signal = atoi(argv[1] + 1);
        first = 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: kill [-signal] process-id...\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        long error = vx_kill(atol(argv[i]), signal);
        if (error) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], vx_strerror(error));
            status = 1;
        }
    }
    return status;
}
