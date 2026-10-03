/* libvexa's own: what its parts share, not for programs. */
#ifndef LIBVEXA_INTERNAL_H
#define LIBVEXA_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#define TCB_KEYS 128 /* PTHREAD_KEYS_MAX */

struct __vx_pthread;

/* Every thread's thread pointer (%fs) points at its block: errno, the
 * pthread keys' values, and the thread itself. */
struct __vx_tcb {
    struct __vx_tcb *self; /* %fs:0, so the block can find itself. */
    int error;
    void *specific[TCB_KEYS];
    struct __vx_pthread *thread;
};

static inline struct __vx_tcb *__vx_tcb(void) {
    struct __vx_tcb *tcb;
    __asm__("movq %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

/* errno from a native result: -VX_E* becomes -1 with errno set. */
long __vx_errno_result(long result);
int __vx_errno_of(long vx_error);

void __libvexa_threads_init(void);
/* A thread block (inside a pthread record) for threads started another way. */
struct __vx_tcb *__libvexa_new_tcb(void);
void __libvexa_free_tcb(struct __vx_tcb *tcb);
void __libvexa_run_atexit(void);

#endif
