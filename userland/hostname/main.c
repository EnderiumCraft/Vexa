/* hostname: shows the computer's name, or sets it: hostname [name]. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    if (argc > 1) {
        long error = vx_set_hostname(argv[1]);
        if (error) {
            fprintf(stderr, "hostname: %s: %s (letters, digits, '-' and '.')\n", argv[1],
                    vx_strerror(error));
            return 1;
        }
    }
    char name[80];
    long error = vx_get_hostname(name, sizeof(name));
    if (error) {
        fprintf(stderr, "hostname: %s\n", vx_strerror(error));
        return 1;
    }
    printf("%s\n", name);
    return 0;
}
