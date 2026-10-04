/* POSIX threads over Vexa's own: vx_thread_start for the threads (each with
 * its thread block as its thread pointer), and the wait-on-address calls for
 * mutexes, condition variables and semaphores. */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include "internal.h"

#define DEFAULT_STACK (512 * 1024)

struct __vx_pthread {
    struct __vx_tcb tcb;
    void *(*fn)(void *);
    void *arg;
    void *result;
    void *stack;
    size_t stack_size;
    volatile unsigned running; /* The kernel clears it (and wakes joiners) at the very end. */
    volatile int detached;
    struct __vx_pthread *next_finished; /* Detached and done: freed by the next create. */
};

static struct __vx_pthread *main_thread;
static struct vx_mutex finished_lock = VX_MUTEX_INIT;
static struct __vx_pthread *finished;

/* The main thread's block: set up before main() runs. */
void __libvexa_threads_init(void) {
    __libvexa_tls_init();
    main_thread = (struct __vx_pthread *)__libvexa_tcb_alloc(sizeof(*main_thread));
    if (!main_thread) {
        static const char message[] = "libvexa: out of memory starting the program\n";
        vx_write(2, message, sizeof(message) - 1);
        vx_exit(127);
    }
    main_thread->tcb.thread = main_thread;
    main_thread->running = 1;
    vx_set_thread_pointer(&main_thread->tcb);
}

/* A thread record, with its static TLS (tls.c); zeroed. */
static struct __vx_pthread *new_thread(void) {
    struct __vx_pthread *t = (struct __vx_pthread *)__libvexa_tcb_alloc(sizeof(*t));
    if (t) {
        t->tcb.thread = t;
        t->running = 1;
    }
    return t;
}

static void free_thread(struct __vx_pthread *t) {
    if (t) {
        __libvexa_tcb_free(&t->tcb);
    }
}

struct __vx_tcb *__libvexa_new_tcb(void) {
    struct __vx_pthread *t = new_thread();
    return t ? &t->tcb : NULL;
}

void __libvexa_free_tcb(struct __vx_tcb *tcb) {
    free_thread(tcb ? tcb->thread : NULL);
}

static void (*key_destructors[TCB_KEYS])(void *);
static bool key_used[TCB_KEYS];
static struct vx_mutex key_lock = VX_MUTEX_INIT;

static void run_destructors(struct __vx_tcb *tcb) {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < TCB_KEYS; k++) {
            void *value = tcb->specific[k];
            if (value && key_destructors[k]) {
                tcb->specific[k] = NULL;
                key_destructors[k](value);
                any = true;
            }
        }
        if (!any) {
            break;
        }
    }
}

/* Frees detached threads that have finished (their stacks can go once the
 * kernel has said they're done). */
static void reap(void) {
    vx_mutex_lock(&finished_lock);
    struct __vx_pthread **link = &finished;
    while (*link) {
        struct __vx_pthread *t = *link;
        if (!t->running) {
            *link = t->next_finished;
            vx_unmap(t->stack, t->stack_size);
            free_thread(t);
        } else {
            link = &t->next_finished;
        }
    }
    vx_mutex_unlock(&finished_lock);
}

__attribute__((noreturn)) static void finish(struct __vx_pthread *t, void *result) {
    t->result = result;
    if (t != main_thread) { /* (exit runs the main thread's.) */
        __libvexa_run_thread_dtors();
    }
    run_destructors(&t->tcb);
    if (t == main_thread) {
        exit(0);
    }
    if (t->detached) {
        vx_mutex_lock(&finished_lock);
        t->next_finished = finished;
        finished = t;
        vx_mutex_unlock(&finished_lock);
    }
    vx_thread_exit(0);
}

__attribute__((noreturn)) static void thread_entry(struct __vx_pthread *t) {
    finish(t, t->fn(t->arg));
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    reap();
    size_t stack_size = attr && attr->stack_size ? attr->stack_size : DEFAULT_STACK;
    stack_size = (stack_size + 4095) & ~(size_t)4095;
    struct __vx_pthread *t = new_thread();
    void *stack = t ? vx_map(stack_size, VX_MAP_WRITE) : NULL;
    if (!stack) {
        free_thread(t);
        return EAGAIN;
    }
    t->fn = fn;
    t->arg = arg;
    t->stack = stack;
    t->stack_size = stack_size;
    t->running = 1;
    t->detached = attr && attr->detached;
    struct vx_thread_start start = {
        .entry = (void *)thread_entry,
        .stack = (char *)stack + stack_size - 8, /* As if called: 8 below a 16-byte line. */
        .arg = t,
        .tls = &t->tcb,
        .exit_word = (unsigned *)&t->running,
    };
    if (vx_thread_start(&start) < 0) {
        vx_unmap(stack, stack_size);
        free_thread(t);
        return EAGAIN;
    }
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **result) {
    if (t == pthread_self()) {
        return EDEADLK;
    }
    while (t->running) {
        vx_wait_address(&t->running, 1, -1);
    }
    if (result) {
        *result = t->result;
    }
    vx_unmap(t->stack, t->stack_size);
    free_thread(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    vx_mutex_lock(&finished_lock);
    t->detached = 1;
    vx_mutex_unlock(&finished_lock);
    if (!t->running) { /* Already gone: nobody will join it. */
        vx_unmap(t->stack, t->stack_size);
        free_thread(t);
    }
    return 0;
}

void pthread_exit(void *result) {
    finish(pthread_self(), result);
}

pthread_t pthread_self(void) {
    return __vx_tcb()->thread;
}

int pthread_equal(pthread_t a, pthread_t b) {
    return a == b;
}

int pthread_setname_np(pthread_t t, const char *name) {
    (void)t, (void)name;
    return 0;
}

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *param) {
    (void)t, (void)policy, (void)param;
    return 0;
}

int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *param) {
    (void)t;
    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

int pthread_setcancelstate(int state, int *old) {
    (void)state;
    if (old) {
        *old = PTHREAD_CANCEL_ENABLE;
    }
    return 0;
}

int pthread_sigmask(int how, const void *set, void *old) {
    (void)how, (void)set, (void)old;
    return 0;
}

/* ---- Attributes ---- */

int pthread_attr_init(pthread_attr_t *attr) {
    attr->stack_size = 0;
    attr->detached = 0;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (size < PTHREAD_STACK_MIN) {
        return EINVAL;
    }
    attr->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size) {
    *size = attr->stack_size ? attr->stack_size : DEFAULT_STACK;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
    attr->detached = state == PTHREAD_CREATE_DETACHED;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state) {
    *state = attr->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE;
    return 0;
}

/* ---- Mutexes (Drepper's three-state futex mutex) ---- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    m->state = 0;
    m->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
    m->owner = NULL;
    m->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

static bool try_lock(pthread_mutex_t *m) {
    unsigned expected = 0;
    return __atomic_compare_exchange_n(&m->state, &expected, 1, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

static int lock_for(pthread_mutex_t *m, long timeout_ms) {
    pthread_t self = pthread_self();
    if (m->type != PTHREAD_MUTEX_NORMAL && m->owner == self) {
        if (m->type == PTHREAD_MUTEX_ERRORCHECK) {
            return EDEADLK;
        }
        m->count++;
        return 0;
    }
    if (!try_lock(m)) {
        long deadline = timeout_ms >= 0 ? vx_uptime() + timeout_ms : 0;
        unsigned c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
        while (c != 0) {
            long wait = -1;
            if (timeout_ms >= 0) {
                wait = deadline - vx_uptime();
                if (wait <= 0) {
                    return ETIMEDOUT;
                }
            }
            vx_wait_address(&m->state, 2, wait);
            c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
        }
    }
    m->owner = self;
    m->count = 1;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    return lock_for(m, -1);
}

static long ms_until(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long ms = (deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
    return ms < 0 ? 0 : ms;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *deadline) {
    return lock_for(m, ms_until(deadline));
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner == pthread_self()) {
        m->count++;
        return 0;
    }
    if (!try_lock(m)) {
        return EBUSY;
    }
    m->owner = pthread_self();
    m->count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != pthread_self()) {
            return EPERM;
        }
        if (--m->count) {
            return 0;
        }
    }
    m->owner = NULL;
    if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2) {
        vx_wake_address(&m->state, 1);
    }
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    *type = attr->type;
    return 0;
}

/* ---- Condition variables: a sequence number to wait on ---- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
    (void)attr;
    c->sequence = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

static int cond_wait_for(pthread_cond_t *c, pthread_mutex_t *m, long timeout_ms) {
    unsigned seen = __atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE);
    /* (A recursive mutex is let go of entirely, and taken back as it was.) */
    unsigned count = m->count;
    m->count = 1;
    pthread_mutex_unlock(m);
    long result = vx_wait_address(&c->sequence, seen, timeout_ms);
    pthread_mutex_lock(m);
    m->count = count;
    return result == -VX_ETIMEDOUT ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return cond_wait_for(c, m, -1);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    return cond_wait_for(c, m, ms_until(deadline));
}

int pthread_cond_signal(pthread_cond_t *c) {
    __atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
    vx_wake_address(&c->sequence, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    __atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
    vx_wake_address(&c->sequence, INT_MAX);
    return 0;
}

/* Barriers: the last of `count` threads to arrive starts the next round and
 * wakes the others (it gets PTHREAD_BARRIER_SERIAL_THREAD). */
int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count) {
    (void)attr;
    if (count == 0) {
        return EINVAL;
    }
    pthread_mutex_init(&b->lock, 0);
    pthread_cond_init(&b->done, 0);
    b->count = count;
    b->waiting = 0;
    b->round = 0;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) {
    (void)b;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->lock);
    unsigned round = b->round;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->round++;
        pthread_cond_broadcast(&b->done);
        pthread_mutex_unlock(&b->lock);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (round == b->round) {
        pthread_cond_wait(&b->done, &b->lock);
    }
    pthread_mutex_unlock(&b->lock);
    return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t *attr) {
    attr->unused = 0;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    attr->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) {
    attr->clock = clock;
    return 0;
}

/* ---- Read-write locks ---- */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr) {
    (void)attr;
    memset(l, 0, sizeof(*l));
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l) {
    (void)l;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->lock);
    while (l->writer || l->writers_waiting) {
        pthread_cond_wait(&l->changed, &l->lock);
    }
    l->readers++;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->lock);
    int result = l->writer || l->writers_waiting ? EBUSY : (l->readers++, 0);
    pthread_mutex_unlock(&l->lock);
    return result;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->lock);
    l->writers_waiting++;
    while (l->writer || l->readers) {
        pthread_cond_wait(&l->changed, &l->lock);
    }
    l->writers_waiting--;
    l->writer = 1;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->lock);
    int result = l->writer || l->readers ? EBUSY : (l->writer = 1, 0);
    pthread_mutex_unlock(&l->lock);
    return result;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    pthread_mutex_lock(&l->lock);
    if (l->writer) {
        l->writer = 0;
    } else if (l->readers) {
        l->readers--;
    }
    pthread_cond_broadcast(&l->changed);
    pthread_mutex_unlock(&l->lock);
    return 0;
}

/* ---- Spin locks ---- */

int pthread_spin_init(pthread_spinlock_t *l, int shared) {
    (void)shared;
    *l = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *l) {
    (void)l;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            __builtin_ia32_pause();
        }
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *l) {
    return __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int pthread_spin_unlock(pthread_spinlock_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
    return 0;
}

/* ---- Once and keys ---- */

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    /* 0: not yet; 1: running; 2: done. */
    int expected = 0;
    if (__atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        vx_wake_address((volatile unsigned *)once, INT_MAX);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) {
        vx_wait_address((volatile unsigned *)once, 1, -1);
    }
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    vx_mutex_lock(&key_lock);
    for (unsigned k = 0; k < TCB_KEYS; k++) {
        if (!key_used[k]) {
            key_used[k] = true;
            key_destructors[k] = destructor;
            vx_mutex_unlock(&key_lock);
            *key = k;
            return 0;
        }
    }
    vx_mutex_unlock(&key_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= TCB_KEYS) {
        return EINVAL;
    }
    vx_mutex_lock(&key_lock);
    key_used[key] = false;
    key_destructors[key] = NULL;
    vx_mutex_unlock(&key_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    return key < TCB_KEYS ? __vx_tcb()->specific[key] : NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= TCB_KEYS) {
        return EINVAL;
    }
    __vx_tcb()->specific[key] = (void *)value;
    return 0;
}

/* ---- Semaphores ---- */

int sem_init(sem_t *sem, int shared, unsigned value) {
    (void)shared;
    sem->value = value;
    sem->waiters = 0;
    return 0;
}

int sem_destroy(sem_t *sem) {
    (void)sem;
    return 0;
}

int sem_trywait(sem_t *sem) {
    unsigned v = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
    while (v > 0) {
        if (__atomic_compare_exchange_n(&sem->value, &v, v - 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_ACQUIRE)) {
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

static int sem_wait_for(sem_t *sem, long timeout_ms) {
    long deadline = timeout_ms >= 0 ? vx_uptime() + timeout_ms : 0;
    for (;;) {
        if (sem_trywait(sem) == 0) {
            return 0;
        }
        long wait = -1;
        if (timeout_ms >= 0) {
            wait = deadline - vx_uptime();
            if (wait <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        __atomic_add_fetch(&sem->waiters, 1, __ATOMIC_ACQ_REL);
        vx_wait_address(&sem->value, 0, wait);
        __atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

int sem_wait(sem_t *sem) {
    return sem_wait_for(sem, -1);
}

int sem_timedwait(sem_t *sem, const struct timespec *deadline) {
    return sem_wait_for(sem, ms_until(deadline));
}

int sem_post(sem_t *sem) {
    __atomic_add_fetch(&sem->value, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&sem->waiters, __ATOMIC_ACQUIRE)) {
        vx_wake_address(&sem->value, 1);
    }
    return 0;
}

int sem_getvalue(sem_t *sem, int *value) {
    *value = (int)sem->value;
    return 0;
}

int pthread_getcpuclockid(pthread_t thread, clockid_t *clock) {
    if (thread != pthread_self()) {
        return ENOENT;
    }
    *clock = CLOCK_THREAD_CPUTIME_ID;
    return 0;
}
