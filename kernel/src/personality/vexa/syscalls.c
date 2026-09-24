#include <stddef.h>
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/uaccess.h>
#include <vexa/vfs.h>

/* The native Vexa personality: system calls from programs built with libvexa.
 * Numbers and error codes are defined in abi/vexa/abi.h. */

#define LOG_MAX (64 * 1024)
#define SLEEP_MAX_MS (24ULL * 60 * 60 * 1000)

typedef int64_t (*syscall_fn)(uint64_t a0, uint64_t a1, uint64_t a2);

static int64_t sys_exit(uint64_t code, uint64_t unused1, uint64_t unused2) {
    (void)unused1;
    (void)unused2;
    process_exit((int)code);
}

static int64_t sys_log(uint64_t text, uint64_t length, uint64_t unused) {
    (void)unused;
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

static int64_t sys_yield(uint64_t unused0, uint64_t unused1, uint64_t unused2) {
    (void)unused0;
    (void)unused1;
    (void)unused2;
    thread_yield();
    return 0;
}

static int64_t sys_sleep(uint64_t ms, uint64_t unused1, uint64_t unused2) {
    (void)unused1;
    (void)unused2;
    if (ms > SLEEP_MAX_MS) {
        return -VX_EINVAL;
    }
    thread_sleep_ms(ms);
    return 0;
}

static int64_t sys_process_id(uint64_t unused0, uint64_t unused1, uint64_t unused2) {
    (void)unused0;
    (void)unused1;
    (void)unused2;
    return process_current()->id;
}

static int64_t sys_uptime(uint64_t unused0, uint64_t unused1, uint64_t unused2) {
    (void)unused0;
    (void)unused1;
    (void)unused2;
    return (int64_t)timer_ms();
}

/* ---- Files ---- */

#define IO_CHUNK 4096

/* Copies a (pointer, length) path from user memory into a new kernel buffer. */
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
    buffer[length] = '\0';
    *out = buffer;
    return 0;
}

/* A buffer must lie entirely in user space, whether or not any of it ends up
 * being used (reading at the end of a file still rejects a kernel address). */
static bool user_range_ok(uint64_t address, uint64_t size) {
    return address >= USER_BASE && address + size >= address && address + size <= USER_END;
}

static struct file *get_file(int64_t handle, uint32_t rights, int *error) {
    return (struct file *)handle_get(process_current()->handles, (int)handle, &file_object_type,
                                     rights, error);
}

static int64_t sys_open(uint64_t path, uint64_t length, uint64_t flags) {
    const uint64_t known = VX_OPEN_READ | VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE |
                           VX_OPEN_APPEND;
    if (flags & ~known) {
        return -VX_EINVAL;
    }
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    struct file *file;
    error = vfs_open(kpath, length, (uint32_t)flags, &file);
    kfree(kpath);
    if (error) {
        return error;
    }
    uint32_t rights = (flags & VX_OPEN_READ ? HANDLE_RIGHT_READ : 0) |
                      (flags & (VX_OPEN_WRITE | VX_OPEN_APPEND) ? HANDLE_RIGHT_WRITE : 0);
    int handle = handle_add(process_current()->handles, &file->object, rights);
    if (handle < 0) {
        vfs_close(file);
    }
    return handle;
}

static int64_t sys_close(uint64_t handle, uint64_t unused1, uint64_t unused2) {
    (void)unused1;
    (void)unused2;
    return handle_close(process_current()->handles, (int)handle);
}

static int64_t sys_read(uint64_t handle, uint64_t buffer, uint64_t size) {
    if (!user_range_ok(buffer, size)) {
        return -VX_EFAULT;
    }
    int error;
    struct file *file = get_file((int64_t)handle, HANDLE_RIGHT_READ, &error);
    if (!file) {
        return error;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    int64_t total = chunk ? 0 : -VX_ENOMEM;
    while (chunk && (uint64_t)total < size) {
        uint64_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK;
        int64_t n = vfs_read(file, chunk, want);
        if (n < 0) {
            total = total ? total : n;
            break;
        }
        if (n > 0 && !copy_to_user(buffer + total, chunk, n)) {
            total = -VX_EFAULT;
            break;
        }
        total += n;
        if ((uint64_t)n < want) {
            break; /* End of file, or a device with nothing more right now. */
        }
    }
    kfree(chunk);
    vfs_close(file);
    return total;
}

static int64_t sys_write(uint64_t handle, uint64_t buffer, uint64_t size) {
    if (!user_range_ok(buffer, size)) {
        return -VX_EFAULT;
    }
    int error;
    struct file *file = get_file((int64_t)handle, HANDLE_RIGHT_WRITE, &error);
    if (!file) {
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
        int64_t n = vfs_write(file, chunk, want);
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
    vfs_close(file);
    return total;
}

static int64_t sys_seek(uint64_t handle, uint64_t offset, uint64_t whence) {
    int error;
    struct file *file = get_file((int64_t)handle, 0, &error);
    if (!file) {
        return error;
    }
    int64_t result = vfs_seek(file, (int64_t)offset, (int)whence);
    vfs_close(file);
    return result;
}

static int64_t sys_stat(uint64_t path, uint64_t length, uint64_t out) {
    char *kpath;
    int error = copy_path(path, length, &kpath);
    if (error) {
        return error;
    }
    struct vx_stat stat = {0};
    error = vfs_stat(kpath, length, &stat);
    kfree(kpath);
    if (!error && !copy_to_user(out, &stat, sizeof(stat))) {
        error = -VX_EFAULT;
    }
    return error;
}

static int64_t sys_handle_stat(uint64_t handle, uint64_t out, uint64_t unused) {
    (void)unused;
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

static int64_t sys_read_dir(uint64_t handle, uint64_t entries, uint64_t count) {
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
    error = fn(kpath, length);
    kfree(kpath);
    return error;
}

static int64_t sys_mkdir(uint64_t path, uint64_t length, uint64_t unused) {
    (void)unused;
    return path_call(path, length, vfs_mkdir);
}

static int64_t sys_remove(uint64_t path, uint64_t length, uint64_t unused) {
    (void)unused;
    return path_call(path, length, vfs_remove);
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
};

static void vexa_syscall(struct interrupt_frame *frame) {
    uint64_t number = frame->rax;
    if (number >= sizeof(syscalls) / sizeof(syscalls[0]) || !syscalls[number]) {
        frame->rax = (uint64_t)-VX_ENOSYS;
        return;
    }
    frame->rax = (uint64_t)syscalls[number](frame->rdi, frame->rsi, frame->rdx);
}

const struct personality vexa_personality = {
    .name = "vexa",
    .syscall = vexa_syscall,
};
