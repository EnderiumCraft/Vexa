/* nice: runs a command at a lower (or, with a negative -n, higher) priority:
 * nice [-n N] command [arguments...]. N is added to this one's nice value
 * (10 if not given). */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <vexa/syscall.h>

extern char **environ;

int main(int argc, char **argv) {
    int adjust = 10, first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        adjust = atoi(argv[2]);
        first = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        adjust = atoi(argv[1] + 1); /* The old form: nice -10 command */
        first = 2;
    }
    int nice = 0;
    vx_priority(0, VX_PRIORITY_GET, &nice);
    if (first >= argc) {
        printf("%d\n", nice); /* No command: this one's nice value. */
        return 0;
    }
    /* The child inherits it. */
    vx_priority(0, nice + adjust, NULL);
    pid_t child;
    int error = posix_spawnp(&child, argv[first], NULL, NULL, argv + first, environ);
    if (error) {
        fprintf(stderr, "nice: %s: %s\n", argv[first], strerror(error));
        return 127;
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WEXITSTATUS(status);
}
