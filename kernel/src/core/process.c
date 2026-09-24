#include <vexa/arch.h>
#include <vexa/elf.h>
#include <vexa/fpu.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/process.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/* Every process gets a 256 KiB stack just below the top of user memory. The
 * page under it stays unmapped, so overflowing it faults. */
#define USER_STACK_TOP 0x00007ffffffff000ULL
#define USER_STACK_SIZE (256 * 1024)

__attribute__((noreturn)) void enter_user_mode(uint64_t entry, uint64_t stack); /* syscall.c */

/* Protects the process list. Lock order: the scheduler lock, then this. */
static struct spinlock process_lock = SPINLOCK_INIT;
static struct process *processes;
static uint32_t next_process_id = 1;

static void user_thread_entry(void *arg) {
    struct process *process = arg;
    enter_user_mode(process->entry, process->stack_top);
}

static void unlink_process(struct process *process) {
    uint64_t flags = spin_lock_irqsave(&process_lock);
    for (struct process **link = &processes; *link; link = &(*link)->next) {
        if (*link == process) {
            *link = process->next;
            break;
        }
    }
    spin_unlock_irqrestore(&process_lock, flags);
}

static void free_process(struct process *process) {
    if (process->address_space) {
        vmm_destroy_address_space(process->address_space);
    }
    if (process->handles) {
        handle_table_destroy(process->handles);
    }
    kfree(process);
}

struct process *process_spawn(const char *path, bool detached) {
    /* The process is named after the last part of its path. */
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1]) {
            name = p + 1;
        }
    }
    struct file *file;
    int open_error = vfs_open(path, strlen(path), VX_OPEN_READ, &file);
    if (open_error) {
        kprintf("[proc] cannot start %s: %s\n", path, vfs_error_name(open_error));
        return NULL;
    }
    struct vx_stat stat;
    vfs_file_stat(file, &stat);
    if (stat.type != VX_TYPE_FILE) {
        kprintf("[proc] cannot start %s: not a file\n", path);
        vfs_close(file);
        return NULL;
    }

    struct process *process = kzalloc(sizeof(*process));
    if (!process) {
        kprintf("[proc] cannot start %s: out of memory\n", path);
        vfs_close(file);
        return NULL;
    }
    size_t i = 0;
    for (; name[i] && name[i] != '/' && i < sizeof(process->name) - 1; i++) {
        process->name[i] = name[i];
    }
    process->detached = detached;
    process->stack_top = USER_STACK_TOP;

    const char *reason = "out of memory";
    process->address_space = vmm_create_address_space();
    process->handles = handle_table_create();
    bool ok = process->address_space && process->handles &&
              elf_load(process->address_space, file, &process->entry, &process->personality,
                       &reason) == 0;
    vfs_close(file);
    for (uint64_t page = USER_STACK_TOP - USER_STACK_SIZE; ok && page < USER_STACK_TOP;
         page += PAGE_SIZE) {
        ok = vmm_map_user_page(process->address_space, page, VMM_WRITE) != 0;
    }
    struct thread *thread = ok ? thread_create_stopped(name, user_thread_entry, process) : NULL;
    if (thread) {
        thread->process = process;
        thread->fpu_state = fpu_alloc_state();
        ok = thread->fpu_state != NULL;
    }
    if (!ok || !thread) {
        kprintf("[proc] cannot start %s: %s\n", path, reason);
        if (thread) {
            handle_table_destroy(process->handles); /* Can't be done by the reaper. */
            process->handles = NULL;
            /* Never started: let the scheduler's reaper free it and the process. */
            process->detached = true;
            process->main_thread = thread;
            thread_destroy_unstarted(thread);
        } else {
            free_process(process);
        }
        return NULL;
    }

    process->main_thread = thread;
    uint64_t flags = spin_lock_irqsave(&process_lock);
    process->id = next_process_id++;
    process->next = processes;
    processes = process;
    spin_unlock_irqrestore(&process_lock, flags);

    thread_start(thread);
    return process;
}

static bool has_exited(void *arg) {
    return ((struct process *)arg)->state == PROCESS_EXITED;
}

int process_wait(struct process *process) {
    wait_queue_wait(&process->exited, has_exited, process);
    int code = process->exit_code;
    unlink_process(process);
    kfree(process); /* The address space went with its last thread. */
    return code;
}

struct process *process_current(void) {
    struct thread *thread = thread_current();
    return thread ? thread->process : NULL;
}

void process_exit(int code) {
    struct process *process = process_current();
    process->exit_code = code;
    /* Close handles here, in the thread, since that may wait for the disk. */
    struct handle_table *handles = process->handles;
    process->handles = NULL;
    if (handles) {
        handle_table_destroy(handles);
    }
    thread_exit();
}

void process_kill_current(const char *reason) {
    struct process *process = process_current();
    kprintf("[proc] %s (process %u) killed: %s\n", process->name, process->id, reason);
    process_exit(-1);
}

void process_thread_reaped(struct thread *thread) {
    struct process *process = thread->process;
    if (thread != process->main_thread) {
        return;
    }
    /* The last (only) thread is gone, so no CPU can be using these tables.
     * (Its handles were closed by process_exit.) */
    vmm_destroy_address_space(process->address_space);
    process->address_space = NULL;
    process->state = PROCESS_EXITED;
    if (process->detached) {
        unlink_process(process);
        kfree(process);
    } else {
        wait_queue_wake_all_locked(&process->exited);
    }
}

void process_for_each(void (*fn)(struct process *process, void *arg), void *arg) {
    uint64_t flags = spin_lock_irqsave(&process_lock);
    for (struct process *process = processes; process; process = process->next) {
        fn(process, arg);
    }
    spin_unlock_irqrestore(&process_lock, flags);
}
