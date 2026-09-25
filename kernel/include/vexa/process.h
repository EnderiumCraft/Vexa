#ifndef VEXA_PROCESS_H
#define VEXA_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>
#include <vexa/object.h>
#include <vexa/sched.h>
#include <vexa/signal.h>

struct address_space;
struct handle_table;
struct interrupt_frame;
struct process;

/* A personality turns a process's system calls into core kernel operations
 * (see docs/ARCHITECTURE.md). Each process has exactly one. */
struct personality {
    const char *name;
    void (*syscall)(struct interrupt_frame *frame);
    /* Sets up the frame so the process runs its handler for `signal` next.
     * Returns false if it couldn't (the process is then ended). */
    bool (*deliver_signal)(struct interrupt_frame *frame, int signal);
    /* Personality data follows a process through fork and is freed at exit. */
    void *(*fork_data)(void *data);
    void (*free_data)(void *data);
    /* Per-thread data (thread->personality_data), freed when the thread goes. */
    void (*free_thread_data)(void *data);
    /* Optional: where this personality's programs find an absolute path (the
     * Linux subsystem looks in /linux first). Returns a new string, or NULL
     * to use the path as it is. */
    char *(*translate_path)(const char *path);
};

extern const struct personality vexa_personality;  /* personality/vexa/ */
extern const struct personality linux_personality; /* personality/linux/ */

enum process_state {
    PROCESS_RUNNING,
    PROCESS_EXITED, /* A "zombie" until its parent (or the kernel) collects it. */
};

struct process {
    struct object object;    /* References: the process list, handles, callers. */
    uint32_t id;
    uint32_t group;          /* Process group: who a Ctrl+C goes to. */
    char name[32];
    enum process_state state;
    int exit_code;           /* 128 + signal number if a signal ended it. */
    int exit_signal;         /* The signal that ended it, or 0. */
    bool auto_reap;          /* No parent: collect it as soon as it exits. */
    bool reaped;
    struct process *parent;  /* NULL once the parent is gone. */
    const struct personality *personality;
    void *personality_data;
    struct address_space *address_space;
    struct handle_table *handles;
    /* Threads: the list and thread_count change under the scheduler lock,
     * when threads are added and when they are reaped. live_threads counts
     * those that haven't started exiting; the last one out closes the
     * handles, and the last one reaped ends the process. */
    struct thread *threads;
    uint32_t thread_count;
    uint32_t live_threads;
    bool exiting;            /* exit_group: every thread is on its way out. */
    struct wait_queue threads_changed;
    char *cwd;               /* Absolute, normalized. */
    uint32_t umask;          /* Permission bits new files don't get (kept across exec). */
    uint64_t pending_signals;
    uint8_t signal_actions[VX_SIGNAL_COUNT]; /* enum signal_action */
    char *exe;               /* The program's path, and its arguments NUL-separated. */
    char *cmdline;
    size_t cmdline_length;
    uint64_t start_ms;       /* timer_ms() when it started. */
    uint64_t entry;          /* Where user mode starts, and with what stack. */
    uint64_t stack_pointer;
    struct process_timer timers[PROCESS_TIMERS];
    struct wait_queue exited;           /* Anyone waiting for this process. */
    struct wait_queue children_changed; /* This process, waiting for a child. */
    struct process *next;
};

extern const struct object_type process_object_type;

struct spawn_request {
    const char *path;
    char *const *argv; /* Kernel strings. */
    size_t argc;
    char *const *envp;
    size_t envc;
    struct object *handles[3]; /* Become handles 0, 1, 2 (NULL: none). */
    uint32_t rights[3];
    struct process *parent;    /* NULL for processes the kernel starts. */
    bool new_group;
    uint32_t join_group;       /* Non-zero: join this process group. */
    const char *cwd;           /* NULL: "/" */
};

/* Starts a program in a new process. Returns it with a reference for the
 * caller (object_put when done), or NULL with *error set; *reason may get a
 * human-readable explanation. */
struct process *process_spawn(const struct spawn_request *request, int *error,
                              const char **reason);

/* Waits until a process has exited. Returns -VX_EINTR if a signal interrupts. */
int process_wait_exit(struct process *process, bool interruptible);
/* Collects an exited process: it leaves the process list. */
void process_reap(struct process *process);
/* Waits for a child of `parent` to exit (any child if id is 0), collects it,
 * and returns its id; or 0 with `no_hang` if none has exited yet;
 * -VX_ECHILD if there are no such children; -VX_EINTR. */
int process_wait_child(struct process *parent, uint32_t id, bool no_hang, int *exit_code,
                       int *exit_signal);

/* Loads the program at `path` into a fresh address space with its arguments
 * and environment on the stack, along with its dynamic loader if it has one.
 * A script starting with "#!" runs its interpreter instead. `caller` (may be
 * NULL) is the personality asking, which decides how a script's interpreter
 * path is found. Used by spawn and by exec. */
int process_load(const char *path, char *const *argv, size_t argc, char *const *envp,
                 size_t envc, const struct personality *caller, struct address_space **as_out,
                 uint64_t *entry, uint64_t *stack_pointer,
                 const struct personality **personality, const char **reason);

/* Linux fork: a copy of the calling process (memory shared copy-on-write,
 * handles, signal settings), whose thread resumes from `frame` with rax 0.
 * Returns it with a reference for the caller, or NULL with *error set. */
struct process *process_fork(struct interrupt_frame *frame, int *error);
/* Replaces the calling process's program, like Linux execve. On success
 * `frame` is set up to start the new program; on failure nothing changed. */
int process_exec(const char *path, char *const *argv, size_t argc, char *const *envp,
                 size_t envc, struct interrupt_frame *frame, const char **reason);

/* The process's address space with a reference (vm_put when done), or NULL
 * if it has exited. Safe for other processes' address spaces. */
struct address_space *process_address_space(struct process *process);

struct process *process_current(void);
/* Returns a reference to the process with this id, or NULL. */
struct process *process_find(uint32_t id);
/* Ends the whole process, all its threads (Linux exit_group, vx_exit). */
__attribute__((noreturn)) void process_exit(int code);
__attribute__((noreturn)) void process_exit_by_signal(int signal);
/* Ends the calling thread; if it was the last, the process too, with `code`. */
__attribute__((noreturn)) void process_thread_exit(int code);

/* Starts another thread in the calling process, resuming user mode from
 * `frame` with thread pointer `fs_base`. `copy_vector_registers`: start
 * with the caller's SSE/AVX state instead of a fresh one. The new thread's
 * id is stored (as 32 bits) at each nonzero address in `tid_out[2]` before it
 * starts (Linux's CLONE_PARENT_SETTID/CHILD_SETTID). Returns the new
 * thread's id or a negative VX_E* error. */
int process_thread_create(const struct interrupt_frame *frame, uint64_t fs_base,
                          bool copy_vector_registers, uint64_t clear_on_exit,
                          const uint64_t tid_out[2]);
/* The thread of the calling process with this id, for thread-directed
 * signals. Call with no locks held; the result is only a hint (it may exit). */
bool process_signal_thread(struct process *process, uint32_t tid, int signal);

/* Called by the scheduler, with its lock held, once a process thread is gone. */
void process_thread_reaped(struct thread *thread);

/* Calls `fn` for each process with the process list locked (no sleeping). */
void process_for_each(void (*fn)(struct process *process, void *arg), void *arg);

/* Paths from a process: makes `path` absolute (relative to the process's
 * directory) and normalizes "." and "..". Returns a new string or NULL. */
char *process_absolute_path(struct process *process, const char *path, size_t length);

#endif
