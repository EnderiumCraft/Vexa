/* shutdown: turns the machine off, or restarts it with -r. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    int restart = argc > 1 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "--restart"));
    if (argc > 1 && !restart) {
        fprintf(stderr, "usage: shutdown [-r]\n");
        return 2;
    }
    long error = vx_power(restart ? VX_POWER_RESTART : VX_POWER_OFF);
    fprintf(stderr, "shutdown: %s\n", vx_strerror(error));
    return 1;
}
