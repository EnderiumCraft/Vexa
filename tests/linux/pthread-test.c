/* pthread-test: a Linux program (built with musl) that checks threads on
 * Vexa's Linux subsystem: mutexes, condition variables, thread-local
 * variables, joining, and a signal sent to one thread. With "exit", it
 * leaves threads running and exits: the whole process must end. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS 50000
#define PINGS 2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static __thread long mine = -1; /* Thread-local: each thread its own. */
static int failures;

static void fail(const char *what) {
    printf("pthread-test: FAILED: %s\n", what);
    __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);
}

static void *count(void *arg) {
    long number = (long)arg;
    mine = number;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    if (mine != number) {
        fail("a thread-local variable changed under another thread");
    }
    return (void *)(number + 100);
}

/* Two threads take turns through a condition variable. */
static pthread_mutex_t turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_changed = PTHREAD_COND_INITIALIZER;
static int turn;

static void *ping(void *arg) {
    int me = (int)(long)arg;
    for (int i = 0; i < PINGS; i++) {
        pthread_mutex_lock(&turn_lock);
        while (turn != me) {
            pthread_cond_wait(&turn_changed, &turn_lock);
        }
        turn = 1 - me;
        pthread_cond_broadcast(&turn_changed);
        pthread_mutex_unlock(&turn_lock);
    }
    return NULL;
}

/* pthread_kill: the handler must run in the thread it was sent to. */
static __thread volatile int got_signal;
static volatile int receiver_ready;

static void on_usr1(int signal) {
    (void)signal;
    got_signal = 1;
}

static void *receive(void *arg) {
    (void)arg;
    receiver_ready = 1;
    for (int i = 0; i < 2000 && !got_signal; i++) {
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    return (void *)(long)got_signal;
}

static void *spin(void *arg) {
    volatile long *spins = arg;
    for (;;) {
        (*spins)++;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "exit") == 0) {
        static long spins;
        pthread_t t;
        pthread_create(&t, NULL, spin, &spins);
        pthread_create(&t, NULL, spin, &spins);
        sleep(1);
        printf("pthread-test: exiting while threads spin\n");
        return 0;
    }

    pthread_t threads[THREADS];
    for (long i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, count, (void *)i)) {
            fail("pthread_create");
            return 1;
        }
    }
    for (long i = 0; i < THREADS; i++) {
        void *result;
        pthread_join(threads[i], &result);
        if ((long)result != i + 100) {
            fail("pthread_join returned the wrong value");
        }
    }
    if (counter != (long)THREADS * ROUNDS) {
        fail("the counter is wrong: the mutex let two threads in");
    }
    if (mine != -1) {
        fail("the main thread's thread-local variable changed");
    }

    pthread_t a, b;
    pthread_create(&a, NULL, ping, (void *)0);
    pthread_create(&b, NULL, ping, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    signal(SIGUSR1, on_usr1);
    pthread_t receiver;
    pthread_create(&receiver, NULL, receive, NULL);
    while (!receiver_ready) {
        sched_yield();
    }
    pthread_kill(receiver, SIGUSR1);
    void *received;
    pthread_join(receiver, &received);
    if (!received) {
        fail("the signal didn't reach the thread it was sent to");
    }
    if (got_signal) {
        fail("the signal ran in the main thread instead");
    }

    if (!failures) {
        printf("pthread-test: passed (%d threads, %ld increments, %d condition variable "
               "turns, a signal reached its thread)\n",
               THREADS, counter, 2 * PINGS);
    }
    return failures ? 1 : 0;
}
