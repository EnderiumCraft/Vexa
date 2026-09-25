#include <stdarg.h>
#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/pipe.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/version.h>
#include <vexa/vfs.h>

/*
 * procfs: /proc, files that describe the running system, made up when read.
 * The layout and text formats are Linux's, since that's what programs such as
 * ps and top parse; native programs can read them too.
 *
 *   /proc/self                  link to the reader's own /proc/<pid>
 *   /proc/<pid>/stat, status, cmdline, comm, maps, exe, cwd, fd/
 *   /proc/meminfo, uptime, loadavg, stat, cpuinfo, version, mounts, filesystems
 *
 * Vnodes are made on lookup and freed on release; a file's text is generated
 * afresh on every read.
 */

enum proc_kind {
    PROC_ROOT,
    PROC_SELF,     /* Link: "<pid>" */
    PROC_GLOBAL,   /* A file under /proc */
    PROC_PID_DIR,
    PROC_PID_FILE, /* A file under /proc/<pid> */
    PROC_PID_LINK, /* exe, cwd */
    PROC_FD_DIR,
    PROC_FD_LINK,
};

struct text {
    char *data;
    size_t length, capacity;
    bool failed;
};

struct proc_node {
    struct vnode vnode;
    enum proc_kind kind;
    uint32_t pid;
    int index; /* Which entry (in the tables below), or the fd number. */
};

typedef void (*global_generator)(struct text *text);
typedef void (*process_generator)(struct text *text, struct process *process);

static struct mount *proc_mount;
static struct proc_node proc_root;
static const struct vnode_ops proc_ops;

/* ---- Text ---- */

static void text_printf(struct text *text, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void text_printf(struct text *text, const char *fmt, ...) {
    if (text->failed) {
        return;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list args;
        va_start(args, fmt);
        size_t room = text->capacity - text->length;
        size_t n = kvsnprintf(text->data ? text->data + text->length : NULL,
                              text->data ? room : 0, fmt, args);
        va_end(args);
        if (text->data && n < room) {
            text->length += n;
            return;
        }
        size_t capacity = text->capacity ? text->capacity : 1024;
        while (capacity < text->length + n + 1) {
            capacity *= 2;
        }
        char *bigger = kmalloc(capacity);
        if (!bigger) {
            text->failed = true;
            return;
        }
        memcpy(bigger, text->data, text->length);
        kfree(text->data);
        text->data = bigger;
        text->capacity = capacity;
    }
}

static void text_bytes(struct text *text, const char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        text_printf(text, "%c", bytes[i]);
    }
}

/* ---- Facts about processes and threads ---- */

struct thread_facts {
    struct process *process;
    bool found;
    enum thread_state state;
    uint64_t cpu_ms;
};

static void find_thread(struct thread *thread, void *arg) {
    struct thread_facts *facts = arg;
    if (thread->process == facts->process && thread->state != THREAD_DEAD) {
        facts->found = true;
        facts->state = thread->state;
        facts->cpu_ms += thread->cpu_ms;
    }
}

static struct thread_facts thread_facts(struct process *process) {
    struct thread_facts facts = {.process = process};
    sched_for_each_thread(find_thread, &facts); /* Under the scheduler lock. */
    return facts;
}

/* The one-letter state Linux tools expect. */
static char state_letter(struct process *process, const struct thread_facts *facts) {
    if (process->state == PROCESS_EXITED || !facts->found) {
        return 'Z';
    }
    return facts->state == THREAD_RUNNING || facts->state == THREAD_READY ? 'R' : 'S';
}

static const char *state_name(char letter) {
    return letter == 'R' ? "R (running)" : letter == 'Z' ? "Z (zombie)" : "S (sleeping)";
}

struct memory_facts {
    uint64_t size; /* Every area. */
};

static void add_area_size(const struct vm_area *area, void *arg) {
    ((struct memory_facts *)arg)->size += area->end - area->start;
}

static void memory_of(struct process *process, uint64_t *size, uint64_t *resident) {
    *size = *resident = 0;
    struct address_space *as = process_address_space(process);
    if (as) {
        struct memory_facts facts = {0};
        vm_for_each_area(as, add_area_size, &facts);
        *size = facts.size;
        *resident = vm_resident_bytes(as);
        vm_put(as);
    }
}

/* ---- /proc/<pid>/... ---- */

static void gen_stat(struct text *text, struct process *process) {
    struct thread_facts facts = thread_facts(process);
    uint64_t size, resident;
    memory_of(process, &size, &resident);
    struct process *parent = process->parent;
    /* Fields 1-52 of Linux's /proc/<pid>/stat; times are in 1/100 s. */
    text_printf(text, "%u (%s) %c %u %u 1 1024 %u 0 0 0 0 0 %lu 0 0 0 20 0 %u 0 %lu %lu %lu",
                process->id, process->name, state_letter(process, &facts),
                parent ? parent->id : 0, process->group, process->group, facts.cpu_ms / 10,
                process->thread_count,
                process->start_ms / 10, size, resident / PAGE_SIZE);
    text_printf(text, " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 %d\n",
                process->exit_code);
}

static void gen_status(struct text *text, struct process *process) {
    struct thread_facts facts = thread_facts(process);
    uint64_t size, resident;
    memory_of(process, &size, &resident);
    struct process *parent = process->parent;
    text_printf(text,
                "Name:\t%s\nState:\t%s\nTgid:\t%u\nPid:\t%u\nPPid:\t%u\nUid:\t0\t0\t0\t0\n"
                "Gid:\t0\t0\t0\t0\nVmSize:\t%lu kB\nVmRSS:\t%lu kB\nThreads:\t%u\n"
                "SigPnd:\t%016lx\nPersonality:\t%s\n",
                process->name, state_name(state_letter(process, &facts)), process->id,
                process->id, parent ? parent->id : 0, size / 1024, resident / 1024,
                process->thread_count, process->pending_signals, process->personality ? process->personality->name : "-");
}

static void gen_cmdline(struct text *text, struct process *process) {
    if (process->cmdline) {
        text_bytes(text, process->cmdline, process->cmdline_length);
    }
}

static void gen_comm(struct text *text, struct process *process) {
    text_printf(text, "%s\n", process->name);
}

struct maps_args {
    struct text *text;
    uint64_t heap_start, heap_end;
};

static void map_line(const struct vm_area *area, void *arg) {
    struct maps_args *maps = arg;
    if (maps->text->capacity - maps->text->length < 128) {
        return; /* Full; growing would allocate, which isn't allowed here. */
    }
    const char *name = "";
    if (area->start >= maps->heap_start && area->end <= maps->heap_end + PAGE_SIZE &&
        maps->heap_end > maps->heap_start) {
        name = "[heap]";
    } else if (area->end == 0x00007ffffffff000ULL) {
        name = "[stack]";
    }
    text_printf(maps->text, "%08lx-%08lx r%c%cp 00000000 00:00 0          %s\n", area->start,
                area->end, area->flags & VM_WRITE ? 'w' : '-', area->flags & VM_EXEC ? 'x' : '-',
                name);
}

static void gen_maps(struct text *text, struct process *process) {
    /* Lines are formatted with the address space locked, so into a buffer
     * that won't need to grow (kmalloc must not be called with it held). */
    struct text lines = {.data = kmalloc(64 * 1024), .capacity = 64 * 1024};
    struct address_space *as = lines.data ? process_address_space(process) : NULL;
    if (as) {
        struct maps_args maps = {&lines, as->heap_start, as->heap_end};
        lines.failed = false;
        vm_for_each_area(as, map_line, &maps);
        vm_put(as);
        text_bytes(text, lines.data, lines.length);
    }
    kfree(lines.data);
}

struct pid_entry {
    const char *name;
    enum proc_kind kind;
    process_generator generate;
};

static const struct pid_entry pid_entries[] = {
    {"stat", PROC_PID_FILE, gen_stat},
    {"status", PROC_PID_FILE, gen_status},
    {"cmdline", PROC_PID_FILE, gen_cmdline},
    {"comm", PROC_PID_FILE, gen_comm},
    {"maps", PROC_PID_FILE, gen_maps},
    {"exe", PROC_PID_LINK, NULL},
    {"cwd", PROC_PID_LINK, NULL},
    {"fd", PROC_FD_DIR, NULL},
};
#define PID_ENTRY_COUNT (int)(sizeof(pid_entries) / sizeof(pid_entries[0]))

/* ---- /proc/... ---- */

static void gen_meminfo(struct text *text) {
    uint64_t total = pmm_total_pages() * PAGE_SIZE / 1024;
    uint64_t free = pmm_free_pages() * PAGE_SIZE / 1024;
    text_printf(text,
                "MemTotal:       %lu kB\nMemFree:        %lu kB\nMemAvailable:   %lu kB\n"
                "Buffers:        0 kB\nCached:         0 kB\nSwapCached:     0 kB\n"
                "SwapTotal:      0 kB\nSwapFree:       0 kB\nShmem:          0 kB\n",
                total, free, free);
}

static uint64_t idle_ms(void) {
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].online) {
            ticks += cpus[i].idle_ticks;
        }
    }
    return ticks; /* The timer ticks at 1000 Hz. */
}

static void gen_uptime(struct text *text) {
    uint64_t up = timer_ms(), idle = idle_ms();
    text_printf(text, "%lu.%02lu %lu.%02lu\n", up / 1000, up % 1000 / 10, idle / 1000,
                idle % 1000 / 10);
}

struct count_args {
    uint32_t count, last;
};

static void count_process(struct process *process, void *arg) {
    struct count_args *count = arg;
    count->count++;
    if (process->id > count->last) {
        count->last = process->id;
    }
}

static void gen_loadavg(struct text *text) {
    struct count_args count = {0};
    process_for_each(count_process, &count);
    text_printf(text, "0.00 0.00 0.00 1/%u %u\n", count.count, count.last);
}

static void gen_stat_global(struct text *text) {
    uint64_t user = 0, idle = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].online) {
            user += cpus[i].busy_ticks / 10;
            idle += cpus[i].idle_ticks / 10;
        }
    }
    text_printf(text, "cpu  %lu 0 0 %lu 0 0 0 0 0 0\n", user, idle);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].online) {
            text_printf(text, "cpu%u %lu 0 0 %lu 0 0 0 0 0 0\n", i, cpus[i].busy_ticks / 10,
                        cpus[i].idle_ticks / 10);
        }
    }
    struct count_args count = {0};
    process_for_each(count_process, &count);
    text_printf(text, "btime %lu\nprocesses %u\nprocs_running 1\nprocs_blocked 0\n",
                (uint64_t)time_now() - timer_ms() / 1000, count.last);
}

static void gen_cpuinfo(struct text *text) {
    uint32_t a, b, c, d;
    char vendor[13], brand[49];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';
    memset(brand, 0, sizeof(brand));
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (uint32_t leaf = 0; leaf < 3; leaf++) {
            cpuid(0x80000002 + leaf, &a, &b, &c, &d);
            memcpy(brand + leaf * 16, &a, 4);
            memcpy(brand + leaf * 16 + 4, &b, 4);
            memcpy(brand + leaf * 16 + 8, &c, 4);
            memcpy(brand + leaf * 16 + 12, &d, 4);
        }
    }
    const char *name = brand;
    while (*name == ' ') {
        name++;
    }
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].online) {
            text_printf(text, "processor\t: %u\nvendor_id\t: %s\nmodel name\t: %s\n"
                              "flags\t\t: fpu sse sse2\n\n",
                        i, vendor, *name ? name : "x86_64 processor");
        }
    }
}

static void gen_version(struct text *text) {
    text_printf(text, "Vexa version %s (Linux subsystem, compatible with Linux 6.1.0)\n",
                VEXA_VERSION);
}

static void gen_mounts(struct text *text) {
    /* Reads of /proc files run with the VFS lock held, which also keeps the
     * mount list steady. */
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        bool device = strcmp(m->fs_name, "ext2") == 0;
        text_printf(text, "%s%s %s %s %s 0 0\n", device ? "/dev/" : "", m->source, m->path,
                    m->fs_name, m->read_only ? "ro" : "rw");
    }
}

static void gen_filesystems(struct text *text) {
    text_printf(text, "nodev\ttmpfs\nnodev\tdevfs\nnodev\tproc\n\text2\n");
}

struct global_entry {
    const char *name;
    global_generator generate;
};

static const struct global_entry global_entries[] = {
    {"meminfo", gen_meminfo},   {"uptime", gen_uptime},   {"loadavg", gen_loadavg},
    {"stat", gen_stat_global},  {"cpuinfo", gen_cpuinfo}, {"version", gen_version},
    {"mounts", gen_mounts},     {"filesystems", gen_filesystems},
};
#define GLOBAL_ENTRY_COUNT (int)(sizeof(global_entries) / sizeof(global_entries[0]))

/* ---- Vnodes ---- */

static struct proc_node *node_of(struct vnode *vnode) {
    return (struct proc_node *)vnode;
}

static struct proc_node *new_node(enum proc_kind kind, uint32_t pid, int index) {
    static const uint32_t types[] = {
        [PROC_ROOT] = VX_TYPE_DIRECTORY,     [PROC_SELF] = VX_TYPE_SYMLINK,
        [PROC_GLOBAL] = VX_TYPE_FILE,        [PROC_PID_DIR] = VX_TYPE_DIRECTORY,
        [PROC_PID_FILE] = VX_TYPE_FILE,      [PROC_PID_LINK] = VX_TYPE_SYMLINK,
        [PROC_FD_DIR] = VX_TYPE_DIRECTORY,   [PROC_FD_LINK] = VX_TYPE_SYMLINK,
    };
    struct proc_node *node = kzalloc(sizeof(*node));
    if (!node) {
        return NULL;
    }
    vnode_init(&node->vnode, proc_mount, types[kind], &proc_ops);
    node->kind = kind;
    node->pid = pid;
    node->index = index;
    node->vnode.inode = ((uint64_t)pid << 16) | ((uint64_t)kind << 10) | (uint64_t)(index & 0x3ff);
    node->vnode.modified = time_now();
    node->vnode.links = types[kind] == VX_TYPE_DIRECTORY ? 2 : 1;
    return node;
}

/* Where a link points: made fresh each time. Returns a new string or NULL. */
static char *link_target(struct proc_node *node) {
    char *target = kmalloc(VX_PATH_MAX);
    if (!target) {
        return NULL;
    }
    target[0] = '\0';
    struct process *me = process_current();
    if (node->kind == PROC_SELF) {
        ksnprintf(target, VX_PATH_MAX, "%u", me ? me->id : 0);
        return target;
    }
    struct process *process = process_find(node->pid);
    if (process && node->kind == PROC_PID_LINK) {
        const char *path = pid_entries[node->index].name[0] == 'e' ? process->exe : process->cwd;
        ksnprintf(target, VX_PATH_MAX, "%s", path ? path : "");
    } else if (process && node->kind == PROC_FD_LINK && process == me) {
        uint32_t rights;
        struct object *object = handle_get_any(me->handles, node->index, &rights);
        if (object && object->type == &file_object_type) {
            ksnprintf(target, VX_PATH_MAX, "%s", ((struct file *)object)->path);
        } else if (object) {
            ksnprintf(target, VX_PATH_MAX, "%s:[%lu]", object->type->name,
                      (uint64_t)(uintptr_t)object & 0xffffff);
        }
        if (object) {
            object_put(object);
        }
    }
    if (process) {
        object_put(&process->object);
    }
    if (!target[0]) {
        kfree(target);
        return NULL;
    }
    return target;
}

static int finish_node(struct proc_node *node, struct vnode **out) {
    if (!node) {
        return -VX_ENOMEM;
    }
    if (node->vnode.type == VX_TYPE_SYMLINK) {
        /* Path lookup reads `size` bytes of a link. */
        char *target = link_target(node);
        if (!target) {
            kfree(node);
            return -VX_ENOENT;
        }
        node->vnode.size = strlen(target);
        kfree(target);
    }
    *out = &node->vnode;
    return 0;
}

static bool parse_number(const char *name, size_t length, uint32_t *out) {
    if (length == 0 || length > 9) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(name[i] - '0');
    }
    *out = value;
    return true;
}

static bool names_equal(const char *name, size_t length, const char *other) {
    return strlen(other) == length && memcmp(name, other, length) == 0;
}

static int proc_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct proc_node *parent = node_of(dir);
    if (names_equal(name, length, "..")) {
        struct proc_node *up = parent->kind == PROC_FD_DIR ? new_node(PROC_PID_DIR, parent->pid, 0)
                                                           : NULL;
        if (up) {
            *out = &up->vnode;
            return 0;
        }
        vnode_ref(&proc_root.vnode);
        *out = &proc_root.vnode;
        return 0;
    }
    uint32_t number;
    switch (parent->kind) {
    case PROC_ROOT:
        if (names_equal(name, length, "self")) {
            return finish_node(new_node(PROC_SELF, 0, 0), out);
        }
        for (int i = 0; i < GLOBAL_ENTRY_COUNT; i++) {
            if (names_equal(name, length, global_entries[i].name)) {
                return finish_node(new_node(PROC_GLOBAL, 0, i), out);
            }
        }
        if (parse_number(name, length, &number)) {
            struct process *process = process_find(number);
            if (process) {
                object_put(&process->object);
                return finish_node(new_node(PROC_PID_DIR, number, 0), out);
            }
        }
        return -VX_ENOENT;
    case PROC_PID_DIR:
        for (int i = 0; i < PID_ENTRY_COUNT; i++) {
            if (names_equal(name, length, pid_entries[i].name)) {
                return finish_node(new_node(pid_entries[i].kind, parent->pid, i), out);
            }
        }
        return -VX_ENOENT;
    case PROC_FD_DIR:
        if (parse_number(name, length, &number) && number < HANDLE_MAX) {
            return finish_node(new_node(PROC_FD_LINK, parent->pid, (int)number), out);
        }
        return -VX_ENOENT;
    default:
        return -VX_ENOTDIR;
    }
}

struct pid_list {
    uint32_t ids[256];
    int count;
};

static void collect_pid(struct process *process, void *arg) {
    struct pid_list *list = arg;
    if (list->count < 256) {
        list->ids[list->count++] = process->id;
    }
}

static void set_entry(struct vx_dir_entry *entry, const char *name, uint32_t type, uint64_t inode) {
    size_t length = strlen(name);
    memcpy(entry->name, name, length + 1);
    entry->name_length = (unsigned)length;
    entry->type = type;
    entry->inode = inode;
}

static int proc_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry) {
    struct proc_node *node = node_of(dir);
    char name[16];
    if (node->kind == PROC_ROOT) {
        /* self, the global files, then one directory per process. */
        uint64_t i = (*cookie)++;
        if (i == 0) {
            set_entry(entry, "self", VX_TYPE_SYMLINK, 2);
            return 1;
        }
        if (i - 1 < (uint64_t)GLOBAL_ENTRY_COUNT) {
            set_entry(entry, global_entries[i - 1].name, VX_TYPE_FILE, 3 + i);
            return 1;
        }
        struct pid_list *list = kmalloc(sizeof(*list));
        if (!list) {
            return -VX_ENOMEM;
        }
        list->count = 0;
        process_for_each(collect_pid, list);
        /* Newest first in the list; show them oldest first. */
        uint64_t at = i - 1 - GLOBAL_ENTRY_COUNT;
        int result = 0;
        if (at < (uint64_t)list->count) {
            uint32_t id = list->ids[list->count - 1 - at];
            ksnprintf(name, sizeof(name), "%u", id);
            set_entry(entry, name, VX_TYPE_DIRECTORY, (uint64_t)id << 16);
            result = 1;
        }
        kfree(list);
        return result;
    }
    if (node->kind == PROC_PID_DIR) {
        if (*cookie >= (uint64_t)PID_ENTRY_COUNT) {
            return 0;
        }
        const struct pid_entry *e = &pid_entries[(*cookie)++];
        uint32_t type = e->kind == PROC_FD_DIR    ? VX_TYPE_DIRECTORY
                        : e->kind == PROC_PID_LINK ? VX_TYPE_SYMLINK
                                                   : VX_TYPE_FILE;
        set_entry(entry, e->name, type, ((uint64_t)node->pid << 16) | *cookie);
        return 1;
    }
    if (node->kind == PROC_FD_DIR) {
        struct process *me = process_current();
        if (!me || me->id != node->pid) {
            return 0; /* Only a process's own handles can be listed, for now. */
        }
        for (uint64_t fd = *cookie; fd < HANDLE_MAX; fd++) {
            if (handle_get_flags(me->handles, (int)fd) >= 0) {
                *cookie = fd + 1;
                ksnprintf(name, sizeof(name), "%lu", fd);
                set_entry(entry, name, VX_TYPE_SYMLINK, ((uint64_t)node->pid << 16) | fd);
                return 1;
            }
        }
        *cookie = HANDLE_MAX;
        return 0;
    }
    return -VX_ENOTDIR;
}

static int64_t proc_read(struct vnode *vnode, void *buffer, size_t size, uint64_t offset) {
    struct proc_node *node = node_of(vnode);
    struct text text = {0};
    if (node->kind == PROC_SELF || node->kind == PROC_PID_LINK || node->kind == PROC_FD_LINK) {
        char *target = link_target(node);
        if (!target) {
            return -VX_ENOENT;
        }
        text_printf(&text, "%s", target);
        kfree(target);
    } else if (node->kind == PROC_GLOBAL) {
        global_entries[node->index].generate(&text);
    } else if (node->kind == PROC_PID_FILE) {
        struct process *process = process_find(node->pid);
        if (!process) {
            return -VX_ENOENT; /* It has gone away. */
        }
        pid_entries[node->index].generate(&text, process);
        object_put(&process->object);
    } else {
        return -VX_EISDIR;
    }
    if (text.failed) {
        kfree(text.data);
        return -VX_ENOMEM;
    }
    int64_t n = 0;
    if (offset < text.length) {
        n = (int64_t)(text.length - offset < size ? text.length - offset : size);
        memcpy(buffer, text.data + offset, (size_t)n);
    }
    kfree(text.data);
    return n;
}

static void proc_release(struct vnode *vnode) {
    if (vnode != &proc_root.vnode) {
        kfree(node_of(vnode));
    }
}

static const struct vnode_ops proc_ops = {
    .lookup = proc_lookup,
    .read_dir = proc_read_dir,
    .read = proc_read,
    .release = proc_release,
};

static int proc_mount_fs(struct mount *mount, struct block_device *device) {
    (void)device;
    if (proc_mount) {
        return -VX_EBUSY;
    }
    proc_mount = mount;
    mount->read_only = true;
    vnode_init(&proc_root.vnode, mount, VX_TYPE_DIRECTORY, &proc_ops);
    proc_root.kind = PROC_ROOT;
    proc_root.vnode.inode = 1;
    proc_root.vnode.links = 2;
    proc_root.vnode.modified = time_now();
    mount->root = &proc_root.vnode;
    return 0;
}

const struct filesystem_type procfs_type = {
    .name = "proc",
    .mount = proc_mount_fs,
};
