/* pwd: print the current directory. */
#include <stdio.h>
#include <vexa/syscall.h>

int main(void) {
    char cwd[4096];
    long error = vx_getcwd(cwd, sizeof(cwd));
    if (error < 0) {
        fprintf(stderr, "pwd: %s\n", vx_strerror(error));
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}
