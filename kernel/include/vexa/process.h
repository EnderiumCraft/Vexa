#ifndef VEXA_PROCESS_H
#define VEXA_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/sched.h>

struct address_space;
struct interrupt_frame;

/* A personality turns a process's system calls into core kernel operations
 * (see docs/ARCHITECTURE.md). Each process has exactly one. */
struct personality {
    const char *name;
    void (*syscall)(struct interrupt_frame *frame);
};

extern const struct personality vexa_personality; /* personality/vexa/ */

enum process_state {
    PROCESS_RUNNING,
    PROCESS_EXITED,
};

struct process {
    uint32_t id;
    char name[32];
    enum process_state state;
    int exit_code;
    bool detached; /* Nobody will wait for it: free it as soon as it exits. */
    const struct personality *personality;
    struct address_space *address_space;
    struct thread *main_thread;
    uint64_t entry;      /* Where user mode starts. */
    uint64_t stack_top;
    struct wait_queue exited;
    struct process *next;
};

/* Loads an ELF executable into a new process and starts it. With `detached`,
 * the process cleans up after itself; otherwise call process_wait(). Returns
 * NULL and sets *error (a negative VX_E* value, or -1 with a message printed)
 * on failure. */
struct process *process_spawn(const char *name, const void *elf, size_t size, bool detached,
                              int *error);
/* Waits for a process started without `detached`, frees it, and returns its exit code. */
int process_wait(struct process *process);

struct process *process_current(void);
__attribute__((noreturn)) void process_exit(int code);
/* Ends the current process because of a fault in user mode. */
__attribute__((noreturn)) void process_kill_current(const char *reason);

/* Called by the scheduler, with its lock held, once a process thread is gone. */
void process_thread_reaped(struct thread *thread);

void process_for_each(void (*fn)(struct process *process, void *arg), void *arg);

#endif
