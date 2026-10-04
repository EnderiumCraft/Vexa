#include <stdio.h>
#include <stdlib.h>
#include "internal.h"

void __libvexa_stdio_init(void);
__attribute__((noreturn)) void __libvexa_start_init(long *stack, int (*main)(int, char **, char **),
                                                    void (**init_start)(void), void (**init_end)(void),
                                                    void (**fini_first)(void), void (**fini_last)(void));

char **environ;

/* Called by crt0 with the initial stack and the program's main (passed in,
 * since libvexa.so can't know the program's symbols). */
__attribute__((noreturn)) void __libvexa_start(long *stack, int (*main)(int, char **, char **)) {
    __libvexa_start_init(stack, main, 0, 0, 0, 0);
}

/* The program's .fini_array, run (last to first) when it exits. */
static void (**fini_start)(void), (**fini_end)(void);

static void run_fini_array(void) {
    while (fini_end > fini_start) {
        (*--fini_end)();
    }
}

/* What crt0 calls: as __libvexa_start, and with the program's own
 * .init_array (C++ static constructors, __attribute__((constructor))) run
 * once libvexa is ready, just before main. (The dynamic loader runs only its
 * libraries'.) */
__attribute__((noreturn)) void __libvexa_start_init(long *stack, int (*main)(int, char **, char **),
                                                    void (**init_start)(void), void (**init_end)(void),
                                                    void (**fini_first)(void), void (**fini_last)(void)) {
    int argc = (int)stack[0];
    char **argv = (char **)(stack + 1);
    environ = argv + argc + 1;
    __libvexa_threads_init(); /* First: errno lives in the thread block. */
    __libvexa_stdio_init();
    if (fini_first && fini_last > fini_first) {
        fini_start = fini_first;
        fini_end = fini_last;
        atexit(run_fini_array);
    }
    for (void (**f)(void) = init_start; init_start && f < init_end; f++) {
        (*f)();
    }
    exit(main(argc, argv, environ));
}
