/* echo: print the arguments. -n leaves off the newline. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int first = 1;
    int newline = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        first = 2;
    }
    for (int i = first; i < argc; i++) {
        printf(i > first ? " %s" : "%s", argv[i]);
    }
    if (newline) {
        printf("\n");
    }
    return 0;
}
