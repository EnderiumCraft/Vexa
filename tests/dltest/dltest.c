/* libvexa-test.so: a shared library for posix-test to load with dlopen. It
 * uses libvexa (strlen), and its initializer runs when it's loaded. */
#include <string.h>

int vexa_test_loaded;

__attribute__((constructor)) static void loaded(void) {
    vexa_test_loaded = 42;
}

int vexa_test_add(int a, int b) {
    return a + b;
}

int vexa_test_length(const char *text) {
    return (int)strlen(text);
}
