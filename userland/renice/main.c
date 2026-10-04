/* renice: changes running processes' priority: renice N process-id...
 * (N from -20, first, to 19, last). */
#include <stdio.h>
#include <stdlib.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: renice nice process-id...\n");
        return 2;
    }
    int nice = atoi(argv[1]), status = 0;
    for (int i = 2; i < argc; i++) {
        int now;
        long error = vx_priority(atol(argv[i]), nice, &now);
        if (error) {
            fprintf(stderr, "renice: %s: %s\n", argv[i], vx_strerror(error));
            status = 1;
        } else {
            printf("%s: nice %d\n", argv[i], now);
        }
    }
    return status;
}
