/* thread-test: checks threads. Four threads add to one counter under a
 * mutex, and to another atomically; the main thread joins them and checks
 * the sums, then checks that a wait on an address times out. With "exit",
 * it leaves threads running (sleeping and spinning) and exits: the process
 * must end anyway. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>

#define THREADS 4
#define ROUNDS 100000

static struct vx_mutex lock = VX_MUTEX_INIT;
static unsigned long counter;
static unsigned long atomic_counter;

static void *work(void *arg) {
    long number = (long)arg;
    for (int i = 0; i < ROUNDS; i++) {
        vx_mutex_lock(&lock);
        counter++;
        vx_mutex_unlock(&lock);
        __atomic_add_fetch(&atomic_counter, 1, __ATOMIC_RELAXED);
        if (i % 25000 == 0) {
            vx_yield();
        }
    }
    printf("thread-test: thread %ld (id %ld) done\n", number, vx_thread_id());
    return (void *)(number * 10);
}

static void *sleep_forever(void *arg) {
    (void)arg;
    while (vx_sleep(100000) == 0) {
    }
    return NULL; /* Only if a sleep is interrupted. */
}

static void *spin_forever(void *arg) {
    volatile unsigned long *spins = arg;
    while (*spins != ~0UL) {
        (*spins)++;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "exit") == 0) {
        static unsigned long spins;
        vx_thread_create(sleep_forever, NULL);
        vx_thread_create(spin_forever, (void *)&spins);
        vx_sleep(100);
        printf("thread-test: exiting with threads still running\n");
        return 0; /* vx_exit ends every thread. */
    }

    struct vx_thread *threads[THREADS];
    for (long i = 0; i < THREADS; i++) {
        threads[i] = vx_thread_create(work, (void *)i);
        if (!threads[i]) {
            printf("thread-test: FAILED to start thread %ld\n", i);
            return 1;
        }
    }
    int status = 0;
    for (long i = 0; i < THREADS; i++) {
        long result = (long)vx_thread_join(threads[i]);
        if (result != i * 10) {
            printf("thread-test: FAILED: thread %ld returned %ld\n", i, result);
            status = 1;
        }
    }
    if (counter != THREADS * ROUNDS || atomic_counter != THREADS * ROUNDS) {
        printf("thread-test: FAILED: counters %lu and %lu, expected %d\n", counter,
               atomic_counter, THREADS * ROUNDS);
        status = 1;
    }
    volatile unsigned word = 7;
    long start = vx_uptime();
    long waited = vx_wait_address(&word, 7, 50);
    long took = vx_uptime() - start;
    if (waited != -VX_ETIMEDOUT || took < 40) {
        printf("thread-test: FAILED: timed wait returned %ld after %ld ms\n", waited, took);
        status = 1;
    }
    if (vx_wait_address(&word, 8, -1) != -VX_EAGAIN) {
        printf("thread-test: FAILED: wait on a changed value didn't return at once\n");
        status = 1;
    }
    if (!status) {
        printf("thread-test: passed (%d threads, %lu increments, timeouts work)\n", THREADS,
               counter);
    }
    return status;
}
