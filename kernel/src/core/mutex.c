#include <vexa/cpu.h>
#include <vexa/kprintf.h>
#include <vexa/mutex.h>

/* wait_queue_wait() evaluates this with the scheduler lock held, so two
 * threads can never both see the mutex free. */
static bool try_acquire(void *arg) {
    struct mutex *mutex = arg;
    if (mutex->locked) {
        return false;
    }
    mutex->locked = true;
    mutex->owner = thread_current();
    return true;
}

void mutex_lock(struct mutex *mutex) {
    if (!sched_running()) {
        /* Early boot: only one thread exists, so the lock is always free. */
        mutex->locked = true;
        return;
    }
    if (mutex->owner && mutex->owner == thread_current()) {
        panic("mutex_lock: thread %s already holds this lock", mutex->owner->name);
    }
    wait_queue_wait(&mutex->waiters, try_acquire, mutex);
}

void mutex_unlock(struct mutex *mutex) {
    mutex->owner = NULL;
    __atomic_store_n(&mutex->locked, false, __ATOMIC_RELEASE);
    if (sched_running()) {
        wait_queue_wake_all(&mutex->waiters);
    }
}

bool mutex_held(struct mutex *mutex) {
    return mutex->locked && mutex->owner == thread_current();
}
