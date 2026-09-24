#include <vexa/arch.h>
#include <vexa/elf.h>
#include <vexa/fpu.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/process.h>
#include <vexa/random.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/vfs.h>

/* The stack: an 8 MiB area just below the top of user memory, filled in as
 * it's used. The page under it stays unmapped, so overflowing it faults. */
#define USER_STACK_TOP 0x00007ffffffff000ULL
#define USER_STACK_SIZE (8ULL * 1024 * 1024)
#define MAX_ARGUMENT_BYTES (128 * 1024)

/* Auxiliary vector entries (what the Linux ABI passes a program at startup;
 * native programs ignore them). */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

__attribute__((noreturn)) void enter_user_mode(uint64_t entry, uint64_t stack); /* syscall.c */

/* Protects the process list and parent links. Lock order: the scheduler
 * lock, then this. */
static struct spinlock process_lock = SPINLOCK_INIT;
static struct process *processes;
static uint32_t next_process_id = 1;

static void process_destroy(struct object *object) {
    struct process *process = (struct process *)object;
    if (process->address_space) {
        vm_destroy(process->address_space);
    }
    if (process->personality && process->personality->free_data && process->personality_data) {
        process->personality->free_data(process->personality_data);
    }
    kfree(process->cwd);
    kfree(process);
}

const struct object_type process_object_type = {
    .name = "process",
    .destroy = process_destroy,
};

static void user_thread_entry(void *arg) {
    struct process *process = arg;
    enter_user_mode(process->entry, process->stack_pointer);
}

/* ---- Loading a program ---- */

struct stack_builder {
    struct address_space *as;
    uint64_t top; /* Strings grow down from here. */
    bool failed;
};

static uint64_t push_bytes(struct stack_builder *b, const void *data, size_t size) {
    b->top -= size;
    if (!vm_write(b->as, b->top, data, size)) {
        b->failed = true;
    }
    return b->top;
}

static uint64_t push_string(struct stack_builder *b, const char *s) {
    return push_bytes(b, s, strlen(s) + 1);
}

/* Lays out the start of a program's stack the way the x86_64 System V ABI
 * says (both native and Linux programs start this way):
 *
 *   rsp -> argc, argv[0..argc-1], NULL, envp[...], NULL, auxv pairs, AT_NULL,
 *          then the strings they point to, near the top of the stack. */
static int build_stack(struct address_space *as, char *const *argv, size_t argc,
                       char *const *envp, size_t envc, const struct elf_image *image,
                       uint64_t *stack_pointer) {
    size_t total = 0;
    for (size_t i = 0; i < argc; i++) {
        total += strlen(argv[i]) + 1;
    }
    for (size_t i = 0; i < envc; i++) {
        total += strlen(envp[i]) + 1;
    }
    if (total > MAX_ARGUMENT_BYTES) {
        return -VX_E2BIG;
    }
    size_t words = 1 + argc + 1 + envc + 1 + 2 * 18;
    uint64_t *vector = kmalloc(words * sizeof(uint64_t));
    if (!vector) {
        return -VX_ENOMEM;
    }

    struct stack_builder b = {as, USER_STACK_TOP, false};
    uint8_t random[16];
    random_bytes(random, sizeof(random));
    uint64_t random_at = push_bytes(&b, random, sizeof(random));
    uint64_t platform = push_string(&b, "x86_64");
    uint64_t execfn = argc ? push_string(&b, argv[0]) : platform;

    size_t n = 0;
    vector[n++] = argc;
    for (size_t i = 0; i < argc; i++) {
        vector[n++] = push_string(&b, argv[i]);
    }
    vector[n++] = 0;
    for (size_t i = 0; i < envc; i++) {
        vector[n++] = push_string(&b, envp[i]);
    }
    vector[n++] = 0;
    uint64_t aux[][2] = {
        {AT_PHDR, image->phdr_address}, {AT_PHENT, image->phent}, {AT_PHNUM, image->phnum},
        {AT_PAGESZ, PAGE_SIZE}, {AT_BASE, 0}, {AT_FLAGS, 0}, {AT_ENTRY, image->entry},
        {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0}, {AT_PLATFORM, platform},
        {AT_HWCAP, 0}, {AT_CLKTCK, 100}, {AT_SECURE, 0}, {AT_RANDOM, random_at},
        {AT_EXECFN, execfn}, {AT_NULL, 0},
    };
    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); i++) {
        vector[n++] = aux[i][0];
        vector[n++] = aux[i][1];
    }

    uint64_t sp = (b.top - n * sizeof(uint64_t)) & ~15ULL; /* rsp is 16-byte aligned at argc. */
    bool ok = !b.failed && vm_write(as, sp, vector, n * sizeof(uint64_t));
    kfree(vector);
    if (!ok) {
        return -VX_ENOMEM;
    }
    *stack_pointer = sp;
    return 0;
}

int process_load(const char *path, char *const *argv, size_t argc, char *const *envp,
                 size_t envc, struct address_space **as_out, uint64_t *entry,
                 uint64_t *stack_pointer, const struct personality **personality,
                 const char **reason) {
    struct file *file;
    int error = vfs_open(path, strlen(path), VX_OPEN_READ, &file);
    if (error) {
        *reason = vfs_error_name(error);
        return error;
    }
    struct vx_stat stat;
    vfs_file_stat(file, &stat);
    if (stat.type != VX_TYPE_FILE) {
        vfs_close(file);
        *reason = stat.type == VX_TYPE_DIRECTORY ? "is a directory" : "not a file";
        return stat.type == VX_TYPE_DIRECTORY ? -VX_EISDIR : -VX_EACCES;
    }
    struct address_space *as = vm_create();
    if (!as) {
        vfs_close(file);
        *reason = "out of memory";
        return -VX_ENOMEM;
    }
    struct elf_image image;
    error = elf_load(as, file, &image, reason);
    vfs_close(file);
    if (!error) {
        as->heap_start = as->heap_end = image.heap_start;
        error = vm_add_area(as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VM_WRITE);
    }
    if (!error) {
        error = build_stack(as, argv, argc, envp, envc, &image, stack_pointer);
        if (error) {
            *reason = error == -VX_E2BIG ? "arguments too long" : "out of memory";
        }
    }
    if (error) {
        vm_destroy(as);
        return error;
    }
    *as_out = as;
    *entry = image.entry;
    *personality = image.personality;
    return 0;
}

/* ---- Starting ---- */

struct process *process_spawn(const struct spawn_request *request, int *error,
                              const char **reason) {
    *reason = "out of memory";
    struct process *process = kzalloc(sizeof(*process));
    char *cwd = process ? kmalloc(strlen(request->cwd ? request->cwd : "/") + 1) : NULL;
    if (!cwd) {
        kfree(process);
        *error = -VX_ENOMEM;
        return NULL;
    }
    object_init(&process->object, &process_object_type);
    strcpy(cwd, request->cwd ? request->cwd : "/");
    process->cwd = cwd;
    /* The process is named after the last part of its path. */
    const char *name = request->path;
    for (const char *p = request->path; *p; p++) {
        if (*p == '/' && p[1]) {
            name = p + 1;
        }
    }
    for (size_t i = 0; name[i] && name[i] != '/' && i < sizeof(process->name) - 1; i++) {
        process->name[i] = name[i];
    }

    *error = process_load(request->path, request->argv, request->argc, request->envp,
                          request->envc, &process->address_space, &process->entry,
                          &process->stack_pointer, &process->personality, reason);
    process->handles = *error ? NULL : handle_table_create();
    if (!*error && !process->handles) {
        *error = -VX_ENOMEM;
    }
    struct thread *thread = NULL;
    if (!*error) {
        thread = thread_create_stopped(process->name, user_thread_entry, process);
        void *fpu = thread ? fpu_alloc_state() : NULL;
        if (!fpu) {
            if (thread) {
                thread_destroy_unstarted(thread);
                thread = NULL;
            }
            *error = -VX_ENOMEM;
        } else {
            thread->process = process;
            thread->fpu_state = fpu;
        }
    }
    if (*error) {
        if (process->handles) {
            handle_table_destroy(process->handles);
        }
        object_put(&process->object);
        return NULL;
    }

    for (int i = 0; i < 3; i++) {
        if (request->handles[i]) {
            object_ref(request->handles[i]);
            handle_set(process->handles, i, request->handles[i], request->rights[i]);
        }
    }
    process->main_thread = thread;
    process->parent = request->parent;
    process->auto_reap = request->parent == NULL;

    uint64_t flags = spin_lock_irqsave(&process_lock);
    process->id = next_process_id++;
    process->group = request->join_group                         ? request->join_group
                     : request->new_group || !request->parent ? process->id
                                                              : request->parent->group;
    process->next = processes;
    processes = process;
    object_ref(&process->object); /* One for the list, one for the caller. */
    spin_unlock_irqrestore(&process_lock, flags);

    thread_start(thread);
    return process;
}

/* ---- Exiting and collecting ---- */

static bool has_exited(void *arg) {
    return ((struct process *)arg)->state == PROCESS_EXITED;
}

int process_wait_exit(struct process *process, bool interruptible) {
    if (interruptible) {
        return wait_queue_wait_interruptible(&process->exited, has_exited, process);
    }
    wait_queue_wait(&process->exited, has_exited, process);
    return 0;
}

/* Takes a process off the list. Call with process_lock held; the caller then
 * drops the list's reference (outside the lock is fine too). */
static bool unlink_locked(struct process *process) {
    if (process->reaped) {
        return false;
    }
    process->reaped = true;
    for (struct process **link = &processes; *link; link = &(*link)->next) {
        if (*link == process) {
            *link = process->next;
            break;
        }
    }
    process->parent = NULL;
    return true;
}

void process_reap(struct process *process) {
    uint64_t flags = spin_lock_irqsave(&process_lock);
    bool unlinked = unlink_locked(process);
    spin_unlock_irqrestore(&process_lock, flags);
    if (unlinked) {
        object_put(&process->object);
    }
}

struct child_wait {
    struct process *parent;
    uint32_t id;
    struct process *found;
    bool any;
};

static bool child_ready(void *arg) {
    struct child_wait *wait = arg;
    wait->any = false;
    for (struct process *p = processes; p; p = p->next) {
        if (p->parent == wait->parent && (!wait->id || p->id == wait->id)) {
            wait->any = true;
            if (p->state == PROCESS_EXITED) {
                wait->found = p;
                return true;
            }
        }
    }
    return !wait->any; /* No such children: stop waiting too. */
}

int process_wait_child(struct process *parent, uint32_t id, bool no_hang, int *exit_code,
                       int *exit_signal) {
    struct child_wait wait = {.parent = parent, .id = id};
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&process_lock);
        bool ready = child_ready(&wait);
        struct process *found = wait.found;
        bool unlinked = false;
        if (found) {
            *exit_code = found->exit_code;
            *exit_signal = found->exit_signal;
            unlinked = unlink_locked(found);
        }
        spin_unlock_irqrestore(&process_lock, flags);
        if (found) {
            uint32_t found_id = found->id;
            if (unlinked) {
                object_put(&found->object);
            }
            return (int)found_id;
        }
        if (ready) {
            return -VX_ECHILD;
        }
        if (no_hang) {
            return 0;
        }
        /* Children announce their exit on parent->children_changed. */
        int error = wait_queue_wait_interruptible(&parent->children_changed, child_ready, &wait);
        if (error) {
            return error;
        }
    }
}

struct process *process_current(void) {
    struct thread *thread = thread_current();
    return thread ? thread->process : NULL;
}

struct process *process_find(uint32_t id) {
    uint64_t flags = spin_lock_irqsave(&process_lock);
    struct process *found = NULL;
    for (struct process *p = processes; p; p = p->next) {
        if (p->id == id) {
            found = p;
            object_ref(&found->object);
            break;
        }
    }
    spin_unlock_irqrestore(&process_lock, flags);
    return found;
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
    /* Children lose their parent: exited ones are collected now, the others
     * will be when they exit. */
    struct process *orphans[16];
    int orphan_count;
    do {
        orphan_count = 0;
        uint64_t flags = spin_lock_irqsave(&process_lock);
        for (struct process *p = processes; p; p = p->next) {
            if (p->parent != process) {
                continue;
            }
            p->parent = NULL;
            p->auto_reap = true;
            if (p->state == PROCESS_EXITED && orphan_count < 16 && unlink_locked(p)) {
                orphans[orphan_count++] = p;
            }
        }
        spin_unlock_irqrestore(&process_lock, flags);
        for (int i = 0; i < orphan_count; i++) {
            object_put(&orphans[i]->object);
        }
    } while (orphan_count == 16);
    thread_exit();
}

void process_exit_by_signal(int signal) {
    process_current()->exit_signal = signal;
    process_exit(128 + signal);
}

void process_thread_reaped(struct thread *thread) {
    struct process *process = thread->process;
    if (thread != process->main_thread) {
        return;
    }
    /* The last (only) thread is gone, so no CPU can be using these tables.
     * (Its handles were closed by process_exit.) */
    vm_destroy(process->address_space);
    process->address_space = NULL;
    process->main_thread = NULL; /* The thread is about to be freed. */
    process->state = PROCESS_EXITED;
    wait_queue_wake_all_locked(&process->exited);

    spin_lock_irqsave(&process_lock); /* Interrupts are already off. */
    struct process *parent = process->parent;
    bool unlinked = false;
    if (parent) {
        wait_queue_wake_all_locked(&parent->children_changed);
        signal_send_locked(parent, VX_SIGCHLD);
    } else if (process->auto_reap) {
        unlinked = unlink_locked(process);
    }
    spin_unlock(&process_lock);
    if (unlinked) {
        object_put(&process->object);
    }
}

void process_for_each(void (*fn)(struct process *process, void *arg), void *arg) {
    uint64_t flags = spin_lock_irqsave(&process_lock);
    for (struct process *process = processes; process; process = process->next) {
        fn(process, arg);
    }
    spin_unlock_irqrestore(&process_lock, flags);
}

/* ---- Paths ---- */

char *process_absolute_path(struct process *process, const char *path, size_t length) {
    const char *base = process && process->cwd ? process->cwd : "/";
    size_t base_length = (length && path[0] == '/') ? 0 : strlen(base);
    char *joined = kmalloc(base_length + 1 + length + 1);
    char *result = kmalloc(base_length + 1 + length + 2);
    if (!joined || !result) {
        kfree(joined);
        kfree(result);
        return NULL;
    }
    memcpy(joined, base, base_length);
    joined[base_length] = '/';
    memcpy(joined + base_length + 1, path, length);
    size_t total = base_length + 1 + length;

    /* Rebuild component by component, dropping "." and resolving "..". */
    size_t out = 0;
    for (size_t i = 0; i < total;) {
        while (i < total && joined[i] == '/') {
            i++;
        }
        size_t start = i;
        while (i < total && joined[i] != '/') {
            i++;
        }
        size_t n = i - start;
        if (n == 0 || (n == 1 && joined[start] == '.')) {
            continue;
        }
        if (n == 2 && joined[start] == '.' && joined[start + 1] == '.') {
            while (out > 0 && result[out - 1] != '/') {
                out--;
            }
            if (out > 0) {
                out--; /* The slash before it. */
            }
            continue;
        }
        result[out++] = '/';
        memcpy(result + out, joined + start, n);
        out += n;
    }
    if (out == 0) {
        result[out++] = '/';
    }
    result[out] = '\0';
    kfree(joined);
    return result;
}
