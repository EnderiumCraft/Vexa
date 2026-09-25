#include <stddef.h>
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/monitor.h>
#include <vexa/object.h>
#include <vexa/pipe.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/tty.h>
#include <vexa/uaccess.h>
#include <vexa/version.h>
#include <vexa/vfs.h>

/* The native Vexa personality: system calls from programs built with libvexa.
 * Numbers and error codes are defined in abi/vexa/abi.h. */

#define LOG_MAX (64 * 1024)
#define SLEEP_MAX_MS (24ULL * 60 * 60 * 1000)
#define IO_CHUNK 4096
#define MAX_SPAWN_STRINGS 1024
#define MAX_SPAWN_BYTES (128 * 1024)

typedef int64_t (*syscall_fn)(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

static struct process *me(void) {
    return process_current();
}

/* A buffer must lie entirely in user space, whether or not any of it ends up
 * being used (reading at the end of a file still rejects a kernel address). */
static bool user_range_ok(uint64_t address, uint64_t size) {
    return address >= USER_BASE && address + size >= address && address + size <= USER_END;
}

/* Copies a (pointer, length) path from user memory and makes it absolute,
 * relative to the process's current directory. */
static int copy_path(uint64_t path, uint64_t length, char **out) {
    if (length > VX_PATH_MAX) {
        return -VX_ENAMETOOLONG;
    }
    char *buffer = kmalloc(length + 1);
    if (!buffer) {
        return -VX_ENOMEM;
    }
    if (!copy_from_user(buffer, path, length)) {
        kfree(buffer);
        return -VX_EFAULT;
    }
    *out = process_absolute_path(me(), buffer, length);
    kfree(buffer);
    return *out ? 0 : -VX_ENOMEM;
}

/* ---- Basics ---- */

static int64_t sys_exit(uint64_t code, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    process_exit((int)code);
}

static int64_t sys_log(uint64_t text, uint64_t length, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    if (length > LOG_MAX) {
        return -VX_EINVAL;
    }
    char chunk[256];
    for (uint64_t done = 0; done < length;) {
        uint64_t n = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
        if (!copy_from_user(chunk, text + done, n)) {
            return -VX_EFAULT;
        }
        kwrite(chunk, n);
        done += n;
    }
    return (int64_t)length;
}

static int64_t sys_yield(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0, (void)a1, (void)a2, (void)a3;
    thread_yield();
    return 0;
}

static int64_t sys_sleep(uint64_t ms, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    if (ms > SLEEP_MAX_MS) {
        return -VX_EINVAL;
    }
    return thread_sleep_ms_interruptible(ms);
}

static int64_t sys_process_id(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0, (void)a1, (void)a2, (void)a3;
    return me()->id;
}

static int64_t sys_uptime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0, (void)a1, (void)a2, (void)a3;
    return (int64_t)timer_ms();
}

/* ---- Files and other handles ---- */

static struct file *get_file(int64_t handle, uint32_t rights, int *error) {
    return (struct file *)handle_get(me()->handles, (int)handle, &file_object_type, rights, error);
}

static int64_t sys_open(uint64_t path, uint64_t length, uint64_t flags, uint64_t a3) {
    (void)a3;
    const uint64_t known = VX_OPEN_READ | VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE |
                           VX_OPEN_APPEND | VX_OPEN_NO_FOLLOW;
    if (flags & ~known) {
        return -VX_EINVAL;
    }
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    struct file *file;
    error = vfs_open(kpath, strlen(kpath), (uint32_t)flags, &file);
    kfree(kpath);
    if (error) {
        return error;
    }
    uint32_t rights = (flags & VX_OPEN_READ ? HANDLE_RIGHT_READ : 0) |
                      (flags & (VX_OPEN_WRITE | VX_OPEN_APPEND) ? HANDLE_RIGHT_WRITE : 0);
    int handle = handle_add(me()->handles, &file->object, rights);
    if (handle < 0) {
        vfs_close(file);
    }
    return handle;
}

static int64_t sys_close(uint64_t handle, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    return handle_close(me()->handles, (int)handle);
}

/* Reading and writing work on any handle whose object can (files, pipes...). */
static struct object *get_io(int64_t handle, uint32_t right, int *error) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)handle, &rights);
    if (!object) {
        *error = -VX_EBADF;
        return NULL;
    }
    bool can = right == HANDLE_RIGHT_READ ? object->type->read != NULL : object->type->write != NULL;
    if (!can || !(rights & right)) {
        object_put(object);
        *error = -VX_EACCES;
        return NULL;
    }
    return object;
}

static int64_t sys_read(uint64_t handle, uint64_t buffer, uint64_t size, uint64_t a3) {
    (void)a3;
    if (!user_range_ok(buffer, size)) {
        return -VX_EFAULT;
    }
    int error;
    struct object *object = get_io((int64_t)handle, HANDLE_RIGHT_READ, &error);
    if (!object) {
        return error;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    int64_t total = chunk ? 0 : -VX_ENOMEM;
    while (chunk && (uint64_t)total < size) {
        uint64_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK;
        int64_t n = object->type->read(object, chunk, want);
        if (n < 0) {
            total = total ? total : n;
            break;
        }
        if (n > 0 && !copy_to_user(buffer + total, chunk, n)) {
            total = -VX_EFAULT;
            break;
        }
        total += n;
        if ((uint64_t)n < want || object->type != &file_object_type) {
            break; /* End of file; or a pipe/terminal: return what's there. */
        }
    }
    kfree(chunk);
    object_put(object);
    return total;
}

static int64_t sys_write(uint64_t handle, uint64_t buffer, uint64_t size, uint64_t a3) {
    (void)a3;
    if (!user_range_ok(buffer, size)) {
        return -VX_EFAULT;
    }
    int error;
    struct object *object = get_io((int64_t)handle, HANDLE_RIGHT_WRITE, &error);
    if (!object) {
        return error;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    int64_t total = chunk ? 0 : -VX_ENOMEM;
    while (chunk && (uint64_t)total < size) {
        uint64_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK;
        if (!copy_from_user(chunk, buffer + total, want)) {
            total = total ? total : -VX_EFAULT;
            break;
        }
        int64_t n = object->type->write(object, chunk, want);
        if (n < 0) {
            total = total ? total : n;
            break;
        }
        total += n;
        if ((uint64_t)n < want) {
            break;
        }
    }
    kfree(chunk);
    object_put(object);
    return total;
}

static int64_t sys_seek(uint64_t handle, uint64_t offset, uint64_t whence, uint64_t a3) {
    (void)a3;
    int error;
    struct file *file = get_file((int64_t)handle, 0, &error);
    if (!file) {
        return error == -VX_EBADF ? -VX_ESPIPE : error;
    }
    int64_t result = vfs_seek(file, (int64_t)offset, (int)whence);
    vfs_close(file);
    return result;
}

static int64_t sys_stat(uint64_t path, uint64_t length, uint64_t out, uint64_t a3) {
    (void)a3;
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    struct vx_stat stat = {0};
    error = vfs_stat(kpath, strlen(kpath), &stat);
    kfree(kpath);
    if (!error && !copy_to_user(out, &stat, sizeof(stat))) {
        error = -VX_EFAULT;
    }
    return error;
}

static int64_t sys_handle_stat(uint64_t handle, uint64_t out, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    int error;
    struct file *file = get_file((int64_t)handle, 0, &error);
    if (!file) {
        return error;
    }
    struct vx_stat stat = {0};
    vfs_file_stat(file, &stat);
    vfs_close(file);
    return copy_to_user(out, &stat, sizeof(stat)) ? 0 : -VX_EFAULT;
}

static int64_t sys_read_dir(uint64_t handle, uint64_t entries, uint64_t count, uint64_t a3) {
    (void)a3;
    if (count > VX_PATH_MAX || !user_range_ok(entries, count * sizeof(struct vx_dir_entry))) {
        return -VX_EFAULT;
    }
    int error;
    struct file *file = get_file((int64_t)handle, HANDLE_RIGHT_READ, &error);
    if (!file) {
        return error;
    }
    struct vx_dir_entry *entry = kzalloc(sizeof(*entry));
    int64_t done = entry ? 0 : -VX_ENOMEM;
    while (entry && (uint64_t)done < count) {
        int result = vfs_read_dir(file, entry);
        if (result <= 0) {
            done = done ? done : result;
            break;
        }
        if (!copy_to_user(entries + done * sizeof(*entry), entry, sizeof(*entry))) {
            done = -VX_EFAULT;
            break;
        }
        done++;
    }
    kfree(entry);
    vfs_close(file);
    return done;
}

static int64_t path_call(uint64_t path, uint64_t length, int (*fn)(const char *, size_t)) {
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    error = fn(kpath, strlen(kpath));
    kfree(kpath);
    return error;
}

static int64_t sys_mkdir(uint64_t path, uint64_t length, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    return path_call(path, length, vfs_mkdir);
}

static int64_t sys_remove(uint64_t path, uint64_t length, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    return path_call(path, length, vfs_remove);
}

static int64_t sys_rename(uint64_t from, uint64_t from_length, uint64_t to, uint64_t to_length) {
    char *kfrom, *kto;
    int error = copy_path(from, from_length, &kfrom);
    if (error) {
        return error;
    }
    error = copy_path(to, to_length, &kto);
    if (!error) {
        error = vfs_rename(kfrom, strlen(kfrom), kto, strlen(kto));
        kfree(kto);
    }
    kfree(kfrom);
    return error;
}

static int64_t sys_symlink(uint64_t target, uint64_t target_length, uint64_t path,
                           uint64_t length) {
    if (target_length == 0 || target_length > VX_PATH_MAX) {
        return target_length ? -VX_ENAMETOOLONG : -VX_ENOENT;
    }
    char *ktarget = kmalloc(target_length + 1);
    if (!ktarget) {
        return -VX_ENOMEM;
    }
    if (!copy_from_user(ktarget, target, target_length)) {
        kfree(ktarget);
        return -VX_EFAULT;
    }
    ktarget[target_length] = '\0';
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (!error) {
        error = strlen(ktarget) != target_length ? -VX_EINVAL
                                                 : vfs_symlink(ktarget, kpath, strlen(kpath));
        kfree(kpath);
    }
    kfree(ktarget);
    return error;
}

static int64_t sys_readlink(uint64_t path, uint64_t length, uint64_t buffer, uint64_t size) {
    if (!user_range_ok(buffer, size)) {
        return -VX_EFAULT;
    }
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    size_t max = size < VX_PATH_MAX ? size : VX_PATH_MAX;
    char *target = kmalloc(max + 1);
    int64_t result = target ? vfs_readlink(kpath, strlen(kpath), target, max) : -VX_ENOMEM;
    if (result > 0 && !copy_to_user(buffer, target, result)) {
        result = -VX_EFAULT;
    }
    kfree(target);
    kfree(kpath);
    return result;
}

static int64_t sys_lstat(uint64_t path, uint64_t length, uint64_t out, uint64_t a3) {
    (void)a3;
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    struct vx_stat stat = {0};
    error = vfs_lstat(kpath, strlen(kpath), &stat);
    kfree(kpath);
    if (!error && !copy_to_user(out, &stat, sizeof(stat))) {
        error = -VX_EFAULT;
    }
    return error;
}

static int64_t sys_chdir(uint64_t path, uint64_t length, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    struct vx_stat stat;
    error = vfs_stat(kpath, strlen(kpath), &stat);
    if (!error && stat.type != VX_TYPE_DIRECTORY) {
        error = -VX_ENOTDIR;
    }
    if (error) {
        kfree(kpath);
        return error;
    }
    kfree(me()->cwd);
    me()->cwd = kpath;
    return 0;
}

static int64_t sys_getcwd(uint64_t buffer, uint64_t size, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    size_t length = strlen(me()->cwd);
    if (size < length + 1) {
        return -VX_EINVAL;
    }
    return copy_to_user(buffer, me()->cwd, length + 1) ? (int64_t)length : -VX_EFAULT;
}

static int64_t sys_pipe(uint64_t out, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    if (!user_range_ok(out, 2 * sizeof(int))) {
        return -VX_EFAULT;
    }
    struct object *read_end, *write_end;
    int error = pipe_create(&read_end, &write_end);
    if (error) {
        return error;
    }
    int handles[2];
    handles[0] = handle_add(me()->handles, read_end, HANDLE_RIGHT_READ);
    if (handles[0] < 0) {
        object_put(read_end);
        object_put(write_end);
        return handles[0];
    }
    handles[1] = handle_add(me()->handles, write_end, HANDLE_RIGHT_WRITE);
    if (handles[1] < 0) {
        handle_close(me()->handles, handles[0]);
        object_put(write_end);
        return handles[1];
    }
    if (!copy_to_user(out, handles, sizeof(handles))) {
        handle_close(me()->handles, handles[0]);
        handle_close(me()->handles, handles[1]);
        return -VX_EFAULT;
    }
    return 0;
}

/* ---- Processes ---- */

/* Copies an array of `count` user strings into one kernel allocation. */
static int copy_strings(uint64_t array, uint64_t count, char ***out, size_t *budget) {
    if (count > MAX_SPAWN_STRINGS) {
        return -VX_E2BIG;
    }
    char **strings = kzalloc((count + 1) * sizeof(char *));
    if (!strings) {
        return -VX_ENOMEM;
    }
    int error = 0;
    char *buffer = kmalloc(VX_PATH_MAX);
    for (uint64_t i = 0; i < count && !error && buffer; i++) {
        uint64_t pointer;
        if (!copy_from_user(&pointer, array + i * sizeof(uint64_t), sizeof(pointer))) {
            error = -VX_EFAULT;
            break;
        }
        int64_t length = copy_string_from_user(buffer, pointer, VX_PATH_MAX);
        if (length < 0 || (size_t)length + 1 > *budget) {
            error = length < 0 ? -VX_EFAULT : -VX_E2BIG;
            break;
        }
        *budget -= length + 1;
        strings[i] = kmalloc(length + 1);
        if (!strings[i]) {
            error = -VX_ENOMEM;
            break;
        }
        memcpy(strings[i], buffer, length + 1);
    }
    if (!buffer) {
        error = -VX_ENOMEM;
    }
    kfree(buffer);
    if (error) {
        for (uint64_t i = 0; i < count; i++) {
            kfree(strings[i]);
        }
        kfree(strings);
        return error;
    }
    *out = strings;
    return 0;
}

static void free_strings(char **strings, size_t count) {
    if (strings) {
        for (size_t i = 0; i < count; i++) {
            kfree(strings[i]);
        }
        kfree(strings);
    }
}

static int64_t sys_spawn(uint64_t path, uint64_t length, uint64_t user_request, uint64_t a3) {
    (void)a3;
    struct vx_spawn spawn;
    if (!copy_from_user(&spawn, user_request, sizeof(spawn))) {
        return -VX_EFAULT;
    }
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    size_t budget = MAX_SPAWN_BYTES;
    char **argv = NULL, **envp = NULL;
    error = copy_strings((uint64_t)spawn.argv, spawn.argc, &argv, &budget);
    if (!error) {
        error = copy_strings((uint64_t)spawn.envp, spawn.envc, &envp, &budget);
    }
    struct spawn_request request = {
        .path = kpath, .argv = argv, .argc = spawn.argc, .envp = envp, .envc = spawn.envc,
        .parent = me(), .new_group = spawn.flags & VX_SPAWN_NEW_GROUP, .cwd = me()->cwd,
        .join_group = spawn.flags & VX_SPAWN_JOIN_GROUP ? spawn.group : 0,
    };
    for (int i = 0; i < 3 && !error; i++) {
        if (spawn.handles[i] >= 0) {
            request.handles[i] = handle_get_any(me()->handles, spawn.handles[i], &request.rights[i]);
            if (!request.handles[i]) {
                error = -VX_EBADF;
            }
        }
    }
    int64_t result = error;
    if (!error) {
        const char *reason;
        struct process *child = process_spawn(&request, &error, &reason);
        if (child) {
            result = handle_add(me()->handles, &child->object, HANDLE_RIGHT_READ);
            if (result < 0) {
                object_put(&child->object);
            }
        } else {
            result = error;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (request.handles[i]) {
            object_put(request.handles[i]);
        }
    }
    free_strings(argv, spawn.argc);
    free_strings(envp, spawn.envc);
    kfree(kpath);
    return result;
}

static int64_t sys_wait(uint64_t handle, uint64_t flags, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    int error;
    struct process *child =
        (struct process *)handle_get(me()->handles, (int)handle, &process_object_type, 0, &error);
    if (!child) {
        return error;
    }
    if ((flags & VX_WAIT_NO_HANG) && child->state != PROCESS_EXITED) {
        object_put(&child->object);
        return -VX_EAGAIN;
    }
    error = process_wait_exit(child, true);
    int64_t result = error ? error : child->exit_code;
    if (!error && child->parent == me()) {
        process_reap(child);
    }
    object_put(&child->object);
    return result;
}

static int64_t sys_handle_process_id(uint64_t handle, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    int error;
    struct process *process =
        (struct process *)handle_get(me()->handles, (int)handle, &process_object_type, 0, &error);
    if (!process) {
        return error;
    }
    int64_t id = process->id;
    object_put(&process->object);
    return id;
}

static int64_t sys_map(uint64_t size, uint64_t flags, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    if (flags & ~(uint64_t)(VX_MAP_WRITE | VX_MAP_EXEC)) {
        return -VX_EINVAL;
    }
    unsigned vm_flags = (flags & VX_MAP_WRITE ? VM_WRITE : 0) | (flags & VX_MAP_EXEC ? VM_EXEC : 0);
    uint64_t address = vm_map(me()->address_space, size, vm_flags);
    return address ? (int64_t)address : -VX_ENOMEM;
}

static int64_t sys_unmap(uint64_t address, uint64_t size, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    if (address % PAGE_SIZE) {
        return -VX_EINVAL;
    }
    return vm_unmap(me()->address_space, address, size);
}

static int64_t sys_kill(uint64_t id, uint64_t signal, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    if (signal > VX_SIGNAL_COUNT) {
        return -VX_EINVAL;
    }
    struct process *target = process_find((uint32_t)id);
    if (!target) {
        return -VX_ESRCH;
    }
    if (signal) {
        signal_send(target, (int)signal);
    }
    object_put(&target->object);
    return 0;
}

static int64_t sys_signal(uint64_t signal, uint64_t action, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    if (signal < 1 || signal > VX_SIGNAL_COUNT || signal == VX_SIGKILL || signal == VX_SIGSTOP ||
        (action != VX_SIGNAL_DEFAULT && action != VX_SIGNAL_IGNORE)) {
        return -VX_EINVAL;
    }
    me()->signal_actions[signal - 1] = action == VX_SIGNAL_IGNORE ? SIGNAL_IGNORE : SIGNAL_DEFAULT;
    return 0;
}

static int64_t sys_set_foreground(uint64_t group, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    tty_set_foreground((uint32_t)group);
    return 0;
}

static int64_t sys_system_info(uint64_t out, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1, (void)a2, (void)a3;
    struct vx_system_info info = {0};
    memcpy(info.version, VEXA_VERSION, sizeof(VEXA_VERSION));
    info.cpus = cpu_online_count();
    info.memory_total = pmm_total_pages() * PAGE_SIZE;
    info.memory_free = pmm_free_pages() * PAGE_SIZE;
    info.uptime_ms = timer_ms();
    return copy_to_user(out, &info, sizeof(info)) ? 0 : -VX_EFAULT;
}

struct list_args {
    struct vx_process_info *entries;
    size_t count, max;
};

static void list_one(struct process *process, void *arg) {
    struct list_args *list = arg;
    if (list->count == list->max) {
        return;
    }
    struct vx_process_info *info = &list->entries[list->count++];
    info->id = process->id;
    info->parent = process->parent ? process->parent->id : 0;
    info->group = process->group;
    info->state = process->state == PROCESS_RUNNING ? 0 : 1;
    info->memory = 0;
    memcpy(info->name, process->name, sizeof(info->name));
}

static int64_t sys_process_list(uint64_t out, uint64_t count, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    if (count > 1024 || !user_range_ok(out, count * sizeof(struct vx_process_info))) {
        return -VX_EINVAL;
    }
    struct list_args list = {kzalloc(count * sizeof(struct vx_process_info) + 1), 0, count};
    if (!list.entries) {
        return -VX_ENOMEM;
    }
    process_for_each(list_one, &list);
    int64_t result = copy_to_user(out, list.entries, list.count * sizeof(struct vx_process_info))
                         ? (int64_t)list.count
                         : -VX_EFAULT;
    kfree(list.entries);
    return result;
}

static int64_t sys_kernel_command(uint64_t text, uint64_t length, uint64_t a2, uint64_t a3) {
    (void)a2, (void)a3;
    char line[128];
    if (length >= sizeof(line)) {
        return -VX_EINVAL;
    }
    if (!copy_from_user(line, text, length)) {
        return -VX_EFAULT;
    }
    line[length] = '\0';
    monitor_command(line);
    return 0;
}

static const syscall_fn syscalls[] = {
    [VX_SYS_EXIT] = sys_exit,
    [VX_SYS_LOG] = sys_log,
    [VX_SYS_YIELD] = sys_yield,
    [VX_SYS_SLEEP] = sys_sleep,
    [VX_SYS_PROCESS_ID] = sys_process_id,
    [VX_SYS_UPTIME] = sys_uptime,
    [VX_SYS_OPEN] = sys_open,
    [VX_SYS_CLOSE] = sys_close,
    [VX_SYS_READ] = sys_read,
    [VX_SYS_WRITE] = sys_write,
    [VX_SYS_SEEK] = sys_seek,
    [VX_SYS_STAT] = sys_stat,
    [VX_SYS_READ_DIR] = sys_read_dir,
    [VX_SYS_MKDIR] = sys_mkdir,
    [VX_SYS_REMOVE] = sys_remove,
    [VX_SYS_HANDLE_STAT] = sys_handle_stat,
    [VX_SYS_SPAWN] = sys_spawn,
    [VX_SYS_WAIT] = sys_wait,
    [VX_SYS_MAP] = sys_map,
    [VX_SYS_UNMAP] = sys_unmap,
    [VX_SYS_PIPE] = sys_pipe,
    [VX_SYS_CHDIR] = sys_chdir,
    [VX_SYS_GETCWD] = sys_getcwd,
    [VX_SYS_KILL] = sys_kill,
    [VX_SYS_SIGNAL] = sys_signal,
    [VX_SYS_SET_FOREGROUND] = sys_set_foreground,
    [VX_SYS_SYSTEM_INFO] = sys_system_info,
    [VX_SYS_PROCESS_LIST] = sys_process_list,
    [VX_SYS_KERNEL_COMMAND] = sys_kernel_command,
    [VX_SYS_RENAME] = sys_rename,
    [VX_SYS_HANDLE_PROCESS_ID] = sys_handle_process_id,
    [VX_SYS_SYMLINK] = sys_symlink,
    [VX_SYS_READLINK] = sys_readlink,
    [VX_SYS_LSTAT] = sys_lstat,
};

static void vexa_syscall(struct interrupt_frame *frame) {
    uint64_t number = frame->rax;
    if (number >= sizeof(syscalls) / sizeof(syscalls[0]) || !syscalls[number]) {
        frame->rax = (uint64_t)-VX_ENOSYS;
        return;
    }
    frame->rax = (uint64_t)syscalls[number](frame->rdi, frame->rsi, frame->rdx, frame->r10);
}

const struct personality vexa_personality = {
    .name = "vexa",
    .syscall = vexa_syscall,
};
