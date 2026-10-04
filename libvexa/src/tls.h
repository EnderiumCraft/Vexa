/* Thread-local storage (__thread, thread_local): what the loader tells
 * libvexa about each module's TLS (shared by libvexa/ld/ld.c and tls.c).
 *
 * x86-64's "variant II": a thread's static TLS sits just below its thread
 * pointer (%fs), each startup module at tp - offset (the program first, so
 * the code the linker relaxed to %fs:-n finds it). Modules dlopen loads
 * later get their blocks on first use, through __tls_get_addr. Module IDs
 * are the loader's object index plus one (the program is module 1). */
#ifndef LIBVEXA_TLS_H
#define LIBVEXA_TLS_H

#include <stdint.h>

#define VX_TLS_MODULES 33 /* (The loader's MAX_OBJECTS, plus one: IDs start at 1.) */

struct vx_tls_module {
    const void *image;     /* .tdata's initial contents (0 if none). */
    uint64_t filesz;       /* Bytes from the image; the rest, to memsz, is zero. */
    uint64_t memsz;        /* 0: this module has no TLS. */
    uint64_t align;
    uint64_t offset;       /* Static TLS: at tp - offset. 0: dynamic. */
};

struct vx_tls_table {
    uint64_t static_size;  /* Bytes below tp (a multiple of static_align). */
    uint64_t static_align;
    struct vx_tls_module modules[VX_TLS_MODULES]; /* [0] unused. */
};

#endif
