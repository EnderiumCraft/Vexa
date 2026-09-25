#ifndef VEXA_SCHED_H
#define VEXA_SCHED_H

#include <stdbool.h>
#include <stdint.h>

struct cpu;
struct process;

enum thread_state {
    THREAD_READY,    /* Waiting in the run queue. */
    THREAD_RUNNING,
    THREAD_SLEEPING, /* Until a time (thread_sleep_ms). */
    THREAD_BLOCKED,  /* On a wait queue. */
    THREAD_DEAD,     /* Exited; freed once no CPU is using its stack. */
};

struct thread {
    uint64_t rsp;          /* Saved stack pointer while switched out. */
    uint64_t stack_top;
    uint32_t id;
    enum thread_state state;
    char name[24];
    struct process *process; /* NULL for kernel threads. */
    void *fpu_state;         /* Saved vector registers; user threads only. */
    uint64_t wake_at;        /* For THREAD_SLEEPING, in timer_ms() time. */
    uint64_t cpu_ms;         /* Time spent running. */
    uint32_t cpu;            /* CPU it last ran on. */
    struct thread *next;     /* Run queue, sleep list or wait queue link. */
    struct thread *all_next; /* List of every thread. */
    struct wait_queue *waiting_on; /* While THREAD_BLOCKED. */
    struct thread *sleep_next;     /* Sleepers list: sleeping, or a timed wait. */
    bool timed;                    /* A timed wait: on a wait queue and the sleepers list. */
    bool interruptible;            /* A signal may end the wait early. */
    uint64_t blocked_signals;      /* Signals this thread doesn't take yet. */
    uint64_t fs_base;              /* User thread-local storage pointer. */

    /* User threads (thread->process set): */
    uint32_t tid;                  /* The first thread's is the process id. */
    struct thread *process_next;   /* The process's thread list (under the scheduler lock). */
    uint64_t pending_signals;      /* Sent to this thread only (the rest are the process's). */
    uint64_t clear_on_exit;        /* User address: set to 0 and woken when the thread exits. */
    bool killed;                   /* Another thread is replacing the program (exec). */
    void *personality_data;        /* The personality's per-thread state. */
};

/* Threads waiting for something. Wake-ups can't be lost: wait_queue_wait()
 * rechecks its condition under the scheduler lock, which wake-ups also take. */
struct wait_queue {
    struct thread *head;
    struct thread *tail;
};

#define WAIT_QUEUE_INIT {0, 0}

/* Turns the code running now into this CPU's idle thread and starts scheduling. */
void sched_init_cpu(struct cpu *cpu);
bool sched_running(void);

/* Creates a kernel thread and makes it runnable. */
struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);
/* Creates a thread without starting it, so the caller can finish setting it
 * up (e.g. attach it to a process) before calling thread_start(). */
struct thread *thread_create_stopped(const char *name, void (*entry)(void *), void *arg);
void thread_start(struct thread *thread);
/* Frees a thread from thread_create_stopped() that was never started. */
void thread_destroy_unstarted(struct thread *thread);

struct thread *thread_current(void);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
__attribute__((noreturn)) void thread_exit(void);

void wait_queue_wait(struct wait_queue *queue, bool (*ready)(void *), void *arg);
/* Like wait_queue_wait, but gives up with -VX_EINTR if a signal arrives for
 * the thread. Use it for waits that could last indefinitely. */
int wait_queue_wait_interruptible(struct wait_queue *queue, bool (*ready)(void *), void *arg);
/* Like wait_queue_wait(_interruptible), giving up with -VX_ETIMEDOUT after
 * timeout_ms milliseconds. */
int wait_queue_wait_timeout(struct wait_queue *queue, bool (*ready)(void *), void *arg,
                            uint64_t timeout_ms, bool interruptible);
/* Sleeps; returns -VX_EINTR if a signal cuts it short. */
int thread_sleep_ms_interruptible(uint64_t ms);
/* Wakes a thread from an interruptible wait (a signal arrived for it). */
void sched_interrupt(struct thread *thread);
void sched_interrupt_locked(struct thread *thread); /* With the scheduler lock held. */
/* After a signal for the whole process: interrupts one of its threads that
 * can take it (for a thread-directed signal, use sched_interrupt). */
void sched_interrupt_process(struct process *process, int signal);
void sched_interrupt_process_locked(struct process *process, int signal);
/* Interrupts every thread of the process (it's exiting). */
void sched_interrupt_all(struct process *process);

/* ---- A process's threads (see process.c) ---- */

/* Adds a thread to the process; false if the process is already exiting. */
bool sched_attach_thread(struct process *process, struct thread *thread);
/* Asks every thread of the process except `keep` to exit (for exec), and
 * waits until they are all gone. */
void sched_kill_other_threads(struct process *process, struct thread *keep);
/* Calls `fn` for each thread of the process, with the scheduler lock held. */
void sched_for_each_process_thread(struct process *process,
                                   void (*fn)(struct thread *thread, void *arg), void *arg);
void wait_queue_wake_all(struct wait_queue *queue);
/* The same, for callers that already hold the scheduler lock. */
void wait_queue_wake_all_locked(struct wait_queue *queue);

/* Called on every CPU's timer tick, and at the end of interrupt handling. */
void sched_tick(void);
void sched_preempt_if_needed(void);

/* Calls `fn` for every thread, with the scheduler lock held. */
void sched_for_each_thread(void (*fn)(struct thread *thread, void *arg), void *arg);
const char *thread_state_name(enum thread_state state);

#endif
