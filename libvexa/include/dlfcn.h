#ifndef LIBVEXA_DLFCN_H
#define LIBVEXA_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Loading shared libraries while a program runs (through Vexa's loader,
 * /lib/vexa-ld.so; not in statically linked programs). Every symbol is bound
 * at once and every library is visible to the others, whatever the flags. */
#define RTLD_LAZY 0x0001
#define RTLD_NOW 0x0002
#define RTLD_NOLOAD 0x0004
#define RTLD_LOCAL 0x0000
#define RTLD_GLOBAL 0x0100
#define RTLD_NODELETE 0x1000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT ((void *)-1)

/* A name without a slash is looked for in /lib; NULL: the program itself. */
void *dlopen(const char *file, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
