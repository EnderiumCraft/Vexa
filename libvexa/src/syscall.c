#include <string.h>
#include <vexa/syscall.h>

/* The syscall instruction clobbers rcx and r11; the kernel may read or write
 * memory passed by pointer, hence the "memory" clobber. */
static inline long syscall4(long number, long a0, long a1, long a2, long a3) {
    long result;
    register long r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

#define syscall0(n) syscall4((n), 0, 0, 0, 0)
#define syscall1(n, a) syscall4((n), (long)(a), 0, 0, 0)
#define syscall2(n, a, b) syscall4((n), (long)(a), (long)(b), 0, 0)
#define syscall3(n, a, b, c) syscall4((n), (long)(a), (long)(b), (long)(c), 0)

void vx_exit(int code) {
    syscall1(VX_SYS_EXIT, code);
    __builtin_unreachable();
}

long vx_log(const char *text, size_t length) {
    return syscall2(VX_SYS_LOG, text, length);
}

long vx_yield(void) {
    return syscall0(VX_SYS_YIELD);
}

long vx_sleep(uint64_t ms) {
    return syscall1(VX_SYS_SLEEP, ms);
}

long vx_process_id(void) {
    return syscall0(VX_SYS_PROCESS_ID);
}

long vx_uptime(void) {
    return syscall0(VX_SYS_UPTIME);
}

int vx_open(const char *path, unsigned flags) {
    return (int)syscall3(VX_SYS_OPEN, path, strlen(path), flags);
}

long vx_close(int handle) {
    return syscall1(VX_SYS_CLOSE, handle);
}

long vx_read(int handle, void *buffer, size_t size) {
    return syscall3(VX_SYS_READ, handle, buffer, size);
}

long vx_write(int handle, const void *buffer, size_t size) {
    return syscall3(VX_SYS_WRITE, handle, buffer, size);
}

long vx_seek(int handle, long offset, int whence) {
    return syscall3(VX_SYS_SEEK, handle, offset, whence);
}

long vx_stat(const char *path, struct vx_stat *stat) {
    return syscall3(VX_SYS_STAT, path, strlen(path), stat);
}

long vx_handle_stat(int handle, struct vx_stat *stat) {
    return syscall2(VX_SYS_HANDLE_STAT, handle, stat);
}

long vx_read_dir(int handle, struct vx_dir_entry *entries, size_t count) {
    return syscall3(VX_SYS_READ_DIR, handle, entries, count);
}

long vx_mkdir(const char *path) {
    return syscall2(VX_SYS_MKDIR, path, strlen(path));
}

long vx_remove(const char *path) {
    return syscall2(VX_SYS_REMOVE, path, strlen(path));
}

long vx_rename(const char *from, const char *to) {
    return syscall4(VX_SYS_RENAME, (long)from, (long)strlen(from), (long)to, (long)strlen(to));
}

long vx_symlink(const char *target, const char *path) {
    return syscall4(VX_SYS_SYMLINK, (long)target, (long)strlen(target), (long)path,
                    (long)strlen(path));
}

long vx_readlink(const char *path, char *buffer, size_t size) {
    return syscall4(VX_SYS_READLINK, (long)path, (long)strlen(path), (long)buffer, (long)size);
}

long vx_lstat(const char *path, struct vx_stat *stat) {
    return syscall3(VX_SYS_LSTAT, path, strlen(path), stat);
}

long vx_thread_start(const struct vx_thread_start *start) {
    return syscall1(VX_SYS_THREAD_CREATE, start);
}

void vx_thread_exit(int code) {
    syscall1(VX_SYS_THREAD_EXIT, code);
    __builtin_unreachable();
}

long vx_thread_id(void) {
    return syscall0(VX_SYS_THREAD_ID);
}

long vx_wait_address(volatile unsigned *address, unsigned expected, long timeout_ms) {
    return syscall3(VX_SYS_WAIT_ADDRESS, address, expected, timeout_ms);
}

long vx_wake_address(volatile unsigned *address, long count) {
    return syscall2(VX_SYS_WAKE_ADDRESS, address, count);
}

long vx_protect(void *address, size_t size, unsigned flags) {
    return syscall3(VX_SYS_PROTECT, address, size, flags);
}

long vx_chdir(const char *path) {
    return syscall2(VX_SYS_CHDIR, path, strlen(path));
}

long vx_getcwd(char *buffer, size_t size) {
    return syscall2(VX_SYS_GETCWD, buffer, size);
}

long vx_pipe(int handles[2]) {
    return syscall1(VX_SYS_PIPE, handles);
}

int vx_spawn(const char *path, const struct vx_spawn *spawn) {
    return (int)syscall3(VX_SYS_SPAWN, path, strlen(path), spawn);
}

long vx_wait(int process, unsigned flags) {
    return syscall2(VX_SYS_WAIT, process, flags);
}

long vx_handle_process_id(int process) {
    return syscall1(VX_SYS_HANDLE_PROCESS_ID, process);
}

long vx_kill(long process_id, int signal) {
    return syscall2(VX_SYS_KILL, process_id, signal);
}

long vx_signal(int signal, int action) {
    return syscall2(VX_SYS_SIGNAL, signal, action);
}

long vx_set_foreground(long group) {
    return syscall1(VX_SYS_SET_FOREGROUND, group);
}

long vx_process_list(struct vx_process_info *entries, size_t count) {
    return syscall2(VX_SYS_PROCESS_LIST, entries, count);
}

void *vx_map(size_t size, unsigned flags) {
    long result = syscall2(VX_SYS_MAP, size, flags);
    return result < 0 ? NULL : (void *)result;
}

long vx_unmap(void *address, size_t size) {
    return syscall2(VX_SYS_UNMAP, address, size);
}

long vx_system_info(struct vx_system_info *info) {
    return syscall1(VX_SYS_SYSTEM_INFO, info);
}

long vx_kernel_command(const char *command) {
    return syscall2(VX_SYS_KERNEL_COMMAND, command, strlen(command));
}

int vx_socket(int family, int type, int protocol) {
    return (int)syscall3(VX_SYS_SOCKET, family, type, protocol);
}

long vx_bind(int handle, const struct vx_socket_address *address, size_t length) {
    return syscall3(VX_SYS_BIND, handle, address, length);
}

long vx_listen(int handle, int backlog) {
    return syscall2(VX_SYS_LISTEN, handle, backlog);
}

int vx_accept(int handle, struct vx_socket_address *peer, unsigned flags) {
    return (int)syscall3(VX_SYS_ACCEPT, handle, peer, flags);
}

long vx_connect(int handle, const struct vx_socket_address *address, size_t length) {
    return syscall3(VX_SYS_CONNECT, handle, address, length);
}

long vx_send(int handle, struct vx_message *message) {
    return syscall2(VX_SYS_SEND, handle, message);
}

long vx_receive(int handle, struct vx_message *message) {
    return syscall2(VX_SYS_RECEIVE, handle, message);
}

long vx_shutdown(int handle, int how) {
    return syscall2(VX_SYS_SHUTDOWN, handle, how);
}

long vx_socket_address(int handle, int peer, struct vx_socket_address *address) {
    return syscall3(VX_SYS_SOCKET_ADDRESS, handle, peer, address);
}

long vx_socket_pair(int family, int type, int handles[2]) {
    return syscall3(VX_SYS_SOCKET_PAIR, family, type, handles);
}

long vx_poll(struct vx_poll *handles, size_t count, long timeout_ms) {
    return syscall3(VX_SYS_POLL, handles, count, timeout_ms);
}

long vx_net_info(struct vx_net_interface *interfaces, size_t count) {
    return syscall2(VX_SYS_NET_INFO, interfaces, count);
}

long vx_control(int handle, unsigned request, void *arg, size_t size) {
    return syscall4(VX_SYS_CONTROL, handle, request, (long)arg, (long)size);
}

void *vx_map_file(int handle, unsigned long offset, size_t size, unsigned flags) {
    long result = syscall4(VX_SYS_MAP_FILE, handle, (long)offset, (long)size, flags);
    return result < 0 ? NULL : (void *)result;
}

long vx_resize(int handle, unsigned long size) {
    return syscall2(VX_SYS_RESIZE, handle, size);
}

const char *vx_strerror(long error) {
    switch (-error) {
    case VX_ENOSYS: return "not supported";
    case VX_EFAULT: return "bad address";
    case VX_EINVAL: return "invalid argument";
    case VX_ENOENT: return "no such file or directory";
    case VX_EEXIST: return "already exists";
    case VX_ENOTDIR: return "not a directory";
    case VX_EISDIR: return "is a directory";
    case VX_ENOTEMPTY: return "directory not empty";
    case VX_EBADF: return "bad handle";
    case VX_EACCES: return "not allowed";
    case VX_ENOSPC: return "no space left";
    case VX_EIO: return "I/O error";
    case VX_ENAMETOOLONG: return "name too long";
    case VX_EMFILE: return "too many open handles";
    case VX_ENOMEM: return "out of memory";
    case VX_EROFS: return "read-only file system";
    case VX_EBUSY: return "busy";
    case VX_EXDEV: return "on a different disk";
    case VX_EINTR: return "interrupted";
    case VX_EPIPE: return "broken pipe";
    case VX_ECHILD: return "no child process";
    case VX_ESRCH: return "no such process";
    case VX_EAGAIN: return "try again";
    case VX_ENOEXEC: return "not a program Vexa can run";
    case VX_E2BIG: return "arguments too long";
    case VX_ENOTTY: return "not a terminal";
    case VX_ESPIPE: return "can't seek here";
    case VX_ELOOP: return "too many symbolic links";
    case VX_ETIMEDOUT: return "timed out";
    case VX_ENOTSOCK: return "not a socket";
    case VX_EAFNOSUPPORT: return "address family not supported";
    case VX_EPROTONOSUPPORT: return "protocol not supported";
    case VX_EOPNOTSUPP: return "not supported by this socket";
    case VX_EADDRINUSE: return "address in use";
    case VX_EADDRNOTAVAIL: return "address not available";
    case VX_ENETUNREACH: return "network unreachable";
    case VX_ECONNREFUSED: return "connection refused";
    case VX_ECONNRESET: return "connection reset";
    case VX_ENOTCONN: return "not connected";
    case VX_EISCONN: return "already connected";
    case VX_EINPROGRESS: return "connecting";
    case VX_EALREADY: return "already connecting";
    case VX_EMSGSIZE: return "message too long";
    case VX_EDESTADDRREQ: return "no destination";
    case VX_ENOPROTOOPT: return "no such option";
    case VX_ECONNABORTED: return "connection aborted";
    case VX_EHOSTUNREACH: return "host unreachable";
    default: return "error";
    }
}
