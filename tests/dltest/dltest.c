/* libvexa-test.so: a shared library for posix-test to load with dlopen. It
 * uses libvexa (strlen), its initializer runs when it's loaded, and it has
 * a thread-local variable (each thread's copy starts at 7). */
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

static __thread int each_thread = 7;

int *vexa_test_tls(void) {
    return &each_thread;
}
