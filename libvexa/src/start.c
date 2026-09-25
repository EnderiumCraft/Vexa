#include <stdio.h>
#include <stdlib.h>

void __libvexa_stdio_init(void);

char **environ;

/* Called by crt0 with the initial stack and the program's main (passed in,
 * since libvexa.so can't know the program's symbols). */
__attribute__((noreturn)) void __libvexa_start(long *stack, int (*main)(int, char **, char **)) {
    int argc = (int)stack[0];
    char **argv = (char **)(stack + 1);
    environ = argv + argc + 1;
    __libvexa_stdio_init();
    exit(main(argc, argv, environ));
}
