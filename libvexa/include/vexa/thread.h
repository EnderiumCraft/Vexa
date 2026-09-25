#ifndef VEXA_THREAD_H
#define VEXA_THREAD_H

/* Threads and locks for Vexa programs.
 *
 *     struct vx_thread *t = vx_thread_create(work, arg);
 *     void *result = vx_thread_join(t);
 *
 *     static struct vx_mutex lock = VX_MUTEX_INIT;
 *     vx_mutex_lock(&lock); ... vx_mutex_unlock(&lock);
 */

struct vx_thread;

/* Starts fn(arg) in a new thread (with a 256 KiB stack). NULL on failure. */
struct vx_thread *vx_thread_create(void *(*fn)(void *), void *arg);
/* Waits for the thread to finish, frees it, and returns what fn returned. */
void *vx_thread_join(struct vx_thread *thread);

/* 0: unlocked, 1: locked, 2: locked and someone may be waiting. */
struct vx_mutex {
    volatile unsigned state;
};
#define VX_MUTEX_INIT {0}

void vx_mutex_lock(struct vx_mutex *mutex);
void vx_mutex_unlock(struct vx_mutex *mutex);

#endif
