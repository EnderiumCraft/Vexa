/* POSIX's file, directory, process and memory calls, over Vexa's own. A file
 * descriptor is a native handle; errors set errno (Linux's numbers). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/resource.h>
#include <vexa/syscall.h>
#include "internal.h"

/* ---- errno ---- */

static const unsigned char errno_of_vx[] = {
    [VX_ENOSYS] = ENOSYS, [VX_EFAULT] = EFAULT, [VX_EINVAL] = EINVAL, [VX_ENOENT] = ENOENT,
    [VX_EEXIST] = EEXIST, [VX_ENOTDIR] = ENOTDIR, [VX_EISDIR] = EISDIR,
    [VX_ENOTEMPTY] = ENOTEMPTY, [VX_EBADF] = EBADF, [VX_EACCES] = EACCES, [VX_ENOSPC] = ENOSPC,
    [VX_EIO] = EIO, [VX_ENAMETOOLONG] = ENAMETOOLONG, [VX_EMFILE] = EMFILE,
    [VX_ENOMEM] = ENOMEM, [VX_EROFS] = EROFS, [VX_EBUSY] = EBUSY, [VX_EXDEV] = EXDEV,
    [VX_EINTR] = EINTR, [VX_EPIPE] = EPIPE, [VX_ECHILD] = ECHILD, [VX_ESRCH] = ESRCH,
    [VX_EAGAIN] = EAGAIN, [VX_ENOEXEC] = ENOEXEC, [VX_E2BIG] = E2BIG, [VX_ENOTTY] = ENOTTY,
    [VX_ESPIPE] = ESPIPE, [VX_ELOOP] = ELOOP, [VX_ETIMEDOUT] = ETIMEDOUT,
    [VX_ENOTSOCK] = ENOTSOCK, [VX_EAFNOSUPPORT] = EAFNOSUPPORT,
    [VX_EPROTONOSUPPORT] = EPROTONOSUPPORT, [VX_EOPNOTSUPP] = EOPNOTSUPP,
    [VX_EADDRINUSE] = EADDRINUSE, [VX_EADDRNOTAVAIL] = EADDRNOTAVAIL,
    [VX_ENETUNREACH] = ENETUNREACH, [VX_ECONNREFUSED] = ECONNREFUSED,
    [VX_ECONNRESET] = ECONNRESET, [VX_ENOTCONN] = ENOTCONN, [VX_EISCONN] = EISCONN,
    [VX_EINPROGRESS] = EINPROGRESS, [VX_EALREADY] = EALREADY, [VX_EMSGSIZE] = EMSGSIZE,
    [VX_EDESTADDRREQ] = EDESTADDRREQ, [VX_ENOPROTOOPT] = ENOPROTOOPT,
    [VX_ECONNABORTED] = ECONNABORTED, [VX_EHOSTUNREACH] = EHOSTUNREACH,
    [VX_ENODEV] = ENODEV,
};

int __vx_errno_of(long vx_error) {
    unsigned long code = (unsigned long)(vx_error < 0 ? -vx_error : vx_error);
    return code < sizeof(errno_of_vx) && errno_of_vx[code] ? errno_of_vx[code] : EIO;
}

long __vx_errno_result(long result) {
    if (result >= 0) {
        return result;
    }
    errno = __vx_errno_of(result);
    return -1;
}

int *__errno_location(void) {
    return &__vx_tcb()->error;
}

/* ---- Files ---- */

int open(const char *path, int flags, ...) {
    unsigned vx = 0;
    switch (flags & O_ACCMODE) {
    case O_RDONLY: vx = VX_OPEN_READ; break;
    case O_WRONLY: vx = VX_OPEN_WRITE; break;
    default: vx = VX_OPEN_READ | VX_OPEN_WRITE; break;
    }
    if (flags & O_CREAT) {
        vx |= VX_OPEN_CREATE;
    }
    if (flags & O_TRUNC) {
        vx |= VX_OPEN_TRUNCATE;
    }
    if (flags & O_APPEND) {
        vx |= VX_OPEN_APPEND;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        struct vx_stat st;
        if (vx_stat(path, &st) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    if (flags & O_DIRECTORY) {
        struct vx_stat st;
        long error = vx_stat(path, &st);
        if (error) {
            return (int)__vx_errno_result(error);
        }
        if (st.type != VX_TYPE_DIRECTORY) {
            errno = ENOTDIR;
            return -1;
        }
    }
    return (int)__vx_errno_result(vx_open(path, vx));
}

int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int fcntl(int fd, int command, ...) {
    switch (command) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: { /* The lowest free handle from `lowest` up. */
        va_list args;
        va_start(args, command);
        int lowest = va_arg(args, int);
        va_end(args);
        if (lowest < 0) {
            errno = EINVAL;
            return -1;
        }
        /* vx_dup gives the lowest free handle: take (and then give back) the
         * ones below `lowest` until it's one at or above it. */
        int below[64], count = 0;
        long handle;
        while ((handle = vx_dup(fd, -1)) >= 0 && handle < lowest && count < 64) {
            below[count++] = (int)handle;
        }
        while (count) {
            vx_close(below[--count]);
        }
        if (handle >= 0 && handle < lowest) { /* (More than 64 below it.) */
            vx_close((int)handle);
            handle = -VX_EMFILE;
        }
        return (int)__vx_errno_result(handle);
    }
    case F_GETFD:
    case F_SETFD:
        return 0;
    case F_SETFL: {
        va_list args;
        va_start(args, command);
        int flags = va_arg(args, int);
        va_end(args);
        __vx_set_nonblocking(fd, flags & O_NONBLOCK);
        return 0;
    }
    case F_GETFL: {
        struct vx_stat st;
        return vx_handle_stat(fd, &st) ? (errno = EBADF, -1)
                                       : O_RDWR | (__vx_nonblocking(fd) ? O_NONBLOCK : 0);
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

ssize_t read(int fd, void *buffer, size_t size) {
    if (__vx_nonblocking(fd) && __vx_is_socket(fd)) {
        struct vx_message m = {.data = buffer, .size = size, .flags = VX_MSG_DONTWAIT};
        return __vx_errno_result(vx_receive(fd, &m));
    }
    return __vx_errno_result(vx_read(fd, buffer, size));
}

ssize_t write(int fd, const void *buffer, size_t size) {
    if (__vx_nonblocking(fd) && __vx_is_socket(fd)) {
        struct vx_message m = {.data = (void *)buffer, .size = size, .flags = VX_MSG_DONTWAIT};
        return __vx_errno_result(vx_send(fd, &m));
    }
    return __vx_errno_result(vx_write(fd, buffer, size));
}

ssize_t pread(int fd, void *buffer, size_t size, off_t offset) {
    long here = vx_seek(fd, 0, VX_SEEK_CURRENT);
    if (here < 0 || vx_seek(fd, offset, VX_SEEK_SET) < 0) {
        return __vx_errno_result(here < 0 ? here : -VX_ESPIPE);
    }
    long n = vx_read(fd, buffer, size);
    vx_seek(fd, here, VX_SEEK_SET);
    return __vx_errno_result(n);
}

ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset) {
    long here = vx_seek(fd, 0, VX_SEEK_CURRENT);
    if (here < 0 || vx_seek(fd, offset, VX_SEEK_SET) < 0) {
        return __vx_errno_result(here < 0 ? here : -VX_ESPIPE);
    }
    long n = vx_write(fd, buffer, size);
    vx_seek(fd, here, VX_SEEK_SET);
    return __vx_errno_result(n);
}

int close(int fd) {
    __vx_forget_fd(fd);
    return (int)__vx_errno_result(vx_close(fd));
}

off_t lseek(int fd, off_t offset, int whence) {
    return __vx_errno_result(vx_seek(fd, offset, whence));
}

int ftruncate(int fd, off_t size) {
    return (int)__vx_errno_result(vx_resize(fd, (unsigned long)size));
}

int truncate(const char *path, off_t size) {
    int fd = vx_open(path, VX_OPEN_WRITE);
    if (fd < 0) {
        return (int)__vx_errno_result(fd);
    }
    long error = vx_resize(fd, (unsigned long)size);
    vx_close(fd);
    return (int)__vx_errno_result(error);
}

/* (Vexa writes go straight to the disk: there's nothing waiting to be written.) */
void sync(void) {
}

int fsync(int fd) {
    (void)fd;
    return 0;
}

static void stat_of(const struct vx_stat *in, struct stat *out) {
    memset(out, 0, sizeof(*out));
    static const mode_t types[] = {
        [VX_TYPE_FILE] = S_IFREG,      [VX_TYPE_DIRECTORY] = S_IFDIR,
        [VX_TYPE_CHAR_DEVICE] = S_IFCHR, [VX_TYPE_BLOCK_DEVICE] = S_IFBLK,
        [VX_TYPE_SYMLINK] = S_IFLNK,   [VX_TYPE_SOCKET] = S_IFSOCK,
    };
    out->st_ino = in->inode;
    out->st_mode = (in->type < sizeof(types) / sizeof(types[0]) ? types[in->type] : 0) |
                   (in->mode & 07777);
    out->st_nlink = in->links;
    out->st_size = (off_t)in->size;
    out->st_blksize = 4096;
    out->st_blocks = (blkcnt_t)((in->size + 511) / 512);
    out->st_atim.tv_sec = out->st_mtim.tv_sec = out->st_ctim.tv_sec = in->modified;
}

int stat(const char *path, struct stat *st) {
    struct vx_stat vs;
    long error = vx_stat(path, &vs);
    if (error) {
        return (int)__vx_errno_result(error);
    }
    stat_of(&vs, st);
    return 0;
}

int lstat(const char *path, struct stat *st) {
    struct vx_stat vs;
    long error = vx_lstat(path, &vs);
    if (error) {
        return (int)__vx_errno_result(error);
    }
    stat_of(&vs, st);
    return 0;
}

int fstat(int fd, struct stat *st) {
    struct vx_stat vs;
    long error = vx_handle_stat(fd, &vs);
    if (error) {
        return (int)__vx_errno_result(error);
    }
    stat_of(&vs, st);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    return (int)__vx_errno_result(vx_mkdir(path));
}

int chmod(const char *path, mode_t mode) {
    (void)path, (void)mode;
    return 0;
}

mode_t umask(mode_t mask) {
    (void)mask;
    return 022;
}

int unlink(const char *path) {
    struct vx_stat st;
    if (vx_lstat(path, &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
        errno = EISDIR;
        return -1;
    }
    return (int)__vx_errno_result(vx_remove(path));
}

int rmdir(const char *path) {
    struct vx_stat st;
    long error = vx_lstat(path, &st);
    if (!error && st.type != VX_TYPE_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    return (int)__vx_errno_result(error ? error : vx_remove(path));
}

int remove(const char *path) {
    return (int)__vx_errno_result(vx_remove(path));
}

int rename(const char *from, const char *to) {
    return (int)__vx_errno_result(vx_rename(from, to));
}

int access(const char *path, int mode) {
    (void)mode;
    struct vx_stat st;
    return (int)__vx_errno_result(vx_stat(path, &st));
}

int chdir(const char *path) {
    return (int)__vx_errno_result(vx_chdir(path));
}

char *getcwd(char *buffer, size_t size) {
    char here[1024];
    long n = vx_getcwd(here, sizeof(here));
    if (n < 0) {
        __vx_errno_result(n);
        return NULL;
    }
    size_t length = strlen(here);
    if (!buffer) {
        size = size ? size : length + 1;
        buffer = malloc(size);
        if (!buffer) {
            errno = ENOMEM;
            return NULL;
        }
    }
    if (length + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, here, length + 1);
    return buffer;
}

int isatty(int fd) {
    struct vx_tty_size size;
    if (vx_control(fd, VX_TTY_GET_SIZE, &size, sizeof(size)) == 0) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

int pipe(int fds[2]) {
    return (int)__vx_errno_result(vx_pipe(fds));
}

int dup(int fd) {
    return (int)__vx_errno_result(vx_dup(fd, -1));
}

int dup2(int fd, int to) {
    if (to < 0) {
        errno = EBADF;
        return -1;
    }
    return (int)__vx_errno_result(vx_dup(fd, to));
}

int symlink(const char *target, const char *path) {
    return (int)__vx_errno_result(vx_symlink(target, path));
}

ssize_t readlink(const char *path, char *buffer, size_t size) {
    return __vx_errno_result(vx_readlink(path, buffer, size));
}

/* ---- Directories ---- */

struct __vx_dir {
    int handle;
    struct vx_dir_entry entries[16];
    long count, next;
    struct dirent current;
};

DIR *opendir(const char *path) {
    struct vx_stat st;
    long error = vx_stat(path, &st);
    if (!error && st.type != VX_TYPE_DIRECTORY) {
        errno = ENOTDIR;
        return NULL;
    }
    int handle = error ? (int)error : vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        __vx_errno_result(handle);
        return NULL;
    }
    DIR *dir = calloc(1, sizeof(DIR));
    if (!dir) {
        vx_close(handle);
        errno = ENOMEM;
        return NULL;
    }
    dir->handle = handle;
    return dir;
}

struct dirent *readdir(DIR *dir) {
    if (dir->next == dir->count) {
        long n = vx_read_dir(dir->handle, dir->entries, 16);
        if (n <= 0) {
            if (n < 0) {
                __vx_errno_result(n);
            }
            return NULL;
        }
        dir->count = n;
        dir->next = 0;
    }
    const struct vx_dir_entry *e = &dir->entries[dir->next++];
    static const unsigned char types[] = {
        [VX_TYPE_FILE] = DT_REG, [VX_TYPE_DIRECTORY] = DT_DIR, [VX_TYPE_CHAR_DEVICE] = DT_CHR,
        [VX_TYPE_BLOCK_DEVICE] = DT_BLK, [VX_TYPE_SYMLINK] = DT_LNK,
        [VX_TYPE_SOCKET] = DT_SOCK,
    };
    dir->current.d_ino = e->inode;
    dir->current.d_type = e->type < sizeof(types) ? types[e->type] : DT_UNKNOWN;
    size_t n = e->name_length < sizeof(dir->current.d_name) - 1 ? e->name_length
                                                                : sizeof(dir->current.d_name) - 1;
    memcpy(dir->current.d_name, e->name, n);
    dir->current.d_name[n] = '\0';
    return &dir->current;
}

void rewinddir(DIR *dir) {
    vx_seek(dir->handle, 0, VX_SEEK_SET);
    dir->count = dir->next = 0;
}

int dirfd(DIR *dir) {
    return dir->handle;
}

int closedir(DIR *dir) {
    vx_close(dir->handle);
    free(dir);
    return 0;
}

/* ---- The process ---- */

unsigned sleep(unsigned seconds) {
    vx_sleep((uint64_t)seconds * 1000);
    return 0;
}

int usleep(useconds_t us) {
    vx_sleep((us + 999) / 1000);
    return 0;
}

/* ---- Priorities ---- */

int getpriority(int which, id_t who) {
    int nice = 0;
    long error = vx_priority(which == PRIO_PROCESS ? (long)who : 0, VX_PRIORITY_GET, &nice);
    if (error) {
        errno = __vx_errno_of(error);
        return -1;
    }
    errno = 0; /* (-1 is a valid answer: callers check errno.) */
    return nice;
}

int setpriority(int which, id_t who, int value) {
    long error = vx_priority(which == PRIO_PROCESS ? (long)who : 0, value, NULL);
    if (error) {
        errno = __vx_errno_of(error);
        return -1;
    }
    return 0;
}

int nice(int increment) {
    int now = 0;
    vx_priority(0, VX_PRIORITY_GET, &now);
    long error = vx_priority(0, now + increment, &now);
    if (error) {
        errno = __vx_errno_of(error);
        return -1;
    }
    return now;
}

pid_t getpid(void) {
    return (pid_t)vx_process_id();
}

pid_t getppid(void) {
    return 1;
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

char *getlogin(void) {
    return "user";
}

int gethostname(char *name, size_t size) {
    return (int)__vx_errno_result(vx_get_hostname(name, size) < 0 ? -VX_EINVAL : 0);
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        struct vx_system_info info;
        return vx_system_info(&info) ? 1 : (long)info.cpus;
    }
    case _SC_PHYS_PAGES: {
        struct vx_system_info info;
        return vx_system_info(&info) ? 0 : (long)(info.memory_total / 4096);
    }
    case _SC_CLK_TCK:
        return 100;
    case _SC_OPEN_MAX:
        return 256;
    default:
        errno = EINVAL;
        return -1;
    }
}

int getpagesize(void) {
    return 4096;
}

void _exit(int code) {
    vx_exit(code);
}

void _Exit(int code) {
    vx_exit(code);
}

/* system(): through Vexa's shell, waiting for it. */
int system(const char *command) {
    if (!command) {
        return 1;
    }
    const char *argv[] = {"vsh", "-c", command, NULL};
    unsigned long envc = 0;
    while (environ && environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {.argv = argv, .argc = 3, .envp = (const char *const *)environ,
                             .envc = envc, .handles = {0, 1, 2}};
    int process = vx_spawn("/bin/vsh", &spawn);
    if (process < 0) {
        __vx_errno_result(process);
        return -1;
    }
    long code = vx_wait(process, 0);
    vx_close(process);
    return code < 0 ? -1 : (int)((code & 0xff) << 8);
}

/* ---- Memory ---- */

void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    (void)address;
    unsigned vx = (protection & PROT_WRITE ? VX_MAP_WRITE : 0) |
                  (protection & PROT_EXEC ? VX_MAP_EXEC : 0);
    void *p;
    if (flags & MAP_ANONYMOUS) {
        p = vx_map(size, vx);
    } else if (flags & MAP_SHARED) {
        p = vx_map_file(fd, (unsigned long)offset, size, vx);
    } else {
        /* A private copy of the file. */
        p = vx_map(size, VX_MAP_WRITE);
        if (p) {
            long n = pread(fd, p, size, offset);
            if (n < 0) {
                vx_unmap(p, size);
                return MAP_FAILED;
            }
            if (!(protection & PROT_WRITE)) {
                vx_protect(p, (size + 4095) & ~4095ul, vx);
            }
        }
    }
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

int munmap(void *address, size_t size) {
    return (int)__vx_errno_result(vx_unmap(address, size));
}

int mprotect(void *address, size_t size, int protection) {
    unsigned vx = (protection & PROT_WRITE ? VX_MAP_WRITE : 0) |
                  (protection & PROT_EXEC ? VX_MAP_EXEC : 0);
    return (int)__vx_errno_result(vx_protect(address, size, vx));
}

/* ---- poll ---- */

int poll(struct pollfd *fds, nfds_t count, int timeout_ms) {
    struct vx_poll handles[64];
    if (count > 64) {
        errno = EINVAL;
        return -1;
    }
    for (nfds_t i = 0; i < count; i++) {
        handles[i].handle = fds[i].fd;
        handles[i].events = (fds[i].events & POLLIN ? VX_POLL_READ : 0) |
                            (fds[i].events & POLLOUT ? VX_POLL_WRITE : 0);
        handles[i].ready = 0;
    }
    long n = vx_poll(handles, count, timeout_ms < 0 ? -1 : timeout_ms);
    if (n < 0) {
        return (int)__vx_errno_result(n);
    }
    int ready = 0;
    for (nfds_t i = 0; i < count; i++) {
        unsigned r = handles[i].ready;
        fds[i].revents = (short)((r & VX_POLL_READ ? POLLIN : 0) |
                                 (r & VX_POLL_WRITE ? POLLOUT : 0) |
                                 (r & VX_POLL_HANGUP ? POLLHUP : 0) |
                                 (r & VX_POLL_ERROR ? POLLERR : 0));
        ready += fds[i].revents != 0;
    }
    return ready;
}

/* ---- getopt ---- */

char *optarg;
int optind = 1, opterr = 1, optopt;

int getopt(int argc, char *const argv[], const char *options) {
    static int within;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1]) {
        return -1;
    }
    if (!strcmp(argv[optind], "--")) {
        optind++;
        return -1;
    }
    if (!within) {
        within = 1;
    }
    int c = argv[optind][within];
    const char *spec = c == ':' ? NULL : strchr(options, c);
    if (!argv[optind][++within]) {
        optind++;
        within = 0;
    }
    if (!spec) {
        optopt = c;
        if (opterr && options[0] != ':') {
            fprintf(stderr, "%s: unknown option -%c\n", argv[0], c);
        }
        return '?';
    }
    if (spec[1] == ':') {
        if (within) {
            optarg = (char *)&argv[optind][within];
            optind++;
            within = 0;
        } else if (optind < argc) {
            optarg = argv[optind++];
        } else {
            optopt = c;
            if (opterr && options[0] != ':') {
                fprintf(stderr, "%s: option -%c needs a value\n", argv[0], c);
            }
            return options[0] == ':' ? ':' : '?';
        }
    }
    return c;
}
