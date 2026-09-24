/* sys: run a kernel monitor command, e.g. `sys memtest`, `sys threads`,
 * `sys disks`. With no command, lists them. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    char command[128] = "help";
    if (argc > 1) {
        command[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (strlen(command) + strlen(argv[i]) + 2 >= sizeof(command)) {
                fprintf(stderr, "sys: command too long\n");
                return 2;
            }
            if (i > 1) {
                strcat(command, " ");
            }
            strcat(command, argv[i]);
        }
    }
    fflush(stdout);
    long error = vx_kernel_command(command);
    if (error) {
        fprintf(stderr, "sys: %s\n", vx_strerror(error));
        return 1;
    }
    return 0;
}
