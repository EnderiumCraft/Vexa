#include <stddef.h>
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/fpu.h>
#include <vexa/fs.h>
#include <vexa/futex.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/pipe.h>
#include <vexa/process.h>
#include <vexa/random.h>
#include <vexa/sched.h>
#include <vexa/signal.h>
#include <vexa/socket.h>
#include <vexa/string.h>
#include <vexa/tty.h>
#include <vexa/uaccess.h>
#include <vexa/version.h>
#include <vexa/vfs.h>

#include "linux.h"
#include "sockets.h"

/*
 * The Linux personality: runs unmodified Linux x86_64 programs (for now,
 * static ones such as BusyBox built with musl) by translating their system
 * calls into the Vexa core. It is optional (LINUX_COMPAT in the Makefile);
 * nothing else in the kernel depends on it. See docs/ARCHITECTURE.md.
 *
 * Linux file descriptors are the process's handle numbers, and Linux signal
 * numbers are the core's, so both pass straight through. Everything else
 * (structure layouts, flags, error numbers) is converted here.
 *
 * Calls that aren't implemented return ENOSYS and are logged once each, with
 * their arguments, so it's easy to see what a program is missing.
 */

#pragma GCC diagnostic ignored "-Wunused-parameter"

#define SYSCALL_VECTOR 0x100 /* The frame's vector slot on the syscall path (syscall.S). */
#define IO_CHUNK 4096
#define MAX_EXEC_STRINGS 1024
#define MAX_EXEC_BYTES (128 * 1024)
#define SAVED_FPU_MAX 16

#define BIT(signal) (1ULL << ((signal) - 1))
#define UNBLOCKABLE (BIT(VX_SIGKILL) | BIT(VX_SIGSTOP))

typedef int64_t (*linux_fn)(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5);

/* ---- Per-process state ---- */

struct saved_fpu {
    uint64_t frame; /* The signal frame (ucontext address) it belongs to. */
    void *state;
    struct saved_fpu *next;
};

/* Shared by the process's threads (thread->process->personality_data). */
struct linux_data {
    struct linux_sigaction actions[VX_SIGNAL_COUNT];
};

/* Each thread's own (thread->personality_data). */
struct linux_thread {
    uint64_t last_syscall;
    bool suspend_mask_valid; /* rt_sigsuspend: the mask to restore after the handler. */
    uint64_t suspend_mask;
    /* Vector registers of interrupted code, saved while a handler runs. They
     * stay in the kernel so a program can't hand back a malformed state. */
    struct saved_fpu *saved_fpu;
    int saved_fpu_count;
};

static struct process *me(void) {
    return process_current();
}

static bool user_range_ok(uint64_t address, uint64_t size) {
    return address >= USER_BASE && address + size >= address && address + size <= USER_END;
}

static struct linux_data *data(void) {
    struct process *process = me();
    if (!process->personality_data) {
        process->personality_data = kzalloc(sizeof(struct linux_data));
    }
    return process->personality_data;
}

static struct linux_thread *tdata(void) {
    struct thread *thread = thread_current();
    if (!thread->personality_data) {
        thread->personality_data = kzalloc(sizeof(struct linux_thread));
    }
    return thread->personality_data;
}

static void free_saved_fpu(struct saved_fpu *saved) {
    fpu_free_state(saved->state);
    kfree(saved);
}

static void linux_free_data(void *p) {
    kfree(p);
}

static void *linux_fork_data(void *p) {
    struct linux_data *copy = kmalloc(sizeof(*copy));
    if (copy) {
        memcpy(copy, p, sizeof(*copy));
    }
    return copy;
}

static void linux_free_thread_data(void *p) {
    struct linux_thread *t = p;
    while (t->saved_fpu) {
        struct saved_fpu *next = t->saved_fpu->next;
        free_saved_fpu(t->saved_fpu);
        t->saved_fpu = next;
    }
    kfree(t);
}

/* ---- Errors ---- */

static const uint8_t errno_of_vx[] = {
    [VX_ENOSYS] = LE_ENOSYS, [VX_EFAULT] = LE_EFAULT, [VX_EINVAL] = LE_EINVAL,
    [VX_ENOENT] = LE_ENOENT, [VX_EEXIST] = LE_EEXIST, [VX_ENOTDIR] = LE_ENOTDIR,
    [VX_EISDIR] = LE_EISDIR, [VX_ENOTEMPTY] = LE_ENOTEMPTY, [VX_EBADF] = LE_EBADF,
    [VX_EACCES] = LE_EACCES, [VX_ENOSPC] = LE_ENOSPC, [VX_EIO] = LE_EIO,
    [VX_ENAMETOOLONG] = LE_ENAMETOOLONG, [VX_EMFILE] = LE_EMFILE, [VX_ENOMEM] = LE_ENOMEM,
    [VX_EROFS] = LE_EROFS, [VX_EBUSY] = LE_EBUSY, [VX_EXDEV] = LE_EXDEV,
    [VX_EINTR] = LE_EINTR, [VX_EPIPE] = LE_EPIPE, [VX_ECHILD] = LE_ECHILD,
    [VX_ESRCH] = LE_ESRCH, [VX_EAGAIN] = LE_EAGAIN, [VX_ENOEXEC] = LE_ENOEXEC,
    [VX_E2BIG] = LE_E2BIG, [VX_ENOTTY] = LE_ENOTTY, [VX_ESPIPE] = LE_ESPIPE,
    [VX_ELOOP] = LE_ELOOP, [VX_ETIMEDOUT] = LE_ETIMEDOUT, [VX_ENOTSOCK] = LE_ENOTSOCK,
    [VX_EAFNOSUPPORT] = LE_EAFNOSUPPORT, [VX_EPROTONOSUPPORT] = LE_EPROTONOSUPPORT,
    [VX_EOPNOTSUPP] = LE_EOPNOTSUPP, [VX_EADDRINUSE] = LE_EADDRINUSE,
    [VX_EADDRNOTAVAIL] = LE_EADDRNOTAVAIL, [VX_ENETUNREACH] = LE_ENETUNREACH,
    [VX_ECONNREFUSED] = LE_ECONNREFUSED, [VX_ECONNRESET] = LE_ECONNRESET,
    [VX_ENOTCONN] = LE_ENOTCONN, [VX_EISCONN] = LE_EISCONN, [VX_EINPROGRESS] = LE_EINPROGRESS,
    [VX_EALREADY] = LE_EALREADY, [VX_EMSGSIZE] = LE_EMSGSIZE,
    [VX_EDESTADDRREQ] = LE_EDESTADDRREQ, [VX_ENOPROTOOPT] = LE_ENOPROTOOPT,
    [VX_ECONNABORTED] = LE_ECONNABORTED, [VX_EHOSTUNREACH] = LE_EHOSTUNREACH,
};

/* Converts a core result (negative VX_E* on failure) into a Linux one. */
static int64_t lx(int64_t result) {
    if (result >= 0) {
        return result;
    }
    uint64_t code = (uint64_t)-result;
    if (code < sizeof(errno_of_vx) && errno_of_vx[code]) {
        return -(int64_t)errno_of_vx[code];
    }
    return -LE_EINVAL;
}

int64_t linux_errno(int64_t result) {
    return lx(result);
}

/* ---- Paths ----
 *
 * Linux programs expect a Linux file system layout: /lib/ld-musl-x86_64.so.1,
 * /bin/sh, /usr/lib... Those live under /linux, so that Vexa's own / stays
 * Vexa's. An absolute path from a Linux program is looked up under /linux
 * first, and used as it is if nothing is there (so /dev, /tmp, /mnt and
 * Vexa's own programs are still reachable). The root itself isn't redirected.
 * FreeBSD's Linux emulation works the same way. */

#define LINUX_ROOT "/linux"
#define LINUX_ROOT_LENGTH (sizeof(LINUX_ROOT) - 1)

static bool under_linux_root(const char *path) {
    return memcmp(path, LINUX_ROOT, LINUX_ROOT_LENGTH) == 0 &&
           (path[LINUX_ROOT_LENGTH] == '/' || path[LINUX_ROOT_LENGTH] == '\0');
}

static bool is_directory(const char *path, size_t length) {
    struct vx_stat st;
    return vfs_stat(path, length, &st) == 0 && st.type == VX_TYPE_DIRECTORY;
}

static char *linux_translate_path(const char *path) {
    if (path[0] != '/' || path[1] == '\0' || under_linux_root(path)) {
        return NULL;
    }
    size_t length = strlen(path);
    char *candidate = kmalloc(LINUX_ROOT_LENGTH + length + 1);
    if (!candidate) {
        return NULL;
    }
    memcpy(candidate, LINUX_ROOT, LINUX_ROOT_LENGTH);
    memcpy(candidate + LINUX_ROOT_LENGTH, path, length + 1);
    struct vx_stat st;
    if (vfs_lstat(candidate, LINUX_ROOT_LENGTH + length, &st) == 0) {
        return candidate;
    }
    /* Something new (a file being created): it goes under /linux if its
     * directory exists only there. */
    size_t parent = length;
    while (parent > 0 && path[parent - 1] != '/') {
        parent--;
    }
    if (parent > 1 && is_directory(candidate, LINUX_ROOT_LENGTH + parent - 1) &&
        !is_directory(path, parent - 1)) {
        return candidate;
    }
    kfree(candidate);
    return NULL;
}

/* Makes a (normalized, absolute) path Linux-relative: see above. Takes over `path`. */
static char *linux_path(char *path) {
    char *translated = path ? linux_translate_path(path) : NULL;
    if (translated) {
        kfree(path);
        return translated;
    }
    return path;
}

/* Copies a path from user memory and makes it absolute: relative to the
 * directory open at `dirfd`, or to the current directory for AT_FDCWD. */
static int64_t path_at(int64_t dirfd, uint64_t user_path, char **out) {
    char *buffer = kmalloc(VX_PATH_MAX + 1);
    if (!buffer) {
        return -LE_ENOMEM;
    }
    int64_t length = copy_string_from_user(buffer, user_path, VX_PATH_MAX + 1);
    if (length < 0) {
        kfree(buffer);
        char first;
        return copy_from_user(&first, user_path, 1) ? -LE_ENAMETOOLONG : -LE_EFAULT;
    }
    if (length == 0) {
        kfree(buffer);
        return -LE_ENOENT;
    }
    if (buffer[0] == '/' || (int)dirfd == LINUX_AT_FDCWD) {
        *out = linux_path(process_absolute_path(me(), buffer, length));
        kfree(buffer);
        return *out ? 0 : -LE_ENOMEM;
    }
    int error;
    struct file *dir =
        (struct file *)handle_get(me()->handles, (int)dirfd, &file_object_type, 0, &error);
    if (!dir) {
        kfree(buffer);
        return lx(error);
    }
    if (dir->vnode->type != VX_TYPE_DIRECTORY || !dir->path) {
        vfs_close(dir);
        kfree(buffer);
        return -LE_ENOTDIR;
    }
    size_t base = strlen(dir->path);
    char *joined = kmalloc(base + 1 + length + 1);
    if (joined) {
        memcpy(joined, dir->path, base);
        joined[base] = '/';
        memcpy(joined + base + 1, buffer, length + 1);
        *out = linux_path(process_absolute_path(me(), joined, base + 1 + length));
        kfree(joined);
    }
    vfs_close(dir);
    kfree(buffer);
    return joined && *out ? 0 : -LE_ENOMEM;
}

/* ---- Handles ---- */

static struct file *get_file(int64_t fd, uint32_t rights, int64_t *error) {
    int e;
    struct file *file =
        (struct file *)handle_get(me()->handles, (int)fd, &file_object_type, rights, &e);
    if (!file) {
        /* Not a file: either no such handle, or a pipe or other object. */
        uint32_t r;
        struct object *other = handle_get_any(me()->handles, (int)fd, &r);
        *error = other ? -LE_ESPIPE : -LE_EBADF;
        if (other) {
            object_put(other);
        }
        if (e == -VX_EACCES) {
            *error = -LE_EBADF;
        }
    }
    return file;
}

static bool is_terminal(struct object *object) {
    return object->type == &file_object_type && vfs_is_terminal((struct file *)object);
}

/* ---- Reading and writing ---- */

static int64_t read_object(struct object *object, uint64_t buffer, uint64_t size) {
    if (socket_of(object)) {
        return linux_socket_io(object, buffer, size, false);
    }
    if (!object->type->read) {
        return -LE_EINVAL;
    }
    if (object->type == &file_object_type &&
        ((struct file *)object)->vnode->type == VX_TYPE_DIRECTORY) {
        return -LE_EISDIR;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    if (!chunk) {
        return -LE_ENOMEM;
    }
    int64_t total = 0;
    while ((uint64_t)total < size) {
        uint64_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK;
        int64_t n = object->type->read(object, chunk, want);
        if (n < 0) {
            total = total ? total : lx(n);
            break;
        }
        if (n > 0 && !copy_to_user(buffer + total, chunk, n)) {
            total = total ? total : -LE_EFAULT;
            break;
        }
        total += n;
        if ((uint64_t)n < want || object->type != &file_object_type || is_terminal(object)) {
            break;
        }
    }
    kfree(chunk);
    return total;
}

static int64_t write_object(struct object *object, uint64_t buffer, uint64_t size) {
    if (socket_of(object)) {
        return linux_socket_io(object, buffer, size, true);
    }
    if (!object->type->write) {
        return -LE_EINVAL;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    if (!chunk) {
        return -LE_ENOMEM;
    }
    int64_t total = 0;
    while ((uint64_t)total < size) {
        uint64_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK;
        if (!copy_from_user(chunk, buffer + total, want)) {
            total = total ? total : -LE_EFAULT;
            break;
        }
        int64_t n = object->type->write(object, chunk, want);
        if (n < 0) {
            total = total ? total : lx(n);
            break;
        }
        total += n;
        if ((uint64_t)n < want) {
            break;
        }
    }
    kfree(chunk);
    return total;
}

static struct object *get_io(int64_t fd, uint32_t right, int64_t *error) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)fd, &rights);
    if (!object || !(rights & right)) {
        if (object) {
            object_put(object);
        }
        *error = -LE_EBADF;
        return NULL;
    }
    return object;
}

static int64_t sys_read(struct interrupt_frame *f, uint64_t fd, uint64_t buffer, uint64_t size,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!user_range_ok(buffer, size)) {
        return -LE_EFAULT;
    }
    int64_t error;
    struct object *object = get_io(fd, HANDLE_RIGHT_READ, &error);
    if (!object) {
        return error;
    }
    int64_t result = read_object(object, buffer, size);
    object_put(object);
    return result;
}

static int64_t sys_write(struct interrupt_frame *f, uint64_t fd, uint64_t buffer, uint64_t size,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!user_range_ok(buffer, size)) {
        return -LE_EFAULT;
    }
    int64_t error;
    struct object *object = get_io(fd, HANDLE_RIGHT_WRITE, &error);
    if (!object) {
        return error;
    }
    int64_t result = write_object(object, buffer, size);
    object_put(object);
    return result;
}

static int64_t vector_io(uint64_t fd, uint64_t iov, uint64_t count, bool writing) {
    if (count > 1024) {
        return -LE_EINVAL;
    }
    int64_t error;
    struct object *object = get_io(fd, writing ? HANDLE_RIGHT_WRITE : HANDLE_RIGHT_READ, &error);
    if (!object) {
        return error;
    }
    if (socket_of(object)) {
        /* One message, not one per buffer (a datagram stays whole). */
        object_put(object);
        struct linux_msghdr m = {.iov = iov, .iov_count = count};
        return writing ? linux_socket_sendmsg(fd, &m, 0) : linux_socket_recvmsg(fd, &m, 0);
    }
    int64_t total = 0;
    for (uint64_t i = 0; i < count; i++) {
        struct linux_iovec v;
        if (!copy_from_user(&v, iov + i * sizeof(v), sizeof(v)) ||
            (v.length && !user_range_ok(v.base, v.length))) {
            total = total ? total : -LE_EFAULT;
            break;
        }
        if (v.length == 0) {
            continue;
        }
        int64_t n = writing ? write_object(object, v.base, v.length)
                            : read_object(object, v.base, v.length);
        if (n < 0) {
            total = total ? total : n;
            break;
        }
        total += n;
        if ((uint64_t)n < v.length) {
            break;
        }
    }
    object_put(object);
    return total;
}

static int64_t sys_readv(struct interrupt_frame *f, uint64_t fd, uint64_t iov, uint64_t count,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return vector_io(fd, iov, count, false);
}

static int64_t sys_writev(struct interrupt_frame *f, uint64_t fd, uint64_t iov, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return vector_io(fd, iov, count, true);
}

static int64_t sys_pread64(struct interrupt_frame *f, uint64_t fd, uint64_t buffer,
                           uint64_t size, uint64_t offset, uint64_t a4, uint64_t a5) {
    if (!user_range_ok(buffer, size)) {
        return -LE_EFAULT;
    }
    int64_t error;
    struct file *file = get_file(fd, HANDLE_RIGHT_READ, &error);
    if (!file) {
        return error;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    int64_t total = chunk ? 0 : -LE_ENOMEM;
    while (chunk && (uint64_t)total < size) {
        uint64_t want = size - total < IO_CHUNK ? size - total : IO_CHUNK;
        int64_t n = vfs_pread(file, chunk, want, offset + total);
        if (n < 0) {
            total = total ? total : lx(n);
            break;
        }
        if (n > 0 && !copy_to_user(buffer + total, chunk, n)) {
            total = total ? total : -LE_EFAULT;
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

static int64_t sys_pwrite64(struct interrupt_frame *f, uint64_t fd, uint64_t buffer,
                            uint64_t size, uint64_t offset, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct file *file = get_file(fd, HANDLE_RIGHT_WRITE, &error);
    if (!file) {
        return error;
    }
    /* Write at the offset, then put the file position back. */
    uint64_t saved = file->offset;
    file->offset = offset;
    int64_t result = write_object(&file->object, buffer, size);
    file->offset = saved;
    vfs_close(file);
    return result;
}

static int64_t sys_sendfile(struct interrupt_frame *f, uint64_t out_fd, uint64_t in_fd,
                            uint64_t offset_ptr, uint64_t count, uint64_t a4, uint64_t a5) {
    if (offset_ptr) {
        return -LE_EINVAL; /* Callers fall back to read and write. */
    }
    int64_t error;
    struct object *in = get_io(in_fd, HANDLE_RIGHT_READ, &error);
    if (!in) {
        return error;
    }
    struct object *out = get_io(out_fd, HANDLE_RIGHT_WRITE, &error);
    if (!out) {
        object_put(in);
        return error;
    }
    uint8_t *chunk = kmalloc(IO_CHUNK);
    int64_t total = chunk ? 0 : -LE_ENOMEM;
    while (chunk && (uint64_t)total < count && in->type->read && out->type->write) {
        uint64_t want = count - total < IO_CHUNK ? count - total : IO_CHUNK;
        int64_t n = in->type->read(in, chunk, want);
        if (n <= 0) {
            total = total ? total : lx(n);
            break;
        }
        int64_t w = out->type->write(out, chunk, n);
        if (w < 0) {
            total = total ? total : lx(w);
            break;
        }
        total += w;
        if (w < n) {
            break;
        }
    }
    kfree(chunk);
    object_put(in);
    object_put(out);
    return total;
}

/* ---- Opening and closing ---- */

/* If `path` names one of the caller's own descriptors (/dev/fd/N,
 * /proc/self/fd/N, /dev/stdin...), returns N. Opening such a path on Linux
 * reopens whatever the descriptor refers to, even a pipe (bash's <(...) passes
 * /dev/fd/63), which a plain symbolic link can't do. */
static int own_descriptor(const char *path) {
    static const char *const names[] = {"/dev/stdin", "/dev/stdout", "/dev/stderr"};
    for (int i = 0; i < 3; i++) {
        if (strcmp(path, names[i]) == 0) {
            return i;
        }
    }
    char self[32];
    ksnprintf(self, sizeof(self), "/proc/%u/fd/", me()->id);
    const char *prefixes[] = {"/dev/fd/", "/proc/self/fd/", self};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(prefixes[i]);
        if (strlen(path) <= n || memcmp(path, prefixes[i], n) != 0) {
            continue;
        }
        int fd = 0;
        for (const char *p = path + n; *p; p++) {
            if (*p < '0' || *p > '9' || fd > HANDLE_MAX) {
                return -1;
            }
            fd = fd * 10 + (*p - '0');
        }
        return fd;
    }
    return -1;
}

static int64_t dup_from(int64_t old, int64_t min, bool close_on_exec);

static uint32_t umask_now(void);

static int64_t do_openat(int64_t dirfd, uint64_t user_path, uint64_t flags, uint64_t mode) {
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    int own = own_descriptor(path);
    if (own >= 0) {
        kfree(path);
        return dup_from(own, 0, flags & LINUX_O_CLOEXEC);
    }
    uint32_t vx_flags = 0;
    switch (flags & LINUX_O_ACCMODE) {
    case LINUX_O_RDONLY: vx_flags = VX_OPEN_READ; break;
    case LINUX_O_WRONLY: vx_flags = VX_OPEN_WRITE; break;
    default: vx_flags = VX_OPEN_READ | VX_OPEN_WRITE; break;
    }
    vx_flags |= (flags & LINUX_O_CREAT ? VX_OPEN_CREATE : 0) |
                (flags & LINUX_O_TRUNC ? VX_OPEN_TRUNCATE : 0) |
                (flags & LINUX_O_APPEND ? VX_OPEN_APPEND : 0) |
                (flags & LINUX_O_NOFOLLOW ? VX_OPEN_NO_FOLLOW : 0);
    struct vx_stat stat;
    int exists = vfs_stat(path, strlen(path), &stat);
    if ((flags & LINUX_O_CREAT) && (flags & LINUX_O_EXCL) && exists == 0) {
        kfree(path);
        return -LE_EEXIST;
    }
    if ((flags & LINUX_O_DIRECTORY) && (exists || stat.type != VX_TYPE_DIRECTORY)) {
        kfree(path);
        return exists ? lx(exists) : -LE_ENOTDIR;
    }
    struct file *file;
    error = vfs_open(path, strlen(path), vx_flags, &file);
    kfree(path);
    if (error) {
        return lx(error);
    }
    if (exists && (flags & LINUX_O_CREAT)) {
        vfs_file_chmod(file, (uint32_t)mode & ~umask_now() & 07777); /* A new file. */
    }
    if (flags & LINUX_O_NONBLOCK) {
        file->object.flags |= OBJECT_NONBLOCK;
    }
    uint32_t rights = (vx_flags & VX_OPEN_READ ? HANDLE_RIGHT_READ : 0) |
                      (vx_flags & (VX_OPEN_WRITE | VX_OPEN_APPEND) ? HANDLE_RIGHT_WRITE : 0);
    int fd = handle_add(me()->handles, &file->object, rights);
    if (fd < 0) {
        vfs_close(file);
        return lx(fd);
    }
    if (flags & LINUX_O_CLOEXEC) {
        handle_set_flags(me()->handles, fd, HANDLE_FLAG_CLOSE_ON_EXEC);
    }
    return fd;
}

static int64_t sys_open(struct interrupt_frame *f, uint64_t path, uint64_t flags, uint64_t mode,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    return do_openat(LINUX_AT_FDCWD, path, flags, mode);
}

static int64_t sys_openat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                          uint64_t flags, uint64_t mode, uint64_t a4, uint64_t a5) {
    return do_openat((int)dirfd, path, flags, mode);
}

static int64_t sys_creat(struct interrupt_frame *f, uint64_t path, uint64_t mode, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return do_openat(LINUX_AT_FDCWD, path, LINUX_O_CREAT | LINUX_O_WRONLY | LINUX_O_TRUNC, mode);
}

static int64_t sys_close(struct interrupt_frame *f, uint64_t fd, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return lx(handle_close(me()->handles, (int)fd));
}

static int64_t sys_close_range(struct interrupt_frame *f, uint64_t first, uint64_t last,
                               uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    for (uint64_t fd = first; fd <= last && fd < HANDLE_MAX; fd++) {
        if (flags & LINUX_CLOSE_RANGE_CLOEXEC) {
            handle_set_flags(me()->handles, (int)fd, HANDLE_FLAG_CLOSE_ON_EXEC);
        } else {
            handle_close(me()->handles, (int)fd);
        }
    }
    return 0;
}

static int64_t sys_lseek(struct interrupt_frame *f, uint64_t fd, uint64_t offset,
                         uint64_t whence, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct file *file = get_file(fd, 0, &error);
    if (!file) {
        return error;
    }
    int64_t result;
    if (vfs_is_terminal(file)) {
        result = -LE_ESPIPE;
    } else if (file->vnode->type == VX_TYPE_DIRECTORY) {
        /* Only rewinding (rewinddir) makes sense for a directory. */
        if (whence == 0 && offset == 0) {
            file->offset = 0;
            result = 0;
        } else {
            result = -LE_EINVAL;
        }
    } else {
        result = whence > 2 ? -LE_EINVAL : lx(vfs_seek(file, (int64_t)offset, (int)whence));
    }
    vfs_close(file);
    return result;
}

static int64_t dup_to(int64_t old, int64_t new, uint64_t flags) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)old, &rights);
    if (!object) {
        return -LE_EBADF;
    }
    if (new < 0 || new >= HANDLE_MAX) {
        object_put(object);
        return -LE_EBADF;
    }
    int fd = handle_set(me()->handles, (int)new, object, rights);
    if (fd >= 0) {
        handle_set_flags(me()->handles, fd,
                         flags & LINUX_O_CLOEXEC ? HANDLE_FLAG_CLOSE_ON_EXEC : 0);
    }
    return lx(fd);
}

static int64_t dup_from(int64_t old, int64_t min, bool close_on_exec) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)old, &rights);
    if (!object) {
        return -LE_EBADF;
    }
    if (min < 0 || min >= HANDLE_MAX) {
        object_put(object);
        return -LE_EINVAL;
    }
    int fd = handle_add_from(me()->handles, object, rights, (int)min);
    if (fd < 0) {
        object_put(object);
        return lx(fd);
    }
    if (close_on_exec) {
        handle_set_flags(me()->handles, fd, HANDLE_FLAG_CLOSE_ON_EXEC);
    }
    return fd;
}

static int64_t sys_dup(struct interrupt_frame *f, uint64_t fd, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    return dup_from(fd, 0, false);
}

static int64_t sys_dup2(struct interrupt_frame *f, uint64_t old, uint64_t new, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    if (old == new) {
        return handle_get_flags(me()->handles, (int)old) < 0 ? -LE_EBADF : (int64_t)new;
    }
    return dup_to(old, new, 0);
}

static int64_t sys_dup3(struct interrupt_frame *f, uint64_t old, uint64_t new, uint64_t flags,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    if (old == new || (flags & ~(uint64_t)LINUX_O_CLOEXEC)) {
        return -LE_EINVAL;
    }
    return dup_to(old, new, flags);
}

static void set_nonblocking(struct object *object, bool on) {
    if (on) {
        __atomic_or_fetch(&object->flags, OBJECT_NONBLOCK, __ATOMIC_RELAXED);
    } else {
        __atomic_and_fetch(&object->flags, ~(uint32_t)OBJECT_NONBLOCK, __ATOMIC_RELAXED);
    }
}

static int64_t sys_fcntl(struct interrupt_frame *f, uint64_t fd, uint64_t command, uint64_t arg,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    struct handle_table *handles = me()->handles;
    int flags = handle_get_flags(handles, (int)fd);
    if (flags < 0) {
        return -LE_EBADF;
    }
    switch (command) {
    case LINUX_F_DUPFD:
        return dup_from(fd, arg, false);
    case LINUX_F_DUPFD_CLOEXEC:
        return dup_from(fd, arg, true);
    case LINUX_F_GETFD:
        return flags & HANDLE_FLAG_CLOSE_ON_EXEC ? LINUX_FD_CLOEXEC : 0;
    case LINUX_F_SETFD:
        return lx(handle_set_flags(handles, (int)fd,
                                   arg & LINUX_FD_CLOEXEC ? HANDLE_FLAG_CLOSE_ON_EXEC : 0));
    case LINUX_F_GETFL: {
        uint32_t rights;
        struct object *object = handle_get_any(handles, (int)fd, &rights);
        if (!object) {
            return -LE_EBADF;
        }
        int64_t result = (rights & HANDLE_RIGHT_READ) && (rights & HANDLE_RIGHT_WRITE)
                             ? LINUX_O_RDWR
                         : rights & HANDLE_RIGHT_WRITE ? LINUX_O_WRONLY
                                                       : LINUX_O_RDONLY;
        if (object->type == &file_object_type &&
            (((struct file *)object)->flags & VX_OPEN_APPEND)) {
            result |= LINUX_O_APPEND;
        }
        if (object->flags & OBJECT_NONBLOCK) {
            result |= LINUX_O_NONBLOCK;
        }
        object_put(object);
        return result;
    }
    case LINUX_F_SETFL: {
        /* Only non-blocking mode can change (O_APPEND is set at open). */
        uint32_t rights;
        struct object *object = handle_get_any(handles, (int)fd, &rights);
        if (!object) {
            return -LE_EBADF;
        }
        set_nonblocking(object, arg & LINUX_O_NONBLOCK);
        object_put(object);
        return 0;
    }
    case LINUX_F_GETLK: {
        int16_t unlocked = LINUX_F_UNLCK; /* l_type is the first field of struct flock. */
        return copy_to_user(arg, &unlocked, sizeof(unlocked)) ? 0 : -LE_EFAULT;
    }
    case LINUX_F_SETLK:
    case LINUX_F_SETLKW:
    case LINUX_F_SETOWN:
    case LINUX_F_GETOWN:
        return 0; /* One user, no locks needed yet. */
    default:
        return -LE_EINVAL;
    }
}

static int64_t make_pipe(uint64_t out, uint64_t flags) {
    if (flags & ~(uint64_t)(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK)) {
        return -LE_EINVAL;
    }
    struct object *read_end, *write_end;
    int error = pipe_create(&read_end, &write_end);
    if (error) {
        return lx(error);
    }
    int fds[2];
    fds[0] = handle_add(me()->handles, read_end, HANDLE_RIGHT_READ);
    if (fds[0] < 0) {
        object_put(read_end);
        object_put(write_end);
        return lx(fds[0]);
    }
    fds[1] = handle_add(me()->handles, write_end, HANDLE_RIGHT_WRITE);
    if (fds[1] < 0) {
        handle_close(me()->handles, fds[0]);
        object_put(write_end);
        return lx(fds[1]);
    }
    if (flags & LINUX_O_CLOEXEC) {
        handle_set_flags(me()->handles, fds[0], HANDLE_FLAG_CLOSE_ON_EXEC);
        handle_set_flags(me()->handles, fds[1], HANDLE_FLAG_CLOSE_ON_EXEC);
    }
    if (flags & LINUX_O_NONBLOCK) {
        set_nonblocking(read_end, true);
        set_nonblocking(write_end, true);
    }
    if (!copy_to_user(out, fds, sizeof(fds))) {
        handle_close(me()->handles, fds[0]);
        handle_close(me()->handles, fds[1]);
        return -LE_EFAULT;
    }
    return 0;
}

static int64_t sys_pipe(struct interrupt_frame *f, uint64_t out, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    return make_pipe(out, 0);
}

static int64_t sys_pipe2(struct interrupt_frame *f, uint64_t out, uint64_t flags, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return make_pipe(out, flags);
}

/* ---- File information ---- */

static uint32_t mode_of(uint32_t type, uint32_t permissions) {
    switch (type) {
    case VX_TYPE_DIRECTORY: return LINUX_S_IFDIR | permissions;
    case VX_TYPE_CHAR_DEVICE: return LINUX_S_IFCHR | permissions;
    case VX_TYPE_BLOCK_DEVICE: return LINUX_S_IFBLK | permissions;
    case VX_TYPE_SYMLINK: return LINUX_S_IFLNK | permissions;
    default: return LINUX_S_IFREG | permissions;
    }
}

static void fill_stat(const struct vx_stat *vx, struct linux_stat *st) {
    memset(st, 0, sizeof(*st));
    st->dev = 1;
    st->ino = vx->inode;
    st->nlink = vx->links ? vx->links : 1;
    st->mode = mode_of(vx->type, vx->mode);
    st->size = (int64_t)vx->size;
    st->blksize = 4096;
    st->blocks = (int64_t)((vx->size + 511) / 512);
    st->atime = st->mtime = st->ctime = vx->modified;
    if (vx->type == VX_TYPE_CHAR_DEVICE) {
        st->rdev = vx->inode;
    }
}

static int64_t stat_path(int64_t dirfd, uint64_t user_path, uint64_t out, bool follow) {
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    struct vx_stat vx;
    error = follow ? vfs_stat(path, strlen(path), &vx) : vfs_lstat(path, strlen(path), &vx);
    kfree(path);
    if (error) {
        return lx(error);
    }
    struct linux_stat st;
    fill_stat(&vx, &st);
    return copy_to_user(out, &st, sizeof(st)) ? 0 : -LE_EFAULT;
}

static int64_t stat_fd(int64_t fd, uint64_t out) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)fd, &rights);
    if (!object) {
        return -LE_EBADF;
    }
    struct linux_stat st;
    if (object->type == &file_object_type) {
        struct vx_stat vx;
        vfs_file_stat((struct file *)object, &vx);
        fill_stat(&vx, &st);
    } else {
        memset(&st, 0, sizeof(st));
        st.dev = 2;
        st.nlink = 1;
        st.blksize = 4096;
        st.mode = LINUX_S_IFIFO | 0600; /* Pipes (the only other kind so far). */
    }
    object_put(object);
    return copy_to_user(out, &st, sizeof(st)) ? 0 : -LE_EFAULT;
}

static int64_t sys_stat(struct interrupt_frame *f, uint64_t path, uint64_t out, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    return stat_path(LINUX_AT_FDCWD, path, out, true);
}

static int64_t sys_lstat(struct interrupt_frame *f, uint64_t path, uint64_t out, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return stat_path(LINUX_AT_FDCWD, path, out, false);
}

static int64_t sys_fstat(struct interrupt_frame *f, uint64_t fd, uint64_t out, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return stat_fd(fd, out);
}

static int64_t sys_newfstatat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                              uint64_t out, uint64_t flags, uint64_t a4, uint64_t a5) {
    if (flags & LINUX_AT_EMPTY_PATH) {
        char first;
        if (copy_from_user(&first, path, 1) && first == '\0') {
            if ((int)dirfd != LINUX_AT_FDCWD) {
                return stat_fd(dirfd, out);
            }
            struct vx_stat vx;
            int64_t error = lx(vfs_stat(me()->cwd, strlen(me()->cwd), &vx));
            struct linux_stat st;
            fill_stat(&vx, &st);
            return error ? error : copy_to_user(out, &st, sizeof(st)) ? 0 : -LE_EFAULT;
        }
    }
    return stat_path((int)dirfd, path, out, !(flags & LINUX_AT_SYMLINK_NOFOLLOW));
}

#define LINUX_X_OK 1

static int64_t access_at(int64_t dirfd, uint64_t user_path, uint64_t mode) {
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    struct vx_stat vx;
    error = vfs_stat(path, strlen(path), &vx);
    kfree(path);
    if (error) {
        return lx(error);
    }
    /* Everyone is root: reading and writing are always allowed, running
     * needs an execute bit (or a directory). */
    if ((mode & LINUX_X_OK) && vx.type != VX_TYPE_DIRECTORY && !(vx.mode & 0111)) {
        return -LE_EACCES;
    }
    return 0;
}

static int64_t sys_access(struct interrupt_frame *f, uint64_t path, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return access_at(LINUX_AT_FDCWD, path, mode);
}

static int64_t sys_faccessat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                             uint64_t mode, uint64_t flags, uint64_t a4, uint64_t a5) {
    return access_at((int)dirfd, path, mode);
}

static int64_t chmod_at(int64_t dirfd, uint64_t user_path, uint64_t mode) {
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    error = lx(vfs_chmod(path, strlen(path), (uint32_t)mode & 07777));
    kfree(path);
    return error;
}

static int64_t sys_chmod(struct interrupt_frame *f, uint64_t path, uint64_t mode, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return chmod_at(LINUX_AT_FDCWD, path, mode);
}

static int64_t sys_fchmodat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                            uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5) {
    return chmod_at((int)dirfd, path, mode);
}

static int64_t sys_fchmod(struct interrupt_frame *f, uint64_t fd, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct file *file = get_file(fd, 0, &error);
    if (!file) {
        return error == -LE_ESPIPE ? 0 : error; /* Pipes have no mode to keep. */
    }
    error = lx(vfs_file_chmod(file, (uint32_t)mode & 07777));
    vfs_close(file);
    return error;
}

static int64_t readlink_at(int64_t dirfd, uint64_t user_path, uint64_t buffer, uint64_t size) {
    if ((int64_t)size <= 0) {
        return -LE_EINVAL;
    }
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    size_t max = size < VX_PATH_MAX ? size : VX_PATH_MAX;
    char *target = kmalloc(max);
    int64_t result = target ? lx(vfs_readlink(path, strlen(path), target, max)) : -LE_ENOMEM;
    if (result > 0 && !copy_to_user(buffer, target, result)) {
        result = -LE_EFAULT;
    }
    kfree(target);
    kfree(path);
    return result;
}

static int64_t sys_readlink(struct interrupt_frame *f, uint64_t path, uint64_t buffer,
                            uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5) {
    return readlink_at(LINUX_AT_FDCWD, path, buffer, size);
}

static int64_t sys_readlinkat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                              uint64_t buffer, uint64_t size, uint64_t a4, uint64_t a5) {
    return readlink_at((int)dirfd, path, buffer, size);
}

static int64_t symlink_at(uint64_t user_target, int64_t dirfd, uint64_t user_path) {
    char *target = kmalloc(VX_PATH_MAX + 1);
    if (!target) {
        return -LE_ENOMEM;
    }
    int64_t length = copy_string_from_user(target, user_target, VX_PATH_MAX + 1);
    if (length <= 0) {
        kfree(target);
        return length < 0 ? -LE_EFAULT : -LE_ENOENT;
    }
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (!error) {
        error = lx(vfs_symlink(target, path, strlen(path)));
        kfree(path);
    }
    kfree(target);
    return error;
}

static int64_t link_at(int64_t old_dirfd, uint64_t old_path, int64_t new_dirfd,
                       uint64_t new_path) {
    char *from, *to;
    int64_t error = path_at(old_dirfd, old_path, &from);
    if (error) {
        return error;
    }
    error = path_at(new_dirfd, new_path, &to);
    if (!error) {
        error = vfs_link(from, strlen(from), to, strlen(to));
        error = error == -VX_EACCES ? -LE_EPERM : lx(error);
        kfree(to);
    }
    kfree(from);
    return error;
}

static int64_t sys_link(struct interrupt_frame *f, uint64_t from, uint64_t to, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    return link_at(LINUX_AT_FDCWD, from, LINUX_AT_FDCWD, to);
}

static int64_t sys_linkat(struct interrupt_frame *f, uint64_t old_dirfd, uint64_t from,
                          uint64_t new_dirfd, uint64_t to, uint64_t flags, uint64_t a5) {
    return link_at((int)old_dirfd, from, (int)new_dirfd, to);
}

static int64_t sys_symlink(struct interrupt_frame *f, uint64_t target, uint64_t path,
                           uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return symlink_at(target, LINUX_AT_FDCWD, path);
}

static int64_t sys_symlinkat(struct interrupt_frame *f, uint64_t target, uint64_t dirfd,
                             uint64_t path, uint64_t a3, uint64_t a4, uint64_t a5) {
    return symlink_at(target, (int)dirfd, path);
}

static uint8_t dirent_type(uint32_t type) {
    switch (type) {
    case VX_TYPE_DIRECTORY: return LINUX_DT_DIR;
    case VX_TYPE_CHAR_DEVICE: return LINUX_DT_CHR;
    case VX_TYPE_BLOCK_DEVICE: return LINUX_DT_BLK;
    case VX_TYPE_SYMLINK: return LINUX_DT_LNK;
    default: return LINUX_DT_REG;
    }
}

static int64_t sys_getdents64(struct interrupt_frame *f, uint64_t fd, uint64_t out,
                              uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!user_range_ok(out, size)) {
        return -LE_EFAULT;
    }
    int64_t error;
    struct file *file = get_file(fd, HANDLE_RIGHT_READ, &error);
    if (!file) {
        return error == -LE_ESPIPE ? -LE_ENOTDIR : error;
    }
    if (file->vnode->type != VX_TYPE_DIRECTORY) {
        vfs_close(file);
        return -LE_ENOTDIR;
    }
    struct vx_dir_entry *entry = kmalloc(sizeof(*entry));
    uint8_t *record = kmalloc(sizeof(struct linux_dirent64) + VX_NAME_MAX + 8);
    int64_t written = entry && record ? 0 : -LE_ENOMEM;
    while (written >= 0) {
        uint64_t before = file->offset;
        int result = vfs_read_dir(file, entry);
        if (result <= 0) {
            if (result < 0 && written == 0) {
                written = lx(result);
            }
            break;
        }
        size_t name_length = strlen(entry->name);
        size_t length = (sizeof(struct linux_dirent64) + name_length + 1 + 7) & ~(size_t)7;
        if ((uint64_t)written + length > size) {
            file->offset = before; /* Doesn't fit: hand it out next time. */
            if (written == 0) {
                written = -LE_EINVAL;
            }
            break;
        }
        memset(record, 0, length);
        struct linux_dirent64 *d = (struct linux_dirent64 *)record;
        d->ino = entry->inode ? entry->inode : 1;
        d->off = (int64_t)file->offset;
        d->reclen = (uint16_t)length;
        d->type = dirent_type(entry->type);
        memcpy(record + sizeof(*d), entry->name, name_length);
        if (!copy_to_user(out + written, record, length)) {
            written = -LE_EFAULT;
            break;
        }
        written += length;
    }
    kfree(entry);
    kfree(record);
    vfs_close(file);
    return written;
}

/* struct statfs, from the file system holding `path`. */
static int64_t statfs_of(const char *path, uint64_t out) {
    uint64_t total, free;
    const char *fs;
    int error = vfs_statfs(path, strlen(path), &total, &free, &fs);
    if (error) {
        return lx(error);
    }
    uint64_t st[15] = {0};
    st[0] = strcmp(fs, "ext2") == 0    ? 0xef53     /* f_type: Linux's magic numbers */
            : strcmp(fs, "proc") == 0  ? 0x9fa0
            : strcmp(fs, "devfs") == 0 ? 0x1373
                                       : 0x01021994; /* tmpfs */
    st[1] = 4096;               /* f_bsize */
    st[2] = total / 4096;       /* f_blocks */
    st[3] = st[4] = free / 4096; /* f_bfree, f_bavail */
    st[5] = st[2];              /* f_files: no inode limit to speak of */
    st[6] = st[3];              /* f_ffree */
    st[9] = 255;                /* f_namelen */
    st[10] = 4096;              /* f_frsize */
    return copy_to_user(out, st, sizeof(st)) ? 0 : -LE_EFAULT;
}

static int64_t sys_statfs(struct interrupt_frame *f, uint64_t user_path, uint64_t out,
                          uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    char *path;
    int64_t error = path_at(LINUX_AT_FDCWD, user_path, &path);
    if (error) {
        return error;
    }
    error = statfs_of(path, out);
    kfree(path);
    return error;
}

static int64_t sys_fstatfs(struct interrupt_frame *f, uint64_t fd, uint64_t out, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct file *file = get_file(fd, 0, &error);
    if (!file) {
        return error == -LE_ESPIPE ? statfs_of("/dev", out) : error;
    }
    error = file->path ? statfs_of(file->path, out) : -LE_EINVAL;
    vfs_close(file);
    return error;
}

/* ---- Changing files and directories ---- */

static int64_t mkdir_at(int64_t dirfd, uint64_t user_path, uint64_t mode) {
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    error = lx(vfs_mkdir(path, strlen(path)));
    if (!error) {
        vfs_chmod(path, strlen(path), (uint32_t)mode & ~umask_now() & 07777);
    }
    kfree(path);
    return error;
}

static int64_t sys_mkdir(struct interrupt_frame *f, uint64_t path, uint64_t mode, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return mkdir_at(LINUX_AT_FDCWD, path, mode);
}

static int64_t sys_mkdirat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                           uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5) {
    return mkdir_at((int)dirfd, path, mode);
}

/* unlink refuses directories and rmdir refuses files, as on Linux. */
static int64_t remove_checked(int64_t dirfd, uint64_t user_path, bool directory) {
    char *path;
    int64_t error = path_at(dirfd, user_path, &path);
    if (error) {
        return error;
    }
    struct vx_stat vx;
    error = vfs_stat(path, strlen(path), &vx);
    if (!error && directory && vx.type != VX_TYPE_DIRECTORY) {
        error = -VX_ENOTDIR;
    } else if (!error && !directory && vx.type == VX_TYPE_DIRECTORY) {
        kfree(path);
        return -LE_EISDIR;
    }
    if (!error) {
        error = vfs_remove(path, strlen(path));
    }
    kfree(path);
    return lx(error);
}

static int64_t sys_unlink(struct interrupt_frame *f, uint64_t path, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return remove_checked(LINUX_AT_FDCWD, path, false);
}

static int64_t sys_rmdir(struct interrupt_frame *f, uint64_t path, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return remove_checked(LINUX_AT_FDCWD, path, true);
}

static int64_t sys_unlinkat(struct interrupt_frame *f, uint64_t dirfd, uint64_t path,
                            uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    return remove_checked((int)dirfd, path, flags & LINUX_AT_REMOVEDIR);
}

static int64_t rename_at(int64_t old_dirfd, uint64_t old_path, int64_t new_dirfd,
                         uint64_t new_path, uint64_t flags) {
    if (flags & ~(uint64_t)LINUX_RENAME_NOREPLACE) {
        return -LE_EINVAL;
    }
    char *from, *to;
    int64_t error = path_at(old_dirfd, old_path, &from);
    if (error) {
        return error;
    }
    error = path_at(new_dirfd, new_path, &to);
    if (error) {
        kfree(from);
        return error;
    }
    struct vx_stat vx;
    if ((flags & LINUX_RENAME_NOREPLACE) && vfs_stat(to, strlen(to), &vx) == 0) {
        error = -LE_EEXIST;
    } else {
        error = lx(vfs_rename(from, strlen(from), to, strlen(to)));
    }
    kfree(from);
    kfree(to);
    return error;
}

static int64_t sys_rename(struct interrupt_frame *f, uint64_t from, uint64_t to, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return rename_at(LINUX_AT_FDCWD, from, LINUX_AT_FDCWD, to, 0);
}

static int64_t sys_renameat(struct interrupt_frame *f, uint64_t old_dirfd, uint64_t from,
                            uint64_t new_dirfd, uint64_t to, uint64_t a4, uint64_t a5) {
    return rename_at((int)old_dirfd, from, (int)new_dirfd, to, 0);
}

static int64_t sys_renameat2(struct interrupt_frame *f, uint64_t old_dirfd, uint64_t from,
                             uint64_t new_dirfd, uint64_t to, uint64_t flags, uint64_t a5) {
    return rename_at((int)old_dirfd, from, (int)new_dirfd, to, flags);
}

static int64_t truncate_file(struct file *file, uint64_t size) {
    int64_t result = (int64_t)size < 0 ? -LE_EINVAL : lx(vfs_truncate(file, size));
    vfs_close(file);
    return result;
}

static int64_t sys_truncate(struct interrupt_frame *f, uint64_t user_path, uint64_t size,
                            uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    char *path;
    int64_t error = path_at(LINUX_AT_FDCWD, user_path, &path);
    if (error) {
        return error;
    }
    struct file *file;
    error = vfs_open(path, strlen(path), VX_OPEN_WRITE, &file);
    kfree(path);
    return error ? lx(error) : truncate_file(file, size);
}

static int64_t sys_ftruncate(struct interrupt_frame *f, uint64_t fd, uint64_t size, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct file *file = get_file(fd, HANDLE_RIGHT_WRITE, &error);
    return file ? truncate_file(file, size) : (error == -LE_ESPIPE ? -LE_EINVAL : error);
}

static int64_t sys_chdir(struct interrupt_frame *f, uint64_t user_path, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    char *path;
    int64_t error = path_at(LINUX_AT_FDCWD, user_path, &path);
    if (error) {
        return error;
    }
    struct vx_stat vx;
    error = vfs_stat(path, strlen(path), &vx);
    if (!error && vx.type != VX_TYPE_DIRECTORY) {
        error = -VX_ENOTDIR;
    }
    if (error) {
        kfree(path);
        return lx(error);
    }
    kfree(me()->cwd);
    me()->cwd = path;
    return 0;
}

static int64_t sys_fchdir(struct interrupt_frame *f, uint64_t fd, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct file *file = get_file(fd, 0, &error);
    if (!file) {
        return error == -LE_ESPIPE ? -LE_ENOTDIR : error;
    }
    char *path = NULL;
    if (file->vnode->type == VX_TYPE_DIRECTORY && file->path) {
        path = kmalloc(strlen(file->path) + 1);
        if (path) {
            strcpy(path, file->path);
        }
    }
    error = file->vnode->type != VX_TYPE_DIRECTORY ? -LE_ENOTDIR : path ? 0 : -LE_ENOMEM;
    vfs_close(file);
    if (!error) {
        kfree(me()->cwd);
        me()->cwd = path;
    }
    return error;
}

static int64_t sys_getcwd(struct interrupt_frame *f, uint64_t buffer, uint64_t size, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    /* Inside /linux, show the path the program would use: /usr, not /linux/usr. */
    const char *cwd = me()->cwd;
    if (under_linux_root(cwd) && cwd[LINUX_ROOT_LENGTH] == '/') {
        cwd += LINUX_ROOT_LENGTH;
    }
    size_t length = strlen(cwd) + 1;
    if (size < length) {
        return -LE_ERANGE;
    }
    return copy_to_user(buffer, cwd, length) ? (int64_t)length : -LE_EFAULT;
}

static uint32_t umask_now(void) {
    return me()->umask;
}

static int64_t sys_umask(struct interrupt_frame *f, uint64_t mask, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    uint32_t old = me()->umask;
    me()->umask = mask & 0777;
    return old;
}

/* Owners, permissions and times aren't stored yet; accept changes quietly. */
static int64_t sys_accept_quietly(struct interrupt_frame *f, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return 0;
}

static int64_t sys_not_permitted(struct interrupt_frame *f, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return -LE_EPERM;
}

static int64_t sys_no_such_call_quietly(struct interrupt_frame *f, uint64_t a0, uint64_t a1,
                                        uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return -LE_ENOSYS; /* Programs are known to fall back to something else. */
}

/* ---- The terminal ---- */

static int64_t sys_ioctl(struct interrupt_frame *f, uint64_t fd, uint64_t request, uint64_t arg,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (request == LINUX_FIOCLEX || request == LINUX_FIONCLEX) {
        return lx(handle_set_flags(me()->handles, (int)fd,
                                   request == LINUX_FIOCLEX ? HANDLE_FLAG_CLOSE_ON_EXEC : 0));
    }
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)fd, &rights);
    if (!object) {
        return -LE_EBADF;
    }
    if (request == LINUX_FIONBIO) {
        int on;
        bool ok = copy_from_user(&on, arg, sizeof(on));
        if (ok) {
            set_nonblocking(object, on != 0);
        }
        object_put(object);
        return ok ? 0 : -LE_EFAULT;
    }
    if (socket_of(object)) {
        int64_t result = linux_socket_ioctl(object, request, arg);
        object_put(object);
        return result;
    }
    if (request == LINUX_FIONREAD && object->type == &pipe_read_type) {
        int ready = (object->type->poll(object) & OBJECT_READABLE) ? 1 : 0;
        object_put(object);
        return copy_to_user(arg, &ready, sizeof(ready)) ? 0 : -LE_EFAULT;
    }
    bool terminal = is_terminal(object);
    object_put(object);
    if (!terminal) {
        return -LE_ENOTTY;
    }
    switch (request) {
    case LINUX_TCGETS: {
        struct tty_settings settings;
        tty_get_settings(&settings);
        struct linux_termios t = {settings.iflag, settings.oflag, settings.cflag,
                                  settings.lflag, 0, {0}};
        memcpy(t.cc, settings.cc, sizeof(t.cc));
        return copy_to_user(arg, &t, sizeof(t)) ? 0 : -LE_EFAULT;
    }
    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF: {
        struct linux_termios t;
        if (!copy_from_user(&t, arg, sizeof(t))) {
            return -LE_EFAULT;
        }
        struct tty_settings settings;
        tty_get_settings(&settings);
        settings.iflag = t.iflag;
        settings.oflag = t.oflag;
        settings.cflag = t.cflag;
        settings.lflag = t.lflag;
        memcpy(settings.cc, t.cc, sizeof(t.cc));
        tty_set_settings(&settings);
        return 0;
    }
    case LINUX_TIOCGWINSZ: {
        uint16_t size[4] = {0};
        tty_window_size(&size[0], &size[1]);
        return copy_to_user(arg, size, sizeof(size)) ? 0 : -LE_EFAULT;
    }
    case LINUX_TIOCGPGRP: {
        int group = (int)tty_foreground();
        return copy_to_user(arg, &group, sizeof(group)) ? 0 : -LE_EFAULT;
    }
    case LINUX_TIOCSPGRP: {
        int group;
        if (!copy_from_user(&group, arg, sizeof(group))) {
            return -LE_EFAULT;
        }
        tty_set_foreground((uint32_t)group);
        return 0;
    }
    case LINUX_TIOCGSID: {
        int session = 1;
        return copy_to_user(arg, &session, sizeof(session)) ? 0 : -LE_EFAULT;
    }
    case LINUX_FIONREAD: {
        int ready = (int)tty_bytes_ready();
        return copy_to_user(arg, &ready, sizeof(ready)) ? 0 : -LE_EFAULT;
    }
    case LINUX_TIOCSWINSZ:
    case LINUX_TIOCSCTTY:
    case LINUX_TIOCNOTTY:
    case LINUX_FIONBIO:
    case LINUX_TCFLSH:
    case LINUX_TCXONC:
    case LINUX_TCSBRK:
        return 0;
    default:
        return -LE_EINVAL;
    }
}

/* ---- Waiting for input: poll and select ----
 * Only the terminal can say whether input is waiting; files are always
 * ready, and pipes are reported ready (a read then waits for data). */

static int16_t poll_events(int fd, int16_t wanted) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, fd, &rights);
    if (!object) {
        return LINUX_POLLNVAL;
    }
    /* The OBJECT_* bits have Linux's poll values. */
    int16_t ready = (int16_t)(object->type->poll ? object->type->poll(object)
                                                 : OBJECT_READABLE | OBJECT_WRITABLE);
    object_put(object);
    return ready & (wanted | LINUX_POLLERR | LINUX_POLLHUP);
}

/* Waits up to timeout_ms (negative: forever) for check() to find something. */
static int64_t wait_until_ready(int64_t (*check)(void *), void *arg, int64_t timeout_ms) {
    uint64_t start = timer_ms();
    for (;;) {
        int64_t found = check(arg);
        if (found != 0) {
            return found;
        }
        uint64_t waited = timer_ms() - start;
        if (timeout_ms >= 0 && waited >= (uint64_t)timeout_ms) {
            return 0;
        }
        uint64_t step = 10;
        if (timeout_ms >= 0 && (uint64_t)timeout_ms - waited < step) {
            step = (uint64_t)timeout_ms - waited;
        }
        if (thread_sleep_ms_interruptible(step)) {
            return -LE_EINTR;
        }
    }
}

struct poll_args {
    struct linux_pollfd *fds;
    uint64_t count;
};

static int64_t poll_check(void *arg) {
    struct poll_args *p = arg;
    int64_t ready = 0;
    for (uint64_t i = 0; i < p->count; i++) {
        p->fds[i].revents = p->fds[i].fd < 0 ? 0 : poll_events(p->fds[i].fd, p->fds[i].events);
        ready += p->fds[i].revents != 0;
    }
    return ready;
}

static int64_t do_poll(uint64_t user_fds, uint64_t count, int64_t timeout_ms) {
    if (count > HANDLE_MAX) {
        return -LE_EINVAL;
    }
    struct poll_args p = {kmalloc(count * sizeof(struct linux_pollfd) + 1), count};
    if (!p.fds) {
        return -LE_ENOMEM;
    }
    int64_t result = -LE_EFAULT;
    if (copy_from_user(p.fds, user_fds, count * sizeof(struct linux_pollfd))) {
        result = wait_until_ready(poll_check, &p, timeout_ms);
        if (result >= 0 &&
            !copy_to_user(user_fds, p.fds, count * sizeof(struct linux_pollfd))) {
            result = -LE_EFAULT;
        }
    }
    kfree(p.fds);
    return result;
}

static int64_t timespec_ms(uint64_t user_timespec, int64_t *ms) {
    if (!user_timespec) {
        *ms = -1;
        return 0;
    }
    struct linux_timespec ts;
    if (!copy_from_user(&ts, user_timespec, sizeof(ts))) {
        return -LE_EFAULT;
    }
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) {
        return -LE_EINVAL;
    }
    *ms = ts.sec * 1000 + (ts.nsec + 999999) / 1000000;
    return 0;
}

static int64_t sys_poll(struct interrupt_frame *f, uint64_t fds, uint64_t count,
                        uint64_t timeout, uint64_t a3, uint64_t a4, uint64_t a5) {
    return do_poll(fds, count, (int)timeout);
}

static int64_t sys_ppoll(struct interrupt_frame *f, uint64_t fds, uint64_t count,
                         uint64_t timeout, uint64_t mask, uint64_t a4, uint64_t a5) {
    int64_t ms;
    int64_t error = timespec_ms(timeout, &ms);
    return error ? error : do_poll(fds, count, ms);
}

struct select_args {
    uint64_t sets[3][HANDLE_MAX / 64]; /* What was asked: read, write, except. */
    uint64_t found[3][HANDLE_MAX / 64];
    int count;
};

static int64_t select_check(void *arg) {
    struct select_args *s = arg;
    int64_t ready = 0;
    memset(s->found, 0, sizeof(s->found));
    for (int fd = 0; fd < s->count; fd++) {
        uint64_t bit = 1ULL << (fd % 64);
        int word = fd / 64;
        int16_t wanted = (s->sets[0][word] & bit ? LINUX_POLLIN : 0) |
                         (s->sets[1][word] & bit ? LINUX_POLLOUT : 0);
        if (!wanted && !(s->sets[2][word] & bit)) {
            continue;
        }
        int16_t events = poll_events(fd, wanted);
        if (events & LINUX_POLLNVAL) {
            return -LE_EBADF;
        }
        if (events & LINUX_POLLIN) {
            s->found[0][word] |= bit;
            ready++;
        }
        if (events & LINUX_POLLOUT) {
            s->found[1][word] |= bit;
            ready++;
        }
    }
    return ready;
}

static int64_t do_select(uint64_t count, uint64_t sets[3], int64_t timeout_ms) {
    if (count > HANDLE_MAX) {
        count = HANDLE_MAX;
    }
    struct select_args *s = kzalloc(sizeof(*s));
    if (!s) {
        return -LE_ENOMEM;
    }
    s->count = (int)count;
    size_t bytes = (count + 7) / 8;
    int64_t result = 0;
    for (int i = 0; i < 3; i++) {
        if (sets[i] && !copy_from_user(s->sets[i], sets[i], bytes)) {
            result = -LE_EFAULT;
        }
    }
    if (!result) {
        result = wait_until_ready(select_check, s, timeout_ms);
    }
    for (int i = 0; i < 3 && result >= 0; i++) {
        if (sets[i] && !copy_to_user(sets[i], s->found[i], bytes)) {
            result = -LE_EFAULT;
        }
    }
    kfree(s);
    return result;
}

static int64_t sys_select(struct interrupt_frame *f, uint64_t count, uint64_t readfds,
                          uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t a5) {
    int64_t ms = -1;
    if (timeout) {
        struct linux_timeval tv;
        if (!copy_from_user(&tv, timeout, sizeof(tv))) {
            return -LE_EFAULT;
        }
        ms = tv.sec * 1000 + (tv.usec + 999) / 1000;
    }
    uint64_t sets[3] = {readfds, writefds, exceptfds};
    return do_select(count, sets, ms);
}

static int64_t sys_pselect6(struct interrupt_frame *f, uint64_t count, uint64_t readfds,
                            uint64_t writefds, uint64_t exceptfds, uint64_t timeout,
                            uint64_t mask) {
    int64_t ms;
    int64_t error = timespec_ms(timeout, &ms);
    if (error) {
        return error;
    }
    uint64_t sets[3] = {readfds, writefds, exceptfds};
    return do_select(count, sets, ms);
}

/* ---- epoll ---- */

struct epoll_item {
    int fd;
    uint32_t events;
    uint64_t data;
};

struct epoll {
    struct object object;
    struct spinlock lock;
    struct epoll_item *items;
    size_t count, capacity;
};

struct linux_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

static void epoll_destroy(struct object *object) {
    struct epoll *ep = (struct epoll *)object;
    kfree(ep->items);
    kfree(ep);
}

static const struct object_type epoll_type = {
    .name = "epoll",
    .destroy = epoll_destroy,
};

static int64_t sys_epoll_create1(struct interrupt_frame *f, uint64_t flags, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct epoll *ep = kzalloc(sizeof(*ep));
    if (!ep) {
        return -LE_ENOMEM;
    }
    object_init(&ep->object, &epoll_type);
    int fd = handle_add(me()->handles, &ep->object, HANDLE_RIGHT_READ);
    if (fd < 0) {
        object_put(&ep->object);
        return lx(fd);
    }
    if (flags & LINUX_O_CLOEXEC) {
        handle_set_flags(me()->handles, fd, HANDLE_FLAG_CLOSE_ON_EXEC);
    }
    return fd;
}

static int64_t sys_epoll_create(struct interrupt_frame *f, uint64_t size, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return (int64_t)size <= 0 ? -LE_EINVAL : sys_epoll_create1(f, 0, 0, 0, 0, 0, 0);
}

static struct epoll *get_epoll(int64_t fd) {
    int error;
    return (struct epoll *)handle_get(me()->handles, (int)fd, &epoll_type, 0, &error);
}

static int64_t sys_epoll_ctl(struct interrupt_frame *f, uint64_t epfd, uint64_t op, uint64_t fd,
                             uint64_t user_event, uint64_t a4, uint64_t a5) {
    struct linux_epoll_event event = {0, 0};
    if (op != LINUX_EPOLL_CTL_DEL && !copy_from_user(&event, user_event, sizeof(event))) {
        return -LE_EFAULT;
    }
    uint32_t rights;
    struct object *target = handle_get_any(me()->handles, (int)fd, &rights);
    if (!target) {
        return -LE_EBADF;
    }
    bool is_epoll = target->type == &epoll_type;
    object_put(target);
    struct epoll *ep = get_epoll(epfd);
    if (!ep) {
        return -LE_EBADF;
    }
    if (is_epoll || (int)fd == (int)epfd) {
        object_put(&ep->object);
        return -LE_EINVAL; /* Nesting epoll sets isn't supported. */
    }
    int64_t result = 0;
    struct epoll_item *grown = NULL;
    if (op == LINUX_EPOLL_CTL_ADD && ep->count == ep->capacity) {
        grown = kmalloc((ep->capacity ? ep->capacity * 2 : 8) * sizeof(struct epoll_item));
        if (!grown) {
            object_put(&ep->object);
            return -LE_ENOMEM;
        }
    }
    uint64_t flags = spin_lock_irqsave(&ep->lock);
    size_t at = 0;
    while (at < ep->count && ep->items[at].fd != (int)fd) {
        at++;
    }
    bool present = at < ep->count;
    switch (op) {
    case LINUX_EPOLL_CTL_ADD:
        if (present) {
            result = -LE_EEXIST;
            break;
        }
        if (grown) {
            memcpy(grown, ep->items, ep->count * sizeof(struct epoll_item));
            struct epoll_item *old = ep->items;
            ep->items = grown;
            ep->capacity = ep->capacity ? ep->capacity * 2 : 8;
            grown = old; /* Freed below, outside the lock. */
        }
        ep->items[ep->count++] = (struct epoll_item){(int)fd, event.events, event.data};
        break;
    case LINUX_EPOLL_CTL_MOD:
        if (!present) {
            result = -LE_ENOENT;
            break;
        }
        ep->items[at].events = event.events;
        ep->items[at].data = event.data;
        break;
    case LINUX_EPOLL_CTL_DEL:
        if (!present) {
            result = -LE_ENOENT;
            break;
        }
        ep->items[at] = ep->items[--ep->count];
        break;
    default:
        result = -LE_EINVAL;
    }
    spin_unlock_irqrestore(&ep->lock, flags);
    kfree(grown);
    object_put(&ep->object);
    return result;
}

struct epoll_wait_args {
    struct epoll *ep;
    struct linux_epoll_event *out;
    int max;
};

static int64_t epoll_check(void *arg) {
    struct epoll_wait_args *w = arg;
    struct epoll *ep = w->ep;
    /* Copy the list, then look at each descriptor without the lock held. */
    uint64_t flags = spin_lock_irqsave(&ep->lock);
    size_t count = ep->count;
    struct epoll_item snapshot[64];
    count = count > 64 ? 64 : count;
    memcpy(snapshot, ep->items, count * sizeof(struct epoll_item));
    spin_unlock_irqrestore(&ep->lock, flags);
    int found = 0;
    for (size_t i = 0; i < count && found < w->max; i++) {
        uint32_t wanted = snapshot[i].events;
        if (!(wanted & (LINUX_POLLIN | LINUX_POLLOUT | LINUX_POLLERR | LINUX_POLLHUP)) &&
            !(wanted & LINUX_EPOLLRDHUP)) {
            continue; /* Disabled (a one-shot that already fired). */
        }
        int16_t ready = poll_events(snapshot[i].fd, (int16_t)(wanted & 0xffff));
        if (ready & LINUX_POLLNVAL) {
            continue; /* Closed: Linux drops it from the set. */
        }
        if (ready) {
            w->out[found].events = (uint32_t)(uint16_t)ready;
            w->out[found].data = snapshot[i].data;
            found++;
            if (wanted & LINUX_EPOLLONESHOT) {
                flags = spin_lock_irqsave(&ep->lock);
                for (size_t j = 0; j < ep->count; j++) {
                    if (ep->items[j].fd == snapshot[i].fd) {
                        ep->items[j].events &= LINUX_EPOLLONESHOT | LINUX_EPOLLET;
                    }
                }
                spin_unlock_irqrestore(&ep->lock, flags);
            }
        }
    }
    return found;
}

static int64_t epoll_wait_ms(uint64_t epfd, uint64_t user_events, uint64_t max,
                             int64_t timeout_ms) {
    if ((int64_t)max <= 0 || max > 1024) {
        return -LE_EINVAL;
    }
    struct epoll_wait_args w = {get_epoll(epfd), kmalloc(max * sizeof(struct linux_epoll_event)),
                                (int)max};
    if (!w.ep || !w.out) {
        if (w.ep) {
            object_put(&w.ep->object);
        }
        kfree(w.out);
        return w.ep ? -LE_ENOMEM : -LE_EBADF;
    }
    int64_t found = wait_until_ready(epoll_check, &w, timeout_ms);
    if (found > 0 && !copy_to_user(user_events, w.out, found * sizeof(struct linux_epoll_event))) {
        found = -LE_EFAULT;
    }
    object_put(&w.ep->object);
    kfree(w.out);
    return found;
}

static int64_t sys_epoll_wait(struct interrupt_frame *f, uint64_t epfd, uint64_t events,
                              uint64_t max, uint64_t timeout, uint64_t a4, uint64_t a5) {
    return epoll_wait_ms(epfd, events, max, (int)timeout);
}

static int64_t sys_epoll_pwait2(struct interrupt_frame *f, uint64_t epfd, uint64_t events,
                                uint64_t max, uint64_t timeout, uint64_t mask, uint64_t a5) {
    int64_t ms;
    int64_t error = timespec_ms(timeout, &ms);
    return error ? error : epoll_wait_ms(epfd, events, max, ms);
}

/* ---- Memory ---- */

static int64_t sys_brk(struct interrupt_frame *f, uint64_t end, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    return (int64_t)vm_set_heap_end(me()->address_space, end);
}

static unsigned vm_flags_of(uint64_t prot) {
    return (prot & LINUX_PROT_WRITE ? VM_WRITE : 0) | (prot & LINUX_PROT_EXEC ? VM_EXEC : 0);
}

#define SHARED_MAP_MAX (256ULL * 1024 * 1024)

static int64_t sys_mmap(struct interrupt_frame *f, uint64_t address, uint64_t length,
                        uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
    if (length == 0 || length > USER_END || offset % PAGE_SIZE) {
        return -LE_EINVAL;
    }
    uint64_t size = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    bool anonymous = flags & LINUX_MAP_ANONYMOUS;
    struct file *file = NULL;
    if (!anonymous) {
        int64_t error;
        file = get_file(fd, HANDLE_RIGHT_READ, &error);
        if (!file) {
            return error == -LE_ESPIPE ? -LE_ENODEV : error;
        }
    }
    struct address_space *as = me()->address_space;
    /* Shared mappings of anonymous memory or of files that can lend their
     * pages (tmpfs, so /dev/shm) really share; other file mappings get a
     * copy of the file, so the area starts out writable. */
    bool shared = (flags & LINUX_MAP_SHARED) &&
                  (anonymous || file->vnode->ops->share_page);
    if (shared && size > SHARED_MAP_MAX) {
        if (file) {
            vfs_close(file);
        }
        return -LE_ENOMEM; /* Shared memory is filled in up front. */
    }
    unsigned vm_flags = shared ? vm_flags_of(prot) | VM_SHARED
                               : vm_flags_of(prot) | (file ? VM_WRITE : 0);
    int64_t result;
    if (flags & LINUX_MAP_FIXED) {
        result = address % PAGE_SIZE || address < USER_BASE || address + size > USER_END
                     ? -LE_EINVAL
                     : lx(vm_map_fixed(as, address, size, vm_flags));
        if (result == 0) {
            result = (int64_t)address;
        }
    } else {
        uint64_t mapped = vm_map(as, size, vm_flags);
        result = mapped ? (int64_t)mapped : -LE_ENOMEM;
    }
    if (shared && result > 0) {
        size_t count = size / PAGE_SIZE;
        uint64_t *pages = file ? kzalloc(count * sizeof(uint64_t)) : NULL;
        for (size_t i = 0; pages && i < count; i++) {
            pages[i] = vfs_share_page(file, offset / PAGE_SIZE + i);
        }
        if (file && !pages) {
            vm_unmap(as, (uint64_t)result, size);
            result = -LE_ENOMEM;
        } else if (vm_populate_shared(as, (uint64_t)result, count, pages)) {
            vm_unmap(as, (uint64_t)result, size);
            result = -LE_ENOMEM;
        }
        kfree(pages);
    } else if (file && result > 0) {
        /* Private file mappings get a copy of the file; changes to shared ones
         * of other files aren't written back yet. */
        uint8_t *chunk = kmalloc(IO_CHUNK);
        for (uint64_t done = 0; chunk && done < length; done += IO_CHUNK) {
            uint64_t want = length - done < IO_CHUNK ? length - done : IO_CHUNK;
            int64_t n = vfs_pread(file, chunk, want, offset + done);
            if (n <= 0 || !vm_write(as, (uint64_t)result + done, chunk, n) ||
                (uint64_t)n < want) {
                break;
            }
        }
        kfree(chunk);
        if (!(prot & LINUX_PROT_WRITE)) {
            vm_protect(as, (uint64_t)result, size, vm_flags_of(prot));
        }
    }
    if (file) {
        vfs_close(file);
    }
    return result;
}

static int64_t sys_munmap(struct interrupt_frame *f, uint64_t address, uint64_t length,
                          uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (address % PAGE_SIZE || length == 0) {
        return -LE_EINVAL;
    }
    return lx(vm_unmap(me()->address_space, address, length));
}

static int64_t sys_mprotect(struct interrupt_frame *f, uint64_t address, uint64_t length,
                            uint64_t prot, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (address % PAGE_SIZE) {
        return -LE_EINVAL;
    }
    return lx(vm_protect(me()->address_space, address, length, vm_flags_of(prot)));
}

static int64_t sys_mremap(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return -LE_ENOMEM; /* musl's realloc then allocates, copies and frees. */
}

/* ---- Processes ---- */

static int64_t sys_exit(struct interrupt_frame *f, uint64_t code, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    process_exit((int)(code & 0xff));
}

static int64_t sys_getpid(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return me()->id;
}

static int64_t sys_getppid(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    struct process *parent = me()->parent;
    return parent ? parent->id : 1;
}

static int64_t sys_zero(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    return 0; /* getuid and friends: everyone is root, for now. */
}

static int64_t sys_getresid(struct interrupt_frame *f, uint64_t r, uint64_t e, uint64_t s,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    uint32_t zero = 0;
    return copy_to_user(r, &zero, 4) && copy_to_user(e, &zero, 4) && copy_to_user(s, &zero, 4)
               ? 0
               : -LE_EFAULT;
}

static int64_t sys_getpgid(struct interrupt_frame *f, uint64_t pid, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    if (pid == 0 || pid == me()->id) {
        return me()->group;
    }
    struct process *target = process_find((uint32_t)pid);
    if (!target) {
        return -LE_ESRCH;
    }
    int64_t group = target->group;
    object_put(&target->object);
    return group;
}

static int64_t sys_getpgrp(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    return me()->group;
}

static int64_t sys_setpgid(struct interrupt_frame *f, uint64_t pid, uint64_t group, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    struct process *target = pid == 0 ? me() : process_find((uint32_t)pid);
    if (!target) {
        return -LE_ESRCH;
    }
    int64_t result = 0;
    if (target != me() && target->parent != me()) {
        result = -LE_ESRCH;
    } else {
        target->group = group ? (uint32_t)group : target->id;
    }
    if (pid != 0) {
        object_put(&target->object);
    }
    return result;
}

static int64_t sys_setsid(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    me()->group = me()->id;
    return me()->id;
}

static int64_t sys_getsid(struct interrupt_frame *f, uint64_t pid, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return 1; /* One session: the console. */
}

static int64_t sys_fork(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    int error;
    struct process *child = process_fork(f, &error);
    if (!child) {
        return lx(error);
    }
    int64_t id = child->id;
    object_put(&child->object);
    return id;
}

static int64_t sys_clone(struct interrupt_frame *f, uint64_t flags, uint64_t stack,
                         uint64_t parent_tid, uint64_t child_tid, uint64_t tls, uint64_t a5) {
    /* The new thread or process resumes here too, with rax 0 and, if one is
     * given, its own stack. */
    struct interrupt_frame start = *f;
    start.rax = 0;
    if (stack) {
        start.rsp = stack;
    }
    if (flags & LINUX_CLONE_THREAD) {
        /* A thread: pthread_create. It must share everything that threads share. */
        const uint64_t shared = LINUX_CLONE_VM | LINUX_CLONE_SIGHAND | LINUX_CLONE_FILES |
                                LINUX_CLONE_FS;
        if ((flags & shared) != shared || !stack) {
            return -LE_EINVAL;
        }
        uint64_t fs_base = flags & LINUX_CLONE_SETTLS ? tls : rdmsr(IA32_FS_BASE_MSR);
        if (fs_base >= USER_END) {
            return -LE_EPERM;
        }
        const uint64_t tid_out[2] = {
            flags & LINUX_CLONE_PARENT_SETTID ? parent_tid : 0,
            flags & LINUX_CLONE_CHILD_SETTID ? child_tid : 0,
        };
        int tid = process_thread_create(&start, fs_base, true,
                                        flags & LINUX_CLONE_CHILD_CLEARTID ? child_tid : 0,
                                        tid_out);
        return tid < 0 ? lx(tid) : tid;
    }
    if (flags & LINUX_CLONE_VM) {
        /* A process sharing our memory until it execs (CLONE_VM|CLONE_VFORK,
         * as posix_spawn does). A copy-on-write copy behaves the same for a
         * child that only sets up and execs. */
        if (!(flags & LINUX_CLONE_VFORK)) {
            return -LE_ENOSYS;
        }
    }
    int error;
    struct process *child = process_fork(&start, &error);
    if (!child) {
        return lx(error);
    }
    int64_t id = child->id;
    if (flags & LINUX_CLONE_PARENT_SETTID) {
        uint32_t value = (uint32_t)id;
        copy_to_user(parent_tid, &value, sizeof(value));
    }
    object_put(&child->object);
    return id;
}

static void free_strings(char **strings) {
    if (strings) {
        for (char **s = strings; *s; s++) {
            kfree(*s);
        }
        kfree(strings);
    }
}

/* Copies a NULL-terminated array of user strings. */
static int64_t copy_string_array(uint64_t array, char ***out, size_t *count, size_t *budget) {
    char **strings = kzalloc((MAX_EXEC_STRINGS + 1) * sizeof(char *));
    char *buffer = kmalloc(VX_PATH_MAX);
    int64_t error = strings && buffer ? 0 : -LE_ENOMEM;
    size_t n = 0;
    while (!error && array) {
        uint64_t pointer;
        if (!copy_from_user(&pointer, array + n * sizeof(uint64_t), sizeof(pointer))) {
            error = -LE_EFAULT;
            break;
        }
        if (!pointer) {
            break;
        }
        if (n == MAX_EXEC_STRINGS) {
            error = -LE_E2BIG;
            break;
        }
        int64_t length = copy_string_from_user(buffer, pointer, VX_PATH_MAX);
        if (length < 0 || (size_t)length + 1 > *budget) {
            error = length < 0 ? -LE_EFAULT : -LE_E2BIG;
            break;
        }
        *budget -= length + 1;
        strings[n] = kmalloc(length + 1);
        if (!strings[n]) {
            error = -LE_ENOMEM;
            break;
        }
        memcpy(strings[n++], buffer, length + 1);
    }
    kfree(buffer);
    if (error) {
        free_strings(strings);
        return error;
    }
    *out = strings;
    *count = n;
    return 0;
}

static int64_t sys_execve(struct interrupt_frame *f, uint64_t user_path, uint64_t argv,
                          uint64_t envp, uint64_t a3, uint64_t a4, uint64_t a5) {
    char *path;
    int64_t error = path_at(LINUX_AT_FDCWD, user_path, &path);
    if (error) {
        return error;
    }
    struct vx_stat vx;
    if (vfs_stat(path, strlen(path), &vx) == 0 && vx.type == VX_TYPE_FILE && !(vx.mode & 0111)) {
        kfree(path);
        return -LE_EACCES; /* Not marked executable. */
    }
    size_t budget = MAX_EXEC_BYTES, argc = 0, envc = 0;
    char **args = NULL, **env = NULL;
    error = copy_string_array(argv, &args, &argc, &budget);
    if (!error) {
        error = copy_string_array(envp, &env, &envc, &budget);
    }
    if (!error) {
        const char *reason;
        error = lx(process_exec(path, args, argc, env, envc, f, &reason));
    }
    free_strings(args);
    free_strings(env);
    kfree(path);
    return error;
}

static int64_t sys_wait4(struct interrupt_frame *f, uint64_t pid, uint64_t status_out,
                         uint64_t options, uint64_t rusage, uint64_t a4, uint64_t a5) {
    int64_t id = (int32_t)pid;
    int code, signal;
    int result = process_wait_child(me(), id > 0 ? (uint32_t)id : 0, options & LINUX_WNOHANG,
                                    &code, &signal);
    if (result <= 0) {
        return lx(result);
    }
    int status = signal ? (signal & 0x7f) : (code & 0xff) << 8;
    if (status_out && !copy_to_user(status_out, &status, sizeof(status))) {
        return -LE_EFAULT;
    }
    if (rusage) {
        uint8_t zero[144] = {0};
        copy_to_user(rusage, zero, sizeof(zero));
    }
    return result;
}

static int64_t send_signal(int64_t pid, uint64_t signal) {
    if (signal > VX_SIGNAL_COUNT) {
        return -LE_EINVAL;
    }
    if (pid > 0) {
        struct process *target = process_find((uint32_t)pid);
        if (!target) {
            return -LE_ESRCH;
        }
        if (signal) {
            signal_send(target, (int)signal);
        }
        object_put(&target->object);
        return 0;
    }
    uint32_t group = pid == 0 ? me()->group : (uint32_t)-pid;
    if (pid == -1) {
        return -LE_EPERM; /* Signalling every process isn't allowed. */
    }
    if (!signal) {
        return 0;
    }
    return signal_send_group(group, (int)signal) ? 0 : -LE_ESRCH;
}

static int64_t sys_kill(struct interrupt_frame *f, uint64_t pid, uint64_t signal, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    return send_signal((int32_t)pid, signal);
}

/* A signal for one thread: in process `tgid`, or (0) wherever it is. */
static int64_t signal_thread(uint32_t tgid, uint32_t tid, uint64_t signal) {
    if ((int32_t)tid <= 0 || signal > VX_SIGNAL_COUNT) {
        return -LE_EINVAL;
    }
    if ((!tgid || tgid == me()->id) && process_signal_thread(me(), tid, (int)signal)) {
        return 0;
    }
    /* Another process: look it up by its id (its first thread has the same). */
    struct process *target = process_find(tgid ? tgid : tid);
    bool found = target && process_signal_thread(target, tid, (int)signal);
    if (target) {
        object_put(&target->object);
    }
    return found ? 0 : -LE_ESRCH;
}

static int64_t sys_tkill(struct interrupt_frame *f, uint64_t tid, uint64_t signal, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return signal_thread(0, (uint32_t)tid, signal);
}

static int64_t sys_tgkill(struct interrupt_frame *f, uint64_t tgid, uint64_t tid,
                          uint64_t signal, uint64_t a3, uint64_t a4, uint64_t a5) {
    return (int32_t)tgid <= 0 ? -LE_EINVAL : signal_thread((uint32_t)tgid, (uint32_t)tid, signal);
}

static int64_t sys_arch_prctl(struct interrupt_frame *f, uint64_t code, uint64_t address,
                              uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (code) {
    case LINUX_ARCH_SET_FS:
        if (address >= USER_END) {
            return -LE_EPERM;
        }
        thread_current()->fs_base = address;
        wrmsr(IA32_FS_BASE_MSR, address);
        return 0;
    case LINUX_ARCH_GET_FS: {
        uint64_t base = rdmsr(IA32_FS_BASE_MSR);
        return copy_to_user(address, &base, sizeof(base)) ? 0 : -LE_EFAULT;
    }
    default:
        return -LE_EINVAL;
    }
}

static int64_t sys_prctl(struct interrupt_frame *f, uint64_t option, uint64_t arg, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    struct process *process = me();
    switch (option) {
    case LINUX_PR_SET_NAME: {
        char name[16];
        if (copy_string_from_user(name, arg, sizeof(name)) < 0) {
            return -LE_EFAULT;
        }
        memset(process->name, 0, sizeof(process->name));
        memcpy(process->name, name, strlen(name));
        return 0;
    }
    case LINUX_PR_GET_NAME: {
        char name[16] = {0};
        memcpy(name, process->name, sizeof(name) - 1);
        return copy_to_user(arg, name, sizeof(name)) ? 0 : -LE_EFAULT;
    }
    default:
        return -LE_EINVAL;
    }
}

static int64_t sys_sched_getaffinity(struct interrupt_frame *f, uint64_t pid, uint64_t size,
                                     uint64_t out, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (size < sizeof(uint64_t)) {
        return -LE_EINVAL;
    }
    uint32_t cpus = cpu_online_count();
    uint64_t mask = cpus >= 64 ? ~0ULL : (1ULL << cpus) - 1;
    return copy_to_user(out, &mask, sizeof(mask)) ? (int64_t)sizeof(mask) : -LE_EFAULT;
}

static int64_t sys_sched_yield(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    thread_yield();
    return 0;
}

static void realtime(struct linux_timespec *ts);

/* A futex timeout in milliseconds: relative for FUTEX_WAIT, absolute (on the
 * monotonic or, with FUTEX_CLOCK_REALTIME, the real-time clock) for
 * FUTEX_WAIT_BITSET. -1: no timeout. */
static int64_t futex_timeout(uint64_t user_timespec, bool absolute, bool realtime_clock,
                             int64_t *ms) {
    int64_t error = timespec_ms(user_timespec, ms);
    if (error || *ms < 0 || !absolute) {
        return error;
    }
    struct linux_timespec now;
    if (realtime_clock) {
        realtime(&now);
    } else {
        now.sec = (int64_t)(timer_ms() / 1000);
        now.nsec = (int64_t)(timer_ms() % 1000) * 1000000;
    }
    *ms -= now.sec * 1000 + now.nsec / 1000000;
    if (*ms < 0) {
        *ms = 0;
    }
    return 0;
}

static int64_t sys_futex(struct interrupt_frame *f, uint64_t address, uint64_t op,
                         uint64_t value, uint64_t timeout, uint64_t address2, uint64_t value3) {
    /* Private or not makes no difference here: waits are per address space. */
    bool realtime_clock = op & LINUX_FUTEX_CLOCK_REALTIME;
    int64_t ms;
    int64_t error;
    switch (op & ~(uint64_t)(LINUX_FUTEX_PRIVATE_FLAG | LINUX_FUTEX_CLOCK_REALTIME)) {
    case LINUX_FUTEX_WAIT:
        error = futex_timeout(timeout, false, false, &ms);
        return error ? error : lx(futex_wait(address, (uint32_t)value, FUTEX_ANY, ms));
    case LINUX_FUTEX_WAIT_BITSET:
        error = futex_timeout(timeout, true, realtime_clock, &ms);
        return error ? error : lx(futex_wait(address, (uint32_t)value, (uint32_t)value3, ms));
    case LINUX_FUTEX_WAKE:
        return lx(futex_wake(me()->address_space, address, (int)(value & 0x7fffffff), FUTEX_ANY));
    case LINUX_FUTEX_WAKE_BITSET:
        return lx(futex_wake(me()->address_space, address, (int)(value & 0x7fffffff),
                             (uint32_t)value3));
    case LINUX_FUTEX_REQUEUE:
        /* The 4th argument is a count here, not a timeout. */
        return lx(futex_requeue(address, (int)(value & 0x7fffffff), address2,
                                (int)(timeout & 0x7fffffff), false, 0));
    case LINUX_FUTEX_CMP_REQUEUE:
        return lx(futex_requeue(address, (int)(value & 0x7fffffff), address2,
                                (int)(timeout & 0x7fffffff), true, (uint32_t)value3));
    default:
        return -LE_ENOSYS;
    }
}

static int64_t sys_set_tid_address(struct interrupt_frame *f, uint64_t address, uint64_t a1,
                                   uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct thread *thread = thread_current();
    thread->clear_on_exit = address;
    return thread->tid;
}

static int64_t sys_gettid(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    return thread_current()->tid;
}

static int64_t sys_exit_thread(struct interrupt_frame *f, uint64_t code, uint64_t a1,
                               uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    process_thread_exit((int)(code & 0xff));
}

/* ---- Limits and usage ---- */

static void limit_of(uint64_t resource, uint64_t limit[2]) {
    limit[0] = limit[1] = LINUX_RLIM_INFINITY;
    if (resource == LINUX_RLIMIT_STACK) {
        limit[0] = 8 * 1024 * 1024;
    } else if (resource == LINUX_RLIMIT_NOFILE) {
        limit[0] = limit[1] = HANDLE_MAX;
    }
}

static int64_t sys_getrlimit(struct interrupt_frame *f, uint64_t resource, uint64_t out,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t limit[2];
    limit_of(resource, limit);
    return copy_to_user(out, limit, sizeof(limit)) ? 0 : -LE_EFAULT;
}

static int64_t sys_prlimit64(struct interrupt_frame *f, uint64_t pid, uint64_t resource,
                             uint64_t new_limit, uint64_t old_limit, uint64_t a4, uint64_t a5) {
    if (old_limit) {
        return sys_getrlimit(f, resource, old_limit, 0, 0, 0, 0);
    }
    return 0; /* Limits aren't enforced; accept new ones. */
}

static int64_t sys_getrusage(struct interrupt_frame *f, uint64_t who, uint64_t out, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t usage[18] = {0};
    uint64_t ms = thread_current()->cpu_ms;
    usage[0] = ms / 1000;          /* ru_utime */
    usage[1] = (ms % 1000) * 1000;
    return copy_to_user(out, usage, sizeof(usage)) ? 0 : -LE_EFAULT;
}

static int64_t sys_times(struct interrupt_frame *f, uint64_t out, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (out) {
        uint64_t tms[4] = {thread_current()->cpu_ms / 10, 0, 0, 0};
        if (!copy_to_user(out, tms, sizeof(tms))) {
            return -LE_EFAULT;
        }
    }
    return (int64_t)(timer_ms() / 10);
}

/* ---- Time ---- */

static int64_t boot_epoch = -1; /* Wall-clock seconds at timer_ms() == 0. */

static void realtime(struct linux_timespec *ts) {
    if (boot_epoch < 0) {
        boot_epoch = time_now() - (int64_t)(timer_ms() / 1000);
    }
    uint64_t ms = timer_ms();
    ts->sec = boot_epoch + (int64_t)(ms / 1000);
    ts->nsec = (int64_t)(ms % 1000) * 1000000;
}

static int64_t sys_clock_gettime(struct interrupt_frame *f, uint64_t clock, uint64_t out,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_timespec ts;
    uint64_t ms;
    switch (clock) {
    case LINUX_CLOCK_REALTIME:
    case LINUX_CLOCK_REALTIME_COARSE:
        realtime(&ts);
        break;
    case LINUX_CLOCK_PROCESS_CPUTIME_ID:
    case LINUX_CLOCK_THREAD_CPUTIME_ID:
        ms = thread_current()->cpu_ms;
        ts.sec = (int64_t)(ms / 1000);
        ts.nsec = (int64_t)(ms % 1000) * 1000000;
        break;
    default: /* The monotonic and boot-time clocks. */
        ms = timer_ms();
        ts.sec = (int64_t)(ms / 1000);
        ts.nsec = (int64_t)(ms % 1000) * 1000000;
        break;
    }
    return copy_to_user(out, &ts, sizeof(ts)) ? 0 : -LE_EFAULT;
}

static int64_t sys_clock_getres(struct interrupt_frame *f, uint64_t clock, uint64_t out,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_timespec ts = {0, 1000000};
    return !out || copy_to_user(out, &ts, sizeof(ts)) ? 0 : -LE_EFAULT;
}

static int64_t sys_gettimeofday(struct interrupt_frame *f, uint64_t out, uint64_t zone,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_timespec ts;
    realtime(&ts);
    struct linux_timeval tv = {ts.sec, ts.nsec / 1000};
    if (out && !copy_to_user(out, &tv, sizeof(tv))) {
        return -LE_EFAULT;
    }
    if (zone) {
        int32_t utc[2] = {0, 0};
        if (!copy_to_user(zone, utc, sizeof(utc))) {
            return -LE_EFAULT;
        }
    }
    return 0;
}

static int64_t sys_time(struct interrupt_frame *f, uint64_t out, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_timespec ts;
    realtime(&ts);
    if (out && !copy_to_user(out, &ts.sec, sizeof(ts.sec))) {
        return -LE_EFAULT;
    }
    return ts.sec;
}

static int64_t sleep_ms(int64_t ms, uint64_t remaining_out) {
    uint64_t start = timer_ms();
    if (thread_sleep_ms_interruptible((uint64_t)ms) == 0) {
        return 0;
    }
    if (remaining_out) {
        int64_t left = ms - (int64_t)(timer_ms() - start);
        left = left < 0 ? 0 : left;
        struct linux_timespec ts = {left / 1000, (left % 1000) * 1000000};
        copy_to_user(remaining_out, &ts, sizeof(ts));
    }
    return -LE_EINTR;
}

static int64_t sys_nanosleep(struct interrupt_frame *f, uint64_t request, uint64_t remaining,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t ms;
    int64_t error = request ? timespec_ms(request, &ms) : -LE_EFAULT;
    return error ? error : sleep_ms(ms, remaining);
}

static int64_t sys_clock_nanosleep(struct interrupt_frame *f, uint64_t clock, uint64_t flags,
                                   uint64_t request, uint64_t remaining, uint64_t a4,
                                   uint64_t a5) {
    int64_t ms;
    int64_t error = request ? timespec_ms(request, &ms) : -LE_EFAULT;
    if (error) {
        return error;
    }
    if (flags & LINUX_TIMER_ABSTIME) {
        struct linux_timespec now;
        if (clock == LINUX_CLOCK_REALTIME) {
            realtime(&now);
        } else {
            now.sec = (int64_t)(timer_ms() / 1000);
            now.nsec = (int64_t)(timer_ms() % 1000) * 1000000;
        }
        ms -= now.sec * 1000 + now.nsec / 1000000;
        if (ms <= 0) {
            return 0;
        }
        remaining = 0;
    }
    return sleep_ms(ms, remaining);
}

/* ---- Timers that send signals ---- */

static int64_t sys_alarm(struct interrupt_frame *f, uint64_t seconds, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    struct process_timer *timer = &me()->timers[0];
    uint64_t left = process_timer_left(timer, NULL);
    process_timer_set(timer, me(), VX_SIGALRM, (seconds & 0xffffffff) * 1000, 0);
    return (int64_t)((left + 999) / 1000);
}

static uint64_t timeval_to_ms(const struct linux_timeval *tv) {
    return tv->sec < 0 ? 0 : (uint64_t)tv->sec * 1000 + (uint64_t)(tv->usec + 999) / 1000;
}

static struct linux_timeval ms_to_timeval(uint64_t ms) {
    return (struct linux_timeval){(int64_t)(ms / 1000), (int64_t)(ms % 1000) * 1000};
}

static int64_t sys_getitimer(struct interrupt_frame *f, uint64_t which, uint64_t out,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_timeval value[2] = {{0, 0}, {0, 0}}; /* it_interval, it_value */
    if (which == LINUX_ITIMER_REAL) {
        uint64_t interval;
        uint64_t left = process_timer_left(&me()->timers[0], &interval);
        value[0] = ms_to_timeval(interval);
        value[1] = ms_to_timeval(left);
    }
    return copy_to_user(out, value, sizeof(value)) ? 0 : -LE_EFAULT;
}

static int64_t sys_setitimer(struct interrupt_frame *f, uint64_t which, uint64_t new_value,
                             uint64_t old_value, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (old_value) {
        int64_t error = sys_getitimer(f, which, old_value, 0, 0, 0, 0);
        if (error) {
            return error;
        }
    }
    if (which != LINUX_ITIMER_REAL) {
        return which <= 2 ? 0 : -LE_EINVAL; /* CPU-time timers aren't kept yet. */
    }
    struct linux_timeval value[2];
    if (!new_value) {
        return 0;
    }
    if (!copy_from_user(value, new_value, sizeof(value))) {
        return -LE_EFAULT;
    }
    process_timer_set(&me()->timers[0], me(), VX_SIGALRM, timeval_to_ms(&value[1]),
                      timeval_to_ms(&value[0]));
    return 0;
}

static int64_t sys_timer_create(struct interrupt_frame *f, uint64_t clock, uint64_t user_event,
                                uint64_t id_out, uint64_t a3, uint64_t a4, uint64_t a5) {
    int signal = VX_SIGALRM;
    if (user_event) {
        struct {
            uint64_t value;
            int32_t signo;
            int32_t notify;
        } event;
        if (!copy_from_user(&event, user_event, sizeof(event))) {
            return -LE_EFAULT;
        }
        if (event.notify == LINUX_SIGEV_NONE) {
            signal = 0;
        } else if (event.notify == LINUX_SIGEV_SIGNAL || event.notify == LINUX_SIGEV_THREAD_ID) {
            if (event.signo < 1 || event.signo > VX_SIGNAL_COUNT) {
                return -LE_EINVAL;
            }
            signal = event.signo;
        } else {
            return -LE_EINVAL;
        }
    }
    struct process *process = me();
    for (int i = 1; i < PROCESS_TIMERS; i++) {
        struct process_timer *timer = &process->timers[i];
        if (!__atomic_exchange_n(&timer->in_use, true, __ATOMIC_SEQ_CST)) {
            timer->signal = signal;
            timer->clock = (int)clock;
            timer->process = process;
            int id = i;
            if (!copy_to_user(id_out, &id, sizeof(id))) {
                timer->in_use = false;
                return -LE_EFAULT;
            }
            return 0;
        }
    }
    return -LE_EAGAIN;
}

static struct process_timer *timer_by_id(uint64_t id) {
    struct process *process = me();
    return id >= 1 && id < PROCESS_TIMERS && process->timers[id].in_use ? &process->timers[id]
                                                                         : NULL;
}

static int64_t sys_timer_gettime(struct interrupt_frame *f, uint64_t id, uint64_t out,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct process_timer *timer = timer_by_id(id);
    if (!timer) {
        return -LE_EINVAL;
    }
    uint64_t interval;
    uint64_t left = process_timer_left(timer, &interval);
    struct linux_timespec spec[2] = {
        {(int64_t)(interval / 1000), (int64_t)(interval % 1000) * 1000000},
        {(int64_t)(left / 1000), (int64_t)(left % 1000) * 1000000},
    };
    return copy_to_user(out, spec, sizeof(spec)) ? 0 : -LE_EFAULT;
}

static int64_t sys_timer_settime(struct interrupt_frame *f, uint64_t id, uint64_t flags,
                                 uint64_t new_value, uint64_t old_value, uint64_t a4,
                                 uint64_t a5) {
    struct process_timer *timer = timer_by_id(id);
    if (!timer) {
        return -LE_EINVAL;
    }
    if (old_value) {
        int64_t error = sys_timer_gettime(f, id, old_value, 0, 0, 0, 0);
        if (error) {
            return error;
        }
    }
    struct linux_timespec spec[2];
    if (!copy_from_user(spec, new_value, sizeof(spec))) {
        return -LE_EFAULT;
    }
    int64_t interval = spec[0].sec * 1000 + (spec[0].nsec + 999999) / 1000000;
    int64_t value = spec[1].sec * 1000 + (spec[1].nsec + 999999) / 1000000;
    if ((flags & LINUX_TIMER_ABSTIME) && value > 0) {
        struct linux_timespec now;
        if (timer->clock == LINUX_CLOCK_REALTIME) {
            realtime(&now);
        } else {
            now.sec = (int64_t)(timer_ms() / 1000);
            now.nsec = (int64_t)(timer_ms() % 1000) * 1000000;
        }
        value -= now.sec * 1000 + now.nsec / 1000000;
        if (value <= 0) {
            value = 1; /* Already due: fire at the next tick. */
        }
    }
    process_timer_set(timer, me(), timer->signal, value > 0 ? (uint64_t)value : 0,
                      interval > 0 ? (uint64_t)interval : 0);
    return 0;
}

static int64_t sys_timer_delete(struct interrupt_frame *f, uint64_t id, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct process_timer *timer = timer_by_id(id);
    if (!timer) {
        return -LE_EINVAL;
    }
    process_timer_set(timer, me(), 0, 0, 0);
    timer->in_use = false;
    return 0;
}

/* Waits for one of the signals in `set` (normally blocked) and takes it
 * without running a handler. Returns its number. */
static int64_t sys_rt_sigtimedwait(struct interrupt_frame *f, uint64_t user_set,
                                   uint64_t user_info, uint64_t user_timeout, uint64_t size,
                                   uint64_t a4, uint64_t a5) {
    uint64_t set;
    if (size != sizeof(uint64_t) || !copy_from_user(&set, user_set, sizeof(set))) {
        return size != sizeof(uint64_t) ? -LE_EINVAL : -LE_EFAULT;
    }
    int64_t timeout_ms;
    int64_t error = timespec_ms(user_timeout, &timeout_ms);
    if (error) {
        return error;
    }
    set &= ~UNBLOCKABLE;
    struct thread *thread = thread_current();
    struct process *process = me();
    uint64_t start = timer_ms();
    for (;;) {
        /* Take a waited-for signal: the thread's own first, then the process's. */
        uint64_t *sets[2] = {&thread->pending_signals, &process->pending_signals};
        for (int i = 0; i < 2; i++) {
            uint64_t pending = __atomic_load_n(sets[i], __ATOMIC_ACQUIRE) & set;
            if (pending) {
                int signal = __builtin_ctzll(pending) + 1;
                __atomic_and_fetch(sets[i], ~BIT(signal), __ATOMIC_SEQ_CST);
                if (user_info) {
                    struct linux_siginfo info;
                    memset(&info, 0, sizeof(info));
                    info.signo = signal;
                    info.code = LINUX_SI_KERNEL;
                    copy_to_user(user_info, &info, sizeof(info));
                }
                return signal;
            }
        }
        if (thread_signal_pending(thread)) {
            return -LE_EINTR; /* Something else arrived: a handler or an exit. */
        }
        uint64_t waited = timer_ms() - start;
        if (timeout_ms >= 0 && waited >= (uint64_t)timeout_ms) {
            return -LE_EAGAIN;
        }
        thread_sleep_ms_interruptible(timeout_ms >= 0 && (uint64_t)timeout_ms - waited < 10
                                          ? (uint64_t)timeout_ms - waited
                                          : 10);
    }
}

/* ---- System information ---- */

static int64_t sys_uname(struct interrupt_frame *f, uint64_t out, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    char names[6][65];
    memset(names, 0, sizeof(names));
    /* The release is the Linux version whose interface this subsystem
     * follows; the version string says what's really running. */
    strcpy(names[0], "Vexa");
    strcpy(names[1], "vexa");
    strcpy(names[2], "6.1.0-vexa");
    strcpy(names[3], "Vexa " VEXA_VERSION " (Linux subsystem)");
    strcpy(names[4], "x86_64");
    strcpy(names[5], "(none)");
    return copy_to_user(out, names, sizeof(names)) ? 0 : -LE_EFAULT;
}

static int64_t sys_sysinfo(struct interrupt_frame *f, uint64_t out, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_sysinfo info;
    memset(&info, 0, sizeof(info));
    info.uptime = (int64_t)(timer_ms() / 1000);
    info.totalram = pmm_total_pages() * PAGE_SIZE;
    info.freeram = pmm_free_pages() * PAGE_SIZE;
    info.procs = 1;
    info.mem_unit = 1;
    return copy_to_user(out, &info, sizeof(info)) ? 0 : -LE_EFAULT;
}

static int64_t sys_getrandom(struct interrupt_frame *f, uint64_t out, uint64_t size,
                             uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint8_t chunk[256];
    for (uint64_t done = 0; done < size;) {
        uint64_t n = size - done < sizeof(chunk) ? size - done : sizeof(chunk);
        random_bytes(chunk, n);
        if (!copy_to_user(out + done, chunk, n)) {
            return done ? (int64_t)done : -LE_EFAULT;
        }
        done += n;
    }
    return (int64_t)size;
}

/* ---- Signals ---- */

static int64_t sys_rt_sigaction(struct interrupt_frame *f, uint64_t signal, uint64_t act,
                                uint64_t old_act, uint64_t size, uint64_t a4, uint64_t a5) {
    if (signal < 1 || signal > VX_SIGNAL_COUNT || size != sizeof(uint64_t)) {
        return -LE_EINVAL;
    }
    struct process *process = me();
    struct linux_data *d = data();
    if (!d) {
        return -LE_ENOMEM;
    }
    struct linux_sigaction *slot = &d->actions[signal - 1];
    if (old_act) {
        struct linux_sigaction old = *slot;
        enum signal_action current = process->signal_actions[signal - 1];
        old.handler = current == SIGNAL_IGNORE    ? LINUX_SIG_IGN
                      : current == SIGNAL_DEFAULT ? LINUX_SIG_DFL
                                                  : old.handler;
        if (!copy_to_user(old_act, &old, sizeof(old))) {
            return -LE_EFAULT;
        }
    }
    if (!act) {
        return 0;
    }
    struct linux_sigaction new;
    if (!copy_from_user(&new, act, sizeof(new))) {
        return -LE_EFAULT;
    }
    if (signal == VX_SIGKILL || signal == VX_SIGSTOP) {
        return -LE_EINVAL;
    }
    *slot = new;
    enum signal_action action = new.handler == LINUX_SIG_DFL   ? SIGNAL_DEFAULT
                                : new.handler == LINUX_SIG_IGN ? SIGNAL_IGNORE
                                                               : SIGNAL_HANDLER;
    process->signal_actions[signal - 1] = action;
    if (action == SIGNAL_IGNORE) {
        __atomic_and_fetch(&process->pending_signals, ~BIT(signal), __ATOMIC_SEQ_CST);
    }
    return 0;
}

static int64_t sys_rt_sigprocmask(struct interrupt_frame *f, uint64_t how, uint64_t set,
                                  uint64_t old_set, uint64_t size, uint64_t a4, uint64_t a5) {
    if (size != sizeof(uint64_t)) {
        return -LE_EINVAL;
    }
    struct thread *thread = thread_current();
    uint64_t old = thread->blocked_signals;
    if (set) {
        uint64_t mask;
        if (!copy_from_user(&mask, set, sizeof(mask))) {
            return -LE_EFAULT;
        }
        switch (how) {
        case LINUX_SIG_BLOCK: thread->blocked_signals |= mask; break;
        case LINUX_SIG_UNBLOCK: thread->blocked_signals &= ~mask; break;
        case LINUX_SIG_SETMASK: thread->blocked_signals = mask; break;
        default: return -LE_EINVAL;
        }
        thread->blocked_signals &= ~UNBLOCKABLE;
    }
    if (old_set && !copy_to_user(old_set, &old, sizeof(old))) {
        return -LE_EFAULT;
    }
    return 0;
}

static int64_t sys_rt_sigpending(struct interrupt_frame *f, uint64_t out, uint64_t size,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct thread *thread = thread_current();
    uint64_t pending = (__atomic_load_n(&me()->pending_signals, __ATOMIC_ACQUIRE) |
                        __atomic_load_n(&thread->pending_signals, __ATOMIC_ACQUIRE)) &
                       thread->blocked_signals;
    return copy_to_user(out, &pending, sizeof(pending)) ? 0 : -LE_EFAULT;
}

/* Waits, with `mask` blocked, until a signal arrives. The frame for its
 * handler records the mask from before, so returning restores it. */
static int64_t suspend_with(uint64_t mask) {
    struct thread *thread = thread_current();
    struct linux_thread *t = tdata();
    if (!t) {
        return -LE_ENOMEM;
    }
    t->suspend_mask = thread->blocked_signals;
    t->suspend_mask_valid = true;
    thread->blocked_signals = mask & ~UNBLOCKABLE;
    while (!thread_signal_pending(thread)) {
        thread_sleep_ms_interruptible(60 * 1000);
    }
    return -LE_EINTR;
}

static int64_t sys_rt_sigsuspend(struct interrupt_frame *f, uint64_t set, uint64_t size,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t mask;
    if (size != sizeof(uint64_t) || !copy_from_user(&mask, set, sizeof(mask))) {
        return size != sizeof(uint64_t) ? -LE_EINVAL : -LE_EFAULT;
    }
    return suspend_with(mask);
}

static int64_t sys_pause(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    return suspend_with(thread_current()->blocked_signals);
}

static int64_t sys_sigaltstack(struct interrupt_frame *f, uint64_t stack, uint64_t old_stack,
                               uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    /* Handlers always run on the normal stack for now. */
    if (old_stack) {
        struct {
            uint64_t sp;
            int32_t flags;
            int32_t pad;
            uint64_t size;
        } disabled = {0, LINUX_SS_DISABLE, 0, 0};
        if (!copy_to_user(old_stack, &disabled, sizeof(disabled))) {
            return -LE_EFAULT;
        }
    }
    return 0;
}

/* System calls that are worth restarting after a handler with SA_RESTART. */
static bool restartable(uint64_t number) {
    switch (number) {
    case LINUX_SYS_read: case LINUX_SYS_write: case LINUX_SYS_readv: case LINUX_SYS_writev:
    case LINUX_SYS_open: case LINUX_SYS_openat: case LINUX_SYS_ioctl: case LINUX_SYS_wait4:
    case LINUX_SYS_pread64: case LINUX_SYS_pwrite64:
        return true;
    default:
        return false;
    }
}

static bool linux_deliver_signal(struct interrupt_frame *frame, int signal) {
    struct thread *thread = thread_current();
    struct linux_data *d = data();
    struct linux_thread *t = tdata();
    if (!d || !t) {
        return false;
    }
    struct linux_sigaction action = d->actions[signal - 1];
    if (action.handler <= LINUX_SIG_IGN || !(action.flags & LINUX_SA_RESTORER)) {
        return false; /* No way back from the handler: end the process instead. */
    }
    if (frame->vector == SYSCALL_VECTOR && (int64_t)frame->rax == -LE_EINTR &&
        (action.flags & LINUX_SA_RESTART) && restartable(t->last_syscall)) {
        frame->rax = t->last_syscall; /* Run the call again once the handler returns. */
        frame->rip -= 2;              /* The length of the syscall instruction. */
    }

    /* The frame, below the interrupted code's stack and its red zone:
     *   rsp -> return address (the restorer), ucontext, siginfo */
    uint64_t size = 8 + sizeof(struct linux_ucontext) + sizeof(struct linux_siginfo);
    uint64_t sp = ((frame->rsp - 128 - size) & ~15ULL) - 8; /* rsp % 16 == 8 on entry. */
    uint64_t uc_address = sp + 8;
    uint64_t info_address = uc_address + sizeof(struct linux_ucontext);

    struct linux_ucontext uc;
    memset(&uc, 0, sizeof(uc));
    struct linux_sigcontext *c = &uc.mcontext;
    c->r8 = frame->r8, c->r9 = frame->r9, c->r10 = frame->r10, c->r11 = frame->r11;
    c->r12 = frame->r12, c->r13 = frame->r13, c->r14 = frame->r14, c->r15 = frame->r15;
    c->rdi = frame->rdi, c->rsi = frame->rsi, c->rbp = frame->rbp, c->rbx = frame->rbx;
    c->rdx = frame->rdx, c->rax = frame->rax, c->rcx = frame->rcx, c->rsp = frame->rsp;
    c->rip = frame->rip, c->eflags = frame->rflags;
    c->cs = (uint16_t)frame->cs;
    c->trapno = frame->vector == SYSCALL_VECTOR ? 0 : frame->vector;
    c->err = frame->vector == SYSCALL_VECTOR ? 0 : frame->error_code;
    uc.sigmask = t->suspend_mask_valid ? t->suspend_mask : thread->blocked_signals;
    t->suspend_mask_valid = false;

    struct linux_siginfo info;
    memset(&info, 0, sizeof(info));
    info.signo = signal;
    info.code = LINUX_SI_KERNEL;

    uint64_t restorer = action.restorer;
    if (!copy_to_user(sp, &restorer, sizeof(restorer)) ||
        !copy_to_user(uc_address, &uc, sizeof(uc)) ||
        !copy_to_user(info_address, &info, sizeof(info))) {
        return false; /* The stack is unusable; Linux ends the process too. */
    }

    /* Keep the interrupted code's vector registers until the handler returns. */
    struct saved_fpu *saved = kmalloc(sizeof(*saved));
    void *state = saved ? fpu_alloc_state() : NULL;
    if (!state) {
        kfree(saved);
        return false;
    }
    fpu_save(state);
    saved->frame = uc_address;
    saved->state = state;
    saved->next = t->saved_fpu;
    t->saved_fpu = saved;
    if (++t->saved_fpu_count > SAVED_FPU_MAX) {
        /* Handlers that never returned (they jumped out with longjmp) leave
         * entries behind; drop the oldest. */
        struct saved_fpu *s = t->saved_fpu;
        while (s->next && s->next->next) {
            s = s->next;
        }
        free_saved_fpu(s->next);
        s->next = NULL;
        t->saved_fpu_count--;
    }

    frame->rdi = (uint64_t)signal;
    frame->rsi = info_address;
    frame->rdx = uc_address;
    frame->rax = 0;
    frame->rsp = sp;
    frame->rip = action.handler;
    frame->rflags &= ~(LINUX_EFLAGS_TF | LINUX_EFLAGS_DF);

    thread->blocked_signals |= action.mask | (action.flags & LINUX_SA_NODEFER ? 0 : BIT(signal));
    thread->blocked_signals &= ~UNBLOCKABLE;
    if (action.flags & LINUX_SA_RESETHAND) {
        d->actions[signal - 1].handler = LINUX_SIG_DFL;
        me()->signal_actions[signal - 1] = SIGNAL_DEFAULT;
    }
    return true;
}

static int64_t sys_rt_sigreturn(struct interrupt_frame *f, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t uc_address = f->rsp; /* The handler's `ret` popped the return address. */
    struct linux_ucontext uc;
    struct linux_sigcontext *c = &uc.mcontext;
    if (!copy_from_user(&uc, uc_address, sizeof(uc)) || c->rip >= USER_END ||
        c->rsp >= USER_END) {
        kprintf("[linux] %s (process %u): bad signal frame\n", me()->name, me()->id);
        process_exit_by_signal(VX_SIGSEGV);
    }
    f->r8 = c->r8, f->r9 = c->r9, f->r10 = c->r10, f->r11 = c->r11;
    f->r12 = c->r12, f->r13 = c->r13, f->r14 = c->r14, f->r15 = c->r15;
    f->rdi = c->rdi, f->rsi = c->rsi, f->rbp = c->rbp, f->rbx = c->rbx;
    f->rdx = c->rdx, f->rax = c->rax, f->rcx = c->rcx, f->rsp = c->rsp;
    f->rip = c->rip;
    /* Only the flags a program may change; interrupts stay on. */
    f->rflags = (c->eflags & LINUX_EFLAGS_USER) | RFLAGS_INTERRUPTS_ON;
    f->vector = 0; /* Not a system call return any more: never restart it. */
    thread_current()->blocked_signals = uc.sigmask & ~UNBLOCKABLE;

    struct linux_thread *t = tdata();
    while (t && t->saved_fpu) {
        struct saved_fpu *saved = t->saved_fpu;
        t->saved_fpu = saved->next;
        t->saved_fpu_count--;
        bool mine = saved->frame == uc_address;
        if (mine) {
            fpu_restore(saved->state);
        }
        free_saved_fpu(saved);
        if (mine) {
            break;
        }
    }
    return (int64_t)f->rax; /* The dispatcher stores this back into rax. */
}

/* ---- The system call table ---- */

#define CALL(name, fn) [LINUX_SYS_##name] = fn

static const linux_fn syscalls[] = {
    CALL(read, sys_read),
    CALL(write, sys_write),
    CALL(open, sys_open),
    CALL(close, sys_close),
    CALL(stat, sys_stat),
    CALL(fstat, sys_fstat),
    CALL(lstat, sys_lstat),
    CALL(poll, sys_poll),
    CALL(lseek, sys_lseek),
    CALL(mmap, sys_mmap),
    CALL(mprotect, sys_mprotect),
    CALL(munmap, sys_munmap),
    CALL(brk, sys_brk),
    CALL(rt_sigaction, sys_rt_sigaction),
    CALL(rt_sigprocmask, sys_rt_sigprocmask),
    CALL(rt_sigreturn, sys_rt_sigreturn),
    CALL(ioctl, sys_ioctl),
    CALL(pread64, sys_pread64),
    CALL(pwrite64, sys_pwrite64),
    CALL(readv, sys_readv),
    CALL(writev, sys_writev),
    CALL(access, sys_access),
    CALL(pipe, sys_pipe),
    CALL(select, sys_select),
    CALL(sched_yield, sys_sched_yield),
    CALL(mremap, sys_mremap),
    CALL(msync, sys_accept_quietly),
    CALL(madvise, sys_accept_quietly),
    CALL(dup, sys_dup),
    CALL(dup2, sys_dup2),
    CALL(pause, sys_pause),
    CALL(nanosleep, sys_nanosleep),
    CALL(alarm, sys_alarm),
    CALL(setitimer, sys_setitimer),
    CALL(getitimer, sys_getitimer),
    CALL(timer_create, sys_timer_create),
    CALL(timer_settime, sys_timer_settime),
    CALL(timer_gettime, sys_timer_gettime),
    CALL(timer_getoverrun, sys_zero),
    CALL(timer_delete, sys_timer_delete),
    CALL(rt_sigtimedwait, sys_rt_sigtimedwait),
    CALL(getpid, sys_getpid),
    CALL(sendfile, sys_sendfile),
    CALL(socket, linux_sys_socket),
    CALL(socketpair, linux_sys_socketpair),
    CALL(bind, linux_sys_bind),
    CALL(connect, linux_sys_connect),
    CALL(listen, linux_sys_listen),
    CALL(accept, linux_sys_accept),
    CALL(accept4, linux_sys_accept4),
    CALL(getsockname, linux_sys_getsockname),
    CALL(getpeername, linux_sys_getpeername),
    CALL(shutdown, linux_sys_shutdown),
    CALL(sendto, linux_sys_sendto),
    CALL(recvfrom, linux_sys_recvfrom),
    CALL(sendmsg, linux_sys_sendmsg),
    CALL(recvmsg, linux_sys_recvmsg),
    CALL(sendmmsg, linux_sys_sendmmsg),
    CALL(recvmmsg, linux_sys_recvmmsg),
    CALL(setsockopt, linux_sys_setsockopt),
    CALL(getsockopt, linux_sys_getsockopt),
    CALL(clone, sys_clone),
    CALL(fork, sys_fork),
    CALL(vfork, sys_fork),
    CALL(execve, sys_execve),
    CALL(exit, sys_exit_thread),
    CALL(wait4, sys_wait4),
    CALL(kill, sys_kill),
    CALL(uname, sys_uname),
    CALL(fcntl, sys_fcntl),
    CALL(flock, sys_accept_quietly),
    CALL(fsync, sys_accept_quietly),
    CALL(fdatasync, sys_accept_quietly),
    CALL(truncate, sys_truncate),
    CALL(ftruncate, sys_ftruncate),
    CALL(getcwd, sys_getcwd),
    CALL(chdir, sys_chdir),
    CALL(fchdir, sys_fchdir),
    CALL(rename, sys_rename),
    CALL(mkdir, sys_mkdir),
    CALL(rmdir, sys_rmdir),
    CALL(creat, sys_creat),
    CALL(link, sys_link),
    CALL(unlink, sys_unlink),
    CALL(symlink, sys_symlink),
    CALL(readlink, sys_readlink),
    CALL(chmod, sys_chmod),
    CALL(fchmod, sys_fchmod),
    CALL(chown, sys_accept_quietly),
    CALL(fchown, sys_accept_quietly),
    CALL(lchown, sys_accept_quietly),
    CALL(umask, sys_umask),
    CALL(gettimeofday, sys_gettimeofday),
    CALL(getrlimit, sys_getrlimit),
    CALL(getrusage, sys_getrusage),
    CALL(sysinfo, sys_sysinfo),
    CALL(times, sys_times),
    CALL(getuid, sys_zero),
    CALL(getgid, sys_zero),
    CALL(setuid, sys_zero),
    CALL(setgid, sys_zero),
    CALL(geteuid, sys_zero),
    CALL(getegid, sys_zero),
    CALL(setpgid, sys_setpgid),
    CALL(getppid, sys_getppid),
    CALL(getpgrp, sys_getpgrp),
    CALL(setsid, sys_setsid),
    CALL(setreuid, sys_zero),
    CALL(setregid, sys_zero),
    CALL(getgroups, sys_zero),
    CALL(setgroups, sys_zero),
    CALL(setresuid, sys_zero),
    CALL(getresuid, sys_getresid),
    CALL(setresgid, sys_zero),
    CALL(getresgid, sys_getresid),
    CALL(getpgid, sys_getpgid),
    CALL(getsid, sys_getsid),
    CALL(rt_sigpending, sys_rt_sigpending),
    CALL(rt_sigsuspend, sys_rt_sigsuspend),
    CALL(sigaltstack, sys_sigaltstack),
    CALL(utime, sys_accept_quietly),
    CALL(mknod, sys_not_permitted),
    CALL(personality, sys_zero),
    CALL(statfs, sys_statfs),
    CALL(fstatfs, sys_fstatfs),
    CALL(getpriority, sys_zero),
    CALL(setpriority, sys_zero),
    CALL(prctl, sys_prctl),
    CALL(arch_prctl, sys_arch_prctl),
    CALL(setrlimit, sys_accept_quietly),
    CALL(sync, sys_accept_quietly),
    CALL(mount, sys_not_permitted),
    CALL(umount2, sys_not_permitted),
    CALL(reboot, sys_not_permitted),
    CALL(sethostname, sys_not_permitted),
    CALL(gettid, sys_gettid),
    CALL(tkill, sys_tkill),
    CALL(time, sys_time),
    CALL(futex, sys_futex),
    CALL(sched_getaffinity, sys_sched_getaffinity),
    CALL(getdents64, sys_getdents64),
    CALL(set_tid_address, sys_set_tid_address),
    CALL(clock_gettime, sys_clock_gettime),
    CALL(clock_getres, sys_clock_getres),
    CALL(clock_nanosleep, sys_clock_nanosleep),
    CALL(exit_group, sys_exit),
    CALL(tgkill, sys_tgkill),
    CALL(utimes, sys_accept_quietly),
    CALL(openat, sys_openat),
    CALL(mkdirat, sys_mkdirat),
    CALL(mknodat, sys_not_permitted),
    CALL(fchownat, sys_accept_quietly),
    CALL(futimesat, sys_accept_quietly),
    CALL(newfstatat, sys_newfstatat),
    CALL(unlinkat, sys_unlinkat),
    CALL(renameat, sys_renameat),
    CALL(linkat, sys_linkat),
    CALL(symlinkat, sys_symlinkat),
    CALL(readlinkat, sys_readlinkat),
    CALL(fchmodat, sys_fchmodat),
    CALL(faccessat, sys_faccessat),
    CALL(pselect6, sys_pselect6),
    CALL(ppoll, sys_ppoll),
    CALL(set_robust_list, sys_accept_quietly),
    CALL(utimensat, sys_accept_quietly),
    CALL(dup3, sys_dup3),
    CALL(pipe2, sys_pipe2),
    CALL(prlimit64, sys_prlimit64),
    CALL(renameat2, sys_renameat2),
    CALL(getrandom, sys_getrandom),
    CALL(membarrier, sys_no_such_call_quietly),
    CALL(epoll_create, sys_epoll_create),
    CALL(epoll_create1, sys_epoll_create1),
    CALL(epoll_ctl, sys_epoll_ctl),
    CALL(epoll_wait, sys_epoll_wait),
    CALL(epoll_pwait, sys_epoll_wait),
    CALL(epoll_pwait2, sys_epoll_pwait2),
    CALL(fadvise64, sys_accept_quietly),
    CALL(copy_file_range, sys_no_such_call_quietly),
    CALL(statx, sys_no_such_call_quietly),
    CALL(rseq, sys_no_such_call_quietly),
    CALL(close_range, sys_close_range),
    CALL(faccessat2, sys_faccessat),
};

#define SYSCALL_COUNT (sizeof(syscalls) / sizeof(syscalls[0]))

/* One bit per call number already reported, so each is logged once. */
static uint64_t reported[(LINUX_SYSCALL_LIMIT + 63) / 64];

static void report_unimplemented(struct interrupt_frame *f) {
    uint64_t number = f->rax;
    if (number < LINUX_SYSCALL_LIMIT) {
        uint64_t bit = 1ULL << (number % 64);
        if (__atomic_fetch_or(&reported[number / 64], bit, __ATOMIC_RELAXED) & bit) {
            return;
        }
    }
    kprintf("[linux] %s (process %u): system call %lu is not implemented "
            "(0x%lx, 0x%lx, 0x%lx, 0x%lx)\n",
            me()->name, me()->id, number, f->rdi, f->rsi, f->rdx, f->r10);
}

static void linux_syscall(struct interrupt_frame *frame) {
    uint64_t number = frame->rax;
    struct linux_thread *t = tdata();
    if (t) {
        t->last_syscall = number;
    }
    if (number >= SYSCALL_COUNT || !syscalls[number]) {
        report_unimplemented(frame);
        frame->rax = (uint64_t)-LE_ENOSYS;
        return;
    }
    frame->rax = (uint64_t)syscalls[number](frame, frame->rdi, frame->rsi, frame->rdx,
                                            frame->r10, frame->r8, frame->r9);
}

const struct personality linux_personality = {
    .name = "linux",
    .syscall = linux_syscall,
    .deliver_signal = linux_deliver_signal,
    .fork_data = linux_fork_data,
    .free_data = linux_free_data,
    .free_thread_data = linux_free_thread_data,
    .translate_path = linux_translate_path,
};
