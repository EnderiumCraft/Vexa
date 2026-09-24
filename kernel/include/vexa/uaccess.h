#ifndef VEXA_UACCESS_H
#define VEXA_UACCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* User space is the lower half of the address space, minus the first page so
 * that null pointers always fault. */
#define USER_BASE 0x1000ULL
#define USER_END 0x0000800000000000ULL

extern bool smap_enabled;

/* With SMAP on, the kernel may only touch user memory between these two. */
static inline void user_access_begin(void) {
    if (smap_enabled) {
        __asm__ volatile("stac" : : : "memory");
    }
}

static inline void user_access_end(void) {
    if (smap_enabled) {
        __asm__ volatile("clac" : : : "memory");
    }
}

/* Copy between kernel and the current process's memory. They check that the
 * whole range is mapped user memory (writable, for copy_to_user) and return
 * false instead of faulting if not. */
bool copy_from_user(void *dest, uint64_t user_src, size_t size);
bool copy_to_user(uint64_t user_dest, const void *src, size_t size);

#endif
