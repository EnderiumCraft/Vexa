/* dlopen and the rest: Vexa's loader does the work (libvexa/ld/ld.c). */
#include <dlfcn.h>
#include <stddef.h>

/* The loader's, when the program has one (weak: a static program doesn't). */
extern void *__vx_dlopen(const char *file, int flags) __attribute__((weak));
extern void *__vx_dlsym(void *handle, const char *name) __attribute__((weak));
extern int __vx_dlclose(void *handle) __attribute__((weak));
extern const char *__vx_dlerror(void) __attribute__((weak));

static const char *static_error;

void *dlopen(const char *file, int flags) {
    if (!__vx_dlopen) {
        static_error = "dlopen: this program is linked statically";
        return NULL;
    }
    return __vx_dlopen(file, flags);
}

void *dlsym(void *handle, const char *name) {
    if (!__vx_dlsym) {
        static_error = "dlsym: this program is linked statically";
        return NULL;
    }
    return __vx_dlsym(handle == RTLD_NEXT ? RTLD_DEFAULT : handle, name);
}

int dlclose(void *handle) {
    return __vx_dlclose ? __vx_dlclose(handle) : 0;
}

char *dlerror(void) {
    if (static_error) {
        const char *e = static_error;
        static_error = NULL;
        return (char *)e;
    }
    return __vx_dlerror ? (char *)__vx_dlerror() : NULL;
}
