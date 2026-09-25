#include <stdbool.h>
#include <stdlib.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>

#define STACK_SIZE (256 * 1024)

struct vx_thread {
    void *(*fn)(void *);
    void *arg;
    void *result;
    void *stack;
    volatile unsigned running; /* The kernel sets it to 0 (and wakes us) at exit. */
};

__attribute__((noreturn)) static void thread_entry(struct vx_thread *thread) {
    thread->result = thread->fn(thread->arg);
    vx_thread_exit(0);
}

struct vx_thread *vx_thread_create(void *(*fn)(void *), void *arg) {
    struct vx_thread *thread = malloc(sizeof(*thread));
    void *stack = thread ? vx_map(STACK_SIZE, VX_MAP_WRITE) : NULL;
    if (!stack) {
        free(thread);
        return NULL;
    }
    thread->fn = fn;
    thread->arg = arg;
    thread->result = NULL;
    thread->stack = stack;
    thread->running = 1;
    struct vx_thread_start start = {
        .entry = (void *)thread_entry,
        /* As if thread_entry had just been called: rsp is 8 below a 16-byte boundary. */
        .stack = (char *)stack + STACK_SIZE - 8,
        .arg = thread,
        .tls = NULL,
        .exit_word = (unsigned *)&thread->running,
    };
    if (vx_thread_start(&start) < 0) {
        vx_unmap(stack, STACK_SIZE);
        free(thread);
        return NULL;
    }
    return thread;
}

void *vx_thread_join(struct vx_thread *thread) {
    while (thread->running) {
        vx_wait_address(&thread->running, 1, -1);
    }
    void *result = thread->result;
    vx_unmap(thread->stack, STACK_SIZE);
    free(thread);
    return result;
}

/* The classic three-state futex mutex (Ulrich Drepper, "Futexes Are Tricky"):
 * uncontended locking and unlocking never enter the kernel. */
void vx_mutex_lock(struct vx_mutex *mutex) {
    unsigned c = 0;
    if (__atomic_compare_exchange_n(&mutex->state, &c, 1, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        return;
    }
    if (c != 2) {
        c = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
    }
    while (c != 0) {
        vx_wait_address(&mutex->state, 2, -1);
        c = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
    }
}

void vx_mutex_unlock(struct vx_mutex *mutex) {
    if (__atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE) == 2) {
        vx_wake_address(&mutex->state, 1);
    }
}
