#include <stddef.h>
#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/fpu.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/abi.h>
#include <vexa/process.h>
#include <vexa/signal.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

/*
 * Preemptive round-robin scheduler.
 *
 * All CPUs share one run queue, protected by sched_lock. A thread runs until
 * it blocks, sleeps, yields, or uses up its time slice, at which point the
 * timer interrupt switches to the next ready thread. Each CPU has an idle
 * thread (the code that booted it) for when nothing else is ready.
 *
 * sched_lock is held across a context switch: the thread that switches away
 * takes it, and the thread that resumes releases it. That keeps other CPUs
 * from picking up a thread while its stack is still in use.
 */

#define TIME_SLICE_MS 10
#define KERNEL_STACK_SIZE (64 * 1024)
#define STACK_CACHE_SIZE 64

void switch_context(uint64_t *save_rsp, uint64_t new_rsp); /* switch.S */
void thread_trampoline(void);                              /* switch.S */
void arch_prepare_switch(struct cpu *cpu, struct thread *prev, struct thread *next); /* cpu.c */

static struct spinlock sched_lock = SPINLOCK_INIT;
static struct thread *run_head, *run_tail;
static struct thread *sleepers;
static struct thread *all_threads;
static uint32_t next_thread_id;
static volatile bool running;

/* Freed kernel stacks, reused before allocating new ones. Stacks keep their
 * memory while cached; unmapping would need other CPUs to flush their TLBs. */
static struct spinlock stack_cache_lock = SPINLOCK_INIT;
static uint64_t stack_cache[STACK_CACHE_SIZE];
static int stack_cache_count;

static uint64_t stack_get(void) {
    uint64_t flags = spin_lock_irqsave(&stack_cache_lock);
    uint64_t top = stack_cache_count ? stack_cache[--stack_cache_count] : 0;
    spin_unlock_irqrestore(&stack_cache_lock, flags);
    return top ? top : vmm_alloc_kernel_stack(KERNEL_STACK_SIZE);
}

static bool stack_put(uint64_t top) {
    uint64_t flags = spin_lock_irqsave(&stack_cache_lock);
    bool cached = stack_cache_count < STACK_CACHE_SIZE;
    if (cached) {
        stack_cache[stack_cache_count++] = top;
    }
    spin_unlock_irqrestore(&stack_cache_lock, flags);
    return cached;
}

static void run_queue_push(struct thread *thread) {
    thread->next = NULL;
    if (run_tail) {
        run_tail->next = thread;
    } else {
        run_head = thread;
    }
    run_tail = thread;
}

static struct thread *run_queue_pop(void) {
    struct thread *thread = run_head;
    if (thread) {
        run_head = thread->next;
        if (!run_head) {
            run_tail = NULL;
        }
        thread->next = NULL;
    }
    return thread;
}

static void make_ready(struct thread *thread) {
    thread->state = THREAD_READY;
    run_queue_push(thread);
}

/* Frees a dead thread. Runs on another thread's stack, with sched_lock held. */
static void reap(struct thread *thread) {
    for (struct thread **link = &all_threads; *link; link = &(*link)->all_next) {
        if (*link == thread) {
            *link = thread->all_next;
            break;
        }
    }
    if (thread->process) {
        process_thread_reaped(thread);
    }
    fpu_free_state(thread->fpu_state);
    if (!stack_put(thread->stack_top)) {
        kprintf("[sched] warning: stack cache full, leaking a kernel stack\n");
    }
    kfree(thread);
}

/* Runs right after every context switch, in the thread that resumed. */
static void finish_switch(void) {
    struct cpu *cpu = cpu_current();
    struct thread *prev = cpu->prev;
    cpu->prev = NULL;
    if (prev && prev->state == THREAD_DEAD) {
        reap(prev);
    }
}

/* Switches to the next ready thread. Call with sched_lock held; the current
 * thread must already be queued, sleeping, blocked or dead unless it should
 * keep running when nothing else is ready. */
static void schedule(void) {
    struct cpu *cpu = cpu_current();
    struct thread *prev = cpu->current;
    struct thread *next = run_queue_pop();
    if (!next) {
        if (prev->state == THREAD_RUNNING) {
            return;
        }
        next = cpu->idle;
    }
    cpu->slice_left = TIME_SLICE_MS;
    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }
    if (prev == cpu->idle) {
        prev->state = THREAD_READY; /* Idle threads never sit in the run queue. */
    }
    next->state = THREAD_RUNNING;
    next->cpu = cpu->id;
    cpu->current = next;
    cpu->prev = prev;
    arch_prepare_switch(cpu, prev, next);
    switch_context(&prev->rsp, next->rsp);
    finish_switch();
}

/* C side of thread_trampoline: the first thing every new thread runs. */
__attribute__((noreturn)) void thread_bootstrap(void (*entry)(void *), void *arg) {
    finish_switch();
    spin_unlock(&sched_lock);
    interrupts_enable();
    entry(arg);
    thread_exit();
}

static struct thread *thread_alloc(const char *name) {
    struct thread *thread = kzalloc(sizeof(*thread));
    if (!thread) {
        return NULL;
    }
    size_t i = 0;
    for (; name[i] && i < sizeof(thread->name) - 1; i++) {
        thread->name[i] = name[i];
    }
    thread->name[i] = '\0';
    return thread;
}

static void register_thread(struct thread *thread) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    thread->id = next_thread_id++;
    thread->all_next = all_threads;
    all_threads = thread;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_init_cpu(struct cpu *cpu) {
    char name[8] = "idle";
    if (cpu->id >= 10) {
        name[4] = (char)('0' + cpu->id / 10);
        name[5] = (char)('0' + cpu->id % 10);
    } else {
        name[4] = (char)('0' + cpu->id);
    }
    struct thread *idle = thread_alloc(name);
    if (!idle) {
        panic("sched: out of memory");
    }
    idle->state = THREAD_RUNNING;
    idle->cpu = cpu->id;
    register_thread(idle);
    cpu->idle = idle;
    cpu->current = idle;
    cpu->slice_left = TIME_SLICE_MS;
    running = true;
}

bool sched_running(void) {
    return running;
}

struct thread *thread_create_stopped(const char *name, void (*entry)(void *), void *arg) {
    struct thread *thread = thread_alloc(name);
    if (!thread) {
        return NULL;
    }
    thread->stack_top = stack_get();
    /* Build the stack switch_context() expects: six saved registers, then the
     * return address. r12/r13 carry the entry point and argument. The layout
     * leaves the stack 16-byte aligned for thread_bootstrap's call. */
    uint64_t *sp = (uint64_t *)thread->stack_top;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0;               /* rbp */
    *--sp = 0;               /* rbx */
    *--sp = (uint64_t)entry; /* r12 */
    *--sp = (uint64_t)arg;   /* r13 */
    *--sp = 0;               /* r14 */
    *--sp = 0;               /* r15 */
    thread->rsp = (uint64_t)sp;
    thread->state = THREAD_BLOCKED;
    register_thread(thread);
    return thread;
}

void thread_start(struct thread *thread) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    make_ready(thread);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void thread_destroy_unstarted(struct thread *thread) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    thread->state = THREAD_DEAD;
    reap(thread);
    spin_unlock_irqrestore(&sched_lock, flags);
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg) {
    struct thread *thread = thread_create_stopped(name, entry, arg);
    if (thread) {
        thread_start(thread);
    }
    return thread;
}

struct thread *thread_current(void) {
    return running ? cpu_current()->current : NULL;
}

void thread_yield(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct cpu *cpu = cpu_current();
    struct thread *thread = cpu->current;
    if (thread != cpu->idle && thread->state == THREAD_RUNNING) {
        make_ready(thread);
    }
    schedule();
    spin_unlock_irqrestore(&sched_lock, flags);
}

static int sleep_common(uint64_t ms, bool interruptible) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct thread *thread = cpu_current()->current;
    if (interruptible && thread_signal_pending(thread)) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return -VX_EINTR;
    }
    thread->wake_at = timer_ms() + ms;
    thread->state = THREAD_SLEEPING;
    thread->interruptible = interruptible;
    thread->sleep_next = sleepers;
    sleepers = thread;
    schedule();
    thread->interruptible = false;
    bool early = timer_ms() < thread->wake_at;
    spin_unlock_irqrestore(&sched_lock, flags);
    return early ? -VX_EINTR : 0;
}

void thread_sleep_ms(uint64_t ms) {
    sleep_common(ms, false);
}

int thread_sleep_ms_interruptible(uint64_t ms) {
    return sleep_common(ms, true);
}

void thread_exit(void) {
    spin_lock_irqsave(&sched_lock);
    cpu_current()->current->state = THREAD_DEAD;
    schedule();
    panic("thread_exit: dead thread was scheduled again");
}

/* A timed wait's thread is on its wait queue and on the sleepers list (for
 * the deadline); whichever wakes it first takes it off the other. */
static void remove_sleeper(struct thread *thread) {
    for (struct thread **link = &sleepers; *link; link = &(*link)->sleep_next) {
        if (*link == thread) {
            *link = thread->sleep_next;
            break;
        }
    }
    thread->timed = false;
}

static void remove_from_queue(struct wait_queue *queue, struct thread *thread) {
    struct thread *previous = NULL;
    for (struct thread *t = queue->head; t; previous = t, t = t->next) {
        if (t == thread) {
            if (previous) {
                previous->next = t->next;
            } else {
                queue->head = t->next;
            }
            if (queue->tail == t) {
                queue->tail = previous;
            }
            break;
        }
    }
    thread->waiting_on = NULL;
}

#define NO_DEADLINE 0

static int wait_common(struct wait_queue *queue, bool (*ready)(void *), void *arg,
                       bool interruptible, uint64_t deadline) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sched_lock);
        if (ready(arg)) {
            spin_unlock_irqrestore(&sched_lock, flags);
            return 0;
        }
        struct thread *thread = cpu_current()->current;
        if (interruptible && thread_signal_pending(thread)) {
            spin_unlock_irqrestore(&sched_lock, flags);
            return -VX_EINTR;
        }
        if (deadline != NO_DEADLINE && timer_ms() >= deadline) {
            spin_unlock_irqrestore(&sched_lock, flags);
            return -VX_ETIMEDOUT;
        }
        thread->state = THREAD_BLOCKED;
        thread->waiting_on = queue;
        thread->interruptible = interruptible;
        thread->next = NULL;
        if (queue->tail) {
            queue->tail->next = thread;
        } else {
            queue->head = thread;
        }
        queue->tail = thread;
        if (deadline != NO_DEADLINE) {
            thread->timed = true;
            thread->wake_at = deadline;
            thread->sleep_next = sleepers;
            sleepers = thread;
        }
        schedule();
        thread->interruptible = false;
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}

void wait_queue_wait(struct wait_queue *queue, bool (*ready)(void *), void *arg) {
    wait_common(queue, ready, arg, false, NO_DEADLINE);
}

int wait_queue_wait_interruptible(struct wait_queue *queue, bool (*ready)(void *), void *arg) {
    return wait_common(queue, ready, arg, true, NO_DEADLINE);
}

int wait_queue_wait_timeout(struct wait_queue *queue, bool (*ready)(void *), void *arg,
                            uint64_t timeout_ms, bool interruptible) {
    uint64_t deadline = timer_ms() + timeout_ms;
    return wait_common(queue, ready, arg, interruptible, deadline ? deadline : 1);
}

void wait_queue_wake_all_locked(struct wait_queue *queue) {
    struct thread *thread = queue->head;
    queue->head = queue->tail = NULL;
    while (thread) {
        struct thread *next = thread->next;
        thread->waiting_on = NULL;
        if (thread->timed) {
            remove_sleeper(thread);
        }
        make_ready(thread);
        thread = next;
    }
}

void sched_interrupt_locked(struct thread *thread) {
    if (thread->interruptible && thread->state == THREAD_BLOCKED && thread->waiting_on) {
        remove_from_queue(thread->waiting_on, thread);
        if (thread->timed) {
            remove_sleeper(thread);
        }
        make_ready(thread);
    } else if (thread->interruptible && thread->state == THREAD_SLEEPING) {
        remove_sleeper(thread);
        make_ready(thread);
    }
}

void sched_interrupt(struct thread *thread) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    sched_interrupt_locked(thread);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_interrupt_process_locked(struct process *process, int signal) {
    /* The first thread that doesn't block the signal takes it. (Threads are
     * unlinked, under this lock, when they are reaped.) */
    uint64_t bit = 1ULL << (signal - 1);
    for (struct thread *thread = process->threads; thread; thread = thread->process_next) {
        if (!(thread->blocked_signals & bit) || signal == VX_SIGKILL) {
            sched_interrupt_locked(thread);
            return;
        }
    }
}

void sched_interrupt_process(struct process *process, int signal) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    sched_interrupt_process_locked(process, signal);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_interrupt_all(struct process *process) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (struct thread *thread = process->threads; thread; thread = thread->process_next) {
        sched_interrupt_locked(thread);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

bool sched_attach_thread(struct process *process, struct thread *thread) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    bool ok = !process->exiting;
    if (ok) {
        thread->process = process;
        thread->process_next = process->threads;
        process->threads = thread;
        process->thread_count++;
        __atomic_add_fetch(&process->live_threads, 1, __ATOMIC_SEQ_CST);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return ok;
}

static bool only_one_thread(void *arg) {
    return ((struct process *)arg)->thread_count <= 1;
}

void sched_kill_other_threads(struct process *process, struct thread *keep) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (struct thread *thread = process->threads; thread; thread = thread->process_next) {
        if (thread != keep) {
            thread->killed = true;
            sched_interrupt_locked(thread);
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    /* Until they're reaped: none of them may still have the page tables loaded. */
    wait_queue_wait(&process->threads_changed, only_one_thread, process);
}

void sched_for_each_process_thread(struct process *process,
                                   void (*fn)(struct thread *thread, void *arg), void *arg) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (struct thread *thread = process->threads; thread; thread = thread->process_next) {
        fn(thread, arg);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void wait_queue_wake_all(struct wait_queue *queue) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    wait_queue_wake_all_locked(queue);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_tick(void) {
    if (!running) {
        return;
    }
    struct cpu *cpu = cpu_current();
    struct thread *thread = cpu->current;
    if (!thread) {
        return; /* This CPU is still starting up. */
    }
    thread->cpu_ms++;
    if (thread == cpu->idle) {
        cpu->idle_ticks++;
    } else {
        cpu->busy_ticks++;
    }

    if (cpu->id == 0 && sleepers) {
        uint64_t flags = spin_lock_irqsave(&sched_lock);
        uint64_t now = timer_ms();
        for (struct thread **link = &sleepers; *link;) {
            struct thread *sleeper = *link;
            if (sleeper->wake_at <= now) {
                *link = sleeper->sleep_next;
                if (sleeper->state == THREAD_BLOCKED) {
                    /* A timed wait ran out: it leaves its wait queue too. */
                    remove_from_queue(sleeper->waiting_on, sleeper);
                    sleeper->timed = false;
                }
                make_ready(sleeper);
            } else {
                link = &sleeper->sleep_next;
            }
        }
        spin_unlock_irqrestore(&sched_lock, flags);
    }

    if (thread == cpu->idle) {
        if (run_head) { /* Unlocked peek: at worst we check again next tick. */
            cpu->need_resched = true;
        }
    } else if (cpu->slice_left == 0 || --cpu->slice_left == 0) {
        cpu->need_resched = true;
    }
}

void sched_preempt_if_needed(void) {
    if (!running) {
        return;
    }
    struct cpu *cpu = cpu_current();
    if (cpu->need_resched && cpu->current) {
        cpu->need_resched = false;
        thread_yield();
    }
}

void sched_for_each_thread(void (*fn)(struct thread *thread, void *arg), void *arg) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (struct thread *thread = all_threads; thread; thread = thread->all_next) {
        fn(thread, arg);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

const char *thread_state_name(enum thread_state state) {
    switch (state) {
    case THREAD_READY: return "ready";
    case THREAD_RUNNING: return "running";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_BLOCKED: return "waiting";
    case THREAD_DEAD: return "dead";
    }
    return "?";
}
