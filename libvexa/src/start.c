#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv, char **envp);
void __libvexa_stdio_init(void);

char **environ;

__attribute__((noreturn)) void __libvexa_start(long *stack) {
    int argc = (int)stack[0];
    char **argv = (char **)(stack + 1);
    environ = argv + argc + 1;
    __libvexa_stdio_init();
    exit(main(argc, argv, environ));
}
