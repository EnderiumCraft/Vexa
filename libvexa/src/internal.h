/* libvexa's own: what its parts share, not for programs. */
#ifndef LIBVEXA_INTERNAL_H
#define LIBVEXA_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include "tls.h"

#define TCB_KEYS 128 /* PTHREAD_KEYS_MAX */

struct __vx_pthread;

/* Every thread's thread pointer (%fs) points at its block: errno, the
 * pthread keys' values, and the thread itself. */
struct __vx_tcb {
    struct __vx_tcb *self; /* %fs:0, so the block can find itself. */
    int error;
    void *specific[TCB_KEYS];
    struct __vx_pthread *thread;
    void *dtv[VX_TLS_MODULES]; /* Each TLS module's block in this thread (tls.h). */
    void *block;            /* The memory: static TLS, then this record. */
    struct __vx_thread_dtor *dtors; /* C++ thread_local destructors (stdlib.c). */
    size_t block_size;
};

static inline struct __vx_tcb *__vx_tcb(void) {
    struct __vx_tcb *tcb;
    __asm__("movq %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

/* errno from a native result: -VX_E* becomes -1 with errno set. */
long __vx_errno_result(long result);
int __vx_errno_of(long vx_error);

/* Sockets (socket.c): which descriptors are non-blocking, and sockets. */
bool __vx_nonblocking(int fd);
void __vx_set_nonblocking(int fd, bool on);
bool __vx_is_socket(int fd);
void __vx_forget_fd(int fd);

void __libvexa_threads_init(void);
/* Thread-local storage (tls.c): a thread record of `size` bytes (starting
 * with its struct __vx_tcb, zeroed) with its static TLS below it. */
void __libvexa_tls_init(void);
struct __vx_tcb *__libvexa_tcb_alloc(size_t size);
void __libvexa_tcb_free(struct __vx_tcb *tcb);
/* A thread block (inside a pthread record) for threads started another way. */
struct __vx_tcb *__libvexa_new_tcb(void);
void __libvexa_free_tcb(struct __vx_tcb *tcb);
void __libvexa_run_atexit(void);
/* Runs the calling thread's thread_local destructors (at its exit). */
void __libvexa_run_thread_dtors(void);

#endif
