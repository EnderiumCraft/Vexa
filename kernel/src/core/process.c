#include <vexa/arch.h>
#include <vexa/cpu.h>
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
__attribute__((noreturn)) void return_to_user(struct interrupt_frame *f);      /* syscall.S */

/* Protects the process list and parent links. Lock order: the scheduler
 * lock, then this. */
static struct spinlock process_lock = SPINLOCK_INIT;
static struct process *processes;
static uint32_t next_process_id = 1;

static void process_destroy(struct object *object) {
    struct process *process = (struct process *)object;
    if (process->address_space) {
        vm_put(process->address_space);
    }
    if (process->personality && process->personality->free_data && process->personality_data) {
        process->personality->free_data(process->personality_data);
    }
    kfree(process->cwd);
    kfree(process->cmdline);
    kfree(process->exe);
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

struct address_space *process_address_space(struct process *process) {
    uint64_t flags = spin_lock_irqsave(&process_lock);
    struct address_space *as = process->address_space;
    if (as) {
        vm_get(as);
    }
    spin_unlock_irqrestore(&process_lock, flags);
    return as;
}

#define CMDLINE_MAX 4096

/* Remembers what a process is running, for /proc: its program's path and
 * its arguments (NUL-separated, like Linux's /proc/<pid>/cmdline). */
static void set_program(struct process *process, const char *path, char *const *argv,
                        size_t argc) {
    size_t length = 0;
    for (size_t i = 0; i < argc && length < CMDLINE_MAX; i++) {
        length += strlen(argv[i]) + 1;
    }
    length = length < CMDLINE_MAX ? length : CMDLINE_MAX;
    char *cmdline = kmalloc(length + 1);
    char *exe = kmalloc(strlen(path) + 1);
    if (cmdline) {
        size_t at = 0;
        for (size_t i = 0; i < argc && at < length; i++) {
            size_t n = strlen(argv[i]) + 1;
            n = n < length - at ? n : length - at;
            memcpy(cmdline + at, argv[i], n);
            at += n;
        }
    }
    if (exe) {
        strcpy(exe, path);
    }
    kfree(process->cmdline);
    kfree(process->exe);
    process->cmdline = cmdline;
    process->cmdline_length = cmdline ? length : 0;
    process->exe = exe;
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
                       uint64_t interp_base, uint64_t *stack_pointer) {
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
        {AT_PAGESZ, PAGE_SIZE}, {AT_BASE, interp_base}, {AT_FLAGS, 0}, {AT_ENTRY, image->entry},
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

#define MAX_SCRIPT_DEPTH 4 /* A script whose interpreter is a script... */
#define SHEBANG_MAX 256

/* A copy of `path` as `personality` finds it (see struct personality). */
static char *path_for(const struct personality *personality, const char *path) {
    char *translated = personality && personality->translate_path
                           ? personality->translate_path(path)
                           : NULL;
    if (translated) {
        return translated;
    }
    char *copy = kmalloc(strlen(path) + 1);
    if (copy) {
        strcpy(copy, path);
    }
    return copy;
}

static int open_program(const char *path, struct file **out, const char **reason) {
    int error = vfs_open(path, strlen(path), VX_OPEN_READ, out);
    if (error) {
        *reason = vfs_error_name(error);
        return error;
    }
    struct vx_stat stat;
    vfs_file_stat(*out, &stat);
    if (stat.type != VX_TYPE_FILE) {
        vfs_close(*out);
        *reason = stat.type == VX_TYPE_DIRECTORY ? "is a directory" : "not a file";
        return stat.type == VX_TYPE_DIRECTORY ? -VX_EISDIR : -VX_EACCES;
    }
    return 0;
}

/* "#!interpreter [argument]": runs the interpreter with the script's path
 * (and the optional argument) in front of the script's own arguments. */
static int load_script(const char *path, const char *line, char *const *argv, size_t argc,
                       char *const *envp, size_t envc, const struct personality *caller,
                       int depth, struct address_space **as_out, uint64_t *entry,
                       uint64_t *stack_pointer, const struct personality **personality,
                       const char **reason);

static int load_program(const char *path, char *const *argv, size_t argc, char *const *envp,
                        size_t envc, const struct personality *caller, int depth,
                        struct address_space **as_out, uint64_t *entry,
                        uint64_t *stack_pointer, const struct personality **personality,
                        const char **reason) {
    struct file *file;
    int error = open_program(path, &file, reason);
    if (error) {
        return error;
    }
    char start[SHEBANG_MAX + 1];
    int64_t n = vfs_pread(file, start, SHEBANG_MAX, 0);
    if (n >= 2 && start[0] == '#' && start[1] == '!') {
        vfs_close(file);
        if (depth >= MAX_SCRIPT_DEPTH) {
            *reason = "scripts nested too deeply";
            return -VX_ENOEXEC;
        }
        start[n] = '\0';
        return load_script(path, start + 2, argv, argc, envp, envc, caller, depth, as_out,
                           entry, stack_pointer, personality, reason);
    }

    struct address_space *as = vm_create();
    if (!as) {
        vfs_close(file);
        *reason = "out of memory";
        return -VX_ENOMEM;
    }
    struct elf_image image;
    error = elf_load(as, file, ELF_PROGRAM_BASE, &image, reason);
    vfs_close(file);
    uint64_t start_at = image.entry, interp_base = 0;
    if (!error && image.interp[0]) {
        /* A dynamically linked program: its loader starts first, finds the
         * program through the auxiliary vector, and links it. */
        char *interp_path = path_for(image.personality, image.interp);
        struct file *interp;
        error = interp_path ? open_program(interp_path, &interp, reason) : -VX_ENOMEM;
        kfree(interp_path);
        if (error == -VX_ENOENT) {
            *reason = "its dynamic loader (PT_INTERP) is missing";
        }
        if (!error) {
            struct elf_image loader;
            error = elf_load(as, interp, ELF_INTERP_BASE, &loader, reason);
            vfs_close(interp);
            if (!error && loader.interp[0]) {
                *reason = "its dynamic loader is itself dynamically linked";
                error = -VX_ENOEXEC;
            }
            start_at = loader.entry;
            interp_base = loader.base;
        }
    }
    if (!error) {
        as->heap_start = as->heap_end = image.heap_start;
        error = vm_add_area(as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VM_WRITE);
    }
    if (!error) {
        error = build_stack(as, argv, argc, envp, envc, &image, interp_base, stack_pointer);
        if (error) {
            *reason = error == -VX_E2BIG ? "arguments too long" : "out of memory";
        }
    }
    if (error) {
        vm_put(as);
        return error;
    }
    *as_out = as;
    *entry = start_at;
    *personality = image.personality;
    return 0;
}

static int load_script(const char *path, const char *line, char *const *argv, size_t argc,
                       char *const *envp, size_t envc, const struct personality *caller,
                       int depth, struct address_space **as_out, uint64_t *entry,
                       uint64_t *stack_pointer, const struct personality **personality,
                       const char **reason) {
    /* The interpreter, then at most one argument: the rest of the line. */
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *interp_start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
        p++;
    }
    size_t interp_length = (size_t)(p - interp_start);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *arg_start = p;
    while (*p && *p != '\n') {
        p++;
    }
    if (!*p || interp_length == 0) {
        *reason = "a script's #! line is empty or too long";
        return -VX_ENOEXEC;
    }
    const char *arg_end = p;
    while (arg_end > arg_start && (arg_end[-1] == ' ' || arg_end[-1] == '\t' || arg_end[-1] == '\r')) {
        arg_end--;
    }
    char interp[SHEBANG_MAX + 1], arg[SHEBANG_MAX + 1];
    memcpy(interp, interp_start, interp_length);
    interp[interp_length] = '\0';
    size_t arg_length = (size_t)(arg_end - arg_start);
    memcpy(arg, arg_start, arg_length);
    arg[arg_length] = '\0';

    /* New arguments: interpreter [arg] script-path original-arguments-after-argv[0]. */
    size_t new_argc = 0;
    char **new_argv = kmalloc((argc + 3) * sizeof(char *));
    char *interp_path = new_argv ? path_for(caller, interp) : NULL;
    if (!interp_path) {
        kfree(new_argv);
        *reason = "out of memory";
        return -VX_ENOMEM;
    }
    new_argv[new_argc++] = interp;
    if (arg_length) {
        new_argv[new_argc++] = arg;
    }
    new_argv[new_argc++] = (char *)path;
    for (size_t i = 1; i < argc; i++) {
        new_argv[new_argc++] = argv[i];
    }
    int error = load_program(interp_path, new_argv, new_argc, envp, envc, caller, depth + 1,
                             as_out, entry, stack_pointer, personality, reason);
    kfree(interp_path);
    kfree(new_argv);
    return error;
}

int process_load(const char *path, char *const *argv, size_t argc, char *const *envp,
                 size_t envc, const struct personality *caller, struct address_space **as_out,
                 uint64_t *entry, uint64_t *stack_pointer,
                 const struct personality **personality, const char **reason) {
    return load_program(path, argv, argc, envp, envc, caller, 0, as_out, entry, stack_pointer,
                        personality, reason);
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
                          request->envc, request->parent ? request->parent->personality : NULL,
                          &process->address_space, &process->entry,
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
    process->start_ms = timer_ms();
    set_program(process, request->path, request->argv, request->argc);

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

/* ---- fork and exec (for the Linux personality) ---- */

struct fork_start {
    struct process *process;
    struct interrupt_frame frame;
};

static void forked_thread_entry(void *arg) {
    struct fork_start *start = arg;
    struct interrupt_frame frame = start->frame; /* On this thread's own stack. */
    kfree(start);
    interrupts_disable();
    return_to_user(&frame);
}

static void copy_name(struct process *process, const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1]) {
            name = p + 1;
        }
    }
    size_t i = 0;
    for (; name[i] && name[i] != '/' && i < sizeof(process->name) - 1; i++) {
        process->name[i] = name[i];
    }
    process->name[i] = '\0';
}

struct process *process_fork(struct interrupt_frame *frame, int *error) {
    struct process *parent = process_current();
    struct thread *parent_thread = thread_current();
    *error = -VX_ENOMEM;
    struct process *child = kzalloc(sizeof(*child));
    struct fork_start *start = child ? kmalloc(sizeof(*start)) : NULL;
    char *cwd = start ? kmalloc(strlen(parent->cwd) + 1) : NULL;
    if (!cwd) {
        kfree(start);
        kfree(child);
        return NULL;
    }
    object_init(&child->object, &process_object_type);
    strcpy(cwd, parent->cwd);
    child->cwd = cwd;
    memcpy(child->name, parent->name, sizeof(child->name));
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));
    child->personality = parent->personality;
    child->address_space = vm_fork(parent->address_space);
    child->handles = child->address_space ? handle_table_clone(parent->handles) : NULL;
    bool data_ok = true;
    if (child->handles && parent->personality_data && parent->personality->fork_data) {
        child->personality_data = parent->personality->fork_data(parent->personality_data);
        data_ok = child->personality_data != NULL;
    }
    struct thread *thread = NULL;
    void *fpu = NULL;
    if (child->handles && data_ok) {
        thread = thread_create_stopped(child->name, forked_thread_entry, start);
        fpu = thread ? fpu_alloc_state() : NULL;
    }
    if (!fpu) {
        if (thread) {
            thread_destroy_unstarted(thread);
        }
        if (child->handles) {
            handle_table_destroy(child->handles);
        }
        kfree(start);
        object_put(&child->object); /* Frees the address space and data too. */
        return NULL;
    }
    /* The child starts where the parent is: same registers (fork returns 0
     * there), vector registers, and thread-local storage pointer. */
    start->process = child;
    start->frame = *frame;
    start->frame.rax = 0;
    fpu_save(fpu);
    thread->fpu_state = fpu;
    thread->process = child;
    thread->blocked_signals = parent_thread->blocked_signals;
    thread->fs_base = rdmsr(IA32_FS_BASE_MSR);
    child->main_thread = thread;
    child->parent = parent;
    child->start_ms = timer_ms();
    if (parent->exe) {
        child->exe = kmalloc(strlen(parent->exe) + 1);
        if (child->exe) {
            strcpy(child->exe, parent->exe);
        }
    }
    if (parent->cmdline) {
        child->cmdline = kmalloc(parent->cmdline_length + 1);
        if (child->cmdline) {
            memcpy(child->cmdline, parent->cmdline, parent->cmdline_length);
            child->cmdline_length = parent->cmdline_length;
        }
    }

    uint64_t flags = spin_lock_irqsave(&process_lock);
    child->id = next_process_id++;
    child->group = parent->group;
    child->next = processes;
    processes = child;
    object_ref(&child->object);
    spin_unlock_irqrestore(&process_lock, flags);

    *error = 0;
    thread_start(thread);
    return child;
}

int process_exec(const char *path, char *const *argv, size_t argc, char *const *envp,
                 size_t envc, struct interrupt_frame *frame, const char **reason) {
    struct process *process = process_current();
    struct address_space *as;
    uint64_t entry, stack_pointer;
    const struct personality *personality;
    int error = process_load(path, argv, argc, envp, envc, process->personality, &as, &entry,
                             &stack_pointer,
                             &personality, reason);
    if (error) {
        return error; /* The old program carries on. */
    }
    void *fresh_fpu = fpu_alloc_state();
    if (!fresh_fpu) {
        vm_put(as);
        *reason = "out of memory";
        return -VX_ENOMEM;
    }
    /* Past the point of no return: swap in the new program. This is the
     * process's only thread, so nothing else uses the old address space. */
    uint64_t lock_flags = spin_lock_irqsave(&process_lock);
    struct address_space *old = process->address_space;
    process->address_space = as;
    spin_unlock_irqrestore(&process_lock, lock_flags);
    vmm_activate(as);
    vm_put(old);
    set_program(process, path, argv, argc);

    if (process->personality_data && process->personality->free_data) {
        process->personality->free_data(process->personality_data);
    }
    process->personality_data = NULL;
    process->personality = personality;
    /* Handlers belong to the old program; ignored signals stay ignored. */
    for (int i = 0; i < VX_SIGNAL_COUNT; i++) {
        if (process->signal_actions[i] == SIGNAL_HANDLER) {
            process->signal_actions[i] = SIGNAL_DEFAULT;
        }
    }
    handle_close_on_exec(process->handles);
    copy_name(process, path);

    struct thread *thread = thread_current();
    memset(thread->name, 0, sizeof(thread->name));
    memcpy(thread->name, process->name, sizeof(thread->name) - 1);
    thread->fs_base = 0;
    wrmsr(IA32_FS_BASE_MSR, 0);
    fpu_restore(fresh_fpu);
    fpu_free_state(fresh_fpu);

    struct interrupt_frame start = {
        .rip = entry,
        .cs = frame->cs,
        .rflags = RFLAGS_INTERRUPTS_ON,
        .rsp = stack_pointer,
        .ss = frame->ss,
        .vector = frame->vector,
    };
    *frame = start;
    return 0;
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
    spin_lock_irqsave(&process_lock); /* Interrupts are already off. */
    struct address_space *as = process->address_space;
    process->address_space = NULL;
    spin_unlock(&process_lock);
    if (as) {
        vm_put(as);
    }
    process->main_thread = NULL; /* The thread is about to be freed. */
    process->state = PROCESS_EXITED;
    wait_queue_wake_all_locked(&process->exited);

    spin_lock_irqsave(&process_lock);
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
