#ifndef VEXA_MUTEX_H
#define VEXA_MUTEX_H

#include <stdbool.h>
#include <vexa/sched.h>

/* A lock that puts waiting threads to sleep instead of spinning, so it can be
 * held across slow operations like disk I/O. Unlike spinlocks, interrupts stay
 * enabled. Never take one in an interrupt handler. */
struct mutex {
    volatile bool locked;
    struct thread *owner;
    struct wait_queue waiters;
};

#define MUTEX_INIT {false, 0, WAIT_QUEUE_INIT}

void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
bool mutex_held(struct mutex *mutex); /* By the current thread. */

#endif
