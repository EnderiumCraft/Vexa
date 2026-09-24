#include <string.h>
#include <vexa/syscall.h>

/* The syscall instruction clobbers rcx and r11; the kernel may read memory
 * passed by pointer, hence the "memory" clobber. */
static inline long syscall1(long number, long a0) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a0) : "rcx", "r11", "memory");
    return result;
}

static inline long syscall2(long number, long a0, long a1) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory");
    return result;
}

static inline long syscall3(long number, long a0, long a1, long a2) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return result;
}

void vx_exit(int code) {
    syscall1(VX_SYS_EXIT, code);
    __builtin_unreachable();
}

long vx_log(const char *text, size_t length) {
    return syscall2(VX_SYS_LOG, (long)text, (long)length);
}

long vx_yield(void) {
    return syscall1(VX_SYS_YIELD, 0);
}

long vx_sleep(uint64_t ms) {
    return syscall1(VX_SYS_SLEEP, (long)ms);
}

long vx_process_id(void) {
    return syscall1(VX_SYS_PROCESS_ID, 0);
}

long vx_uptime(void) {
    return syscall1(VX_SYS_UPTIME, 0);
}

int vx_open(const char *path, unsigned flags) {
    return (int)syscall3(VX_SYS_OPEN, (long)path, (long)strlen(path), flags);
}

long vx_close(int handle) {
    return syscall1(VX_SYS_CLOSE, handle);
}

long vx_read(int handle, void *buffer, size_t size) {
    return syscall3(VX_SYS_READ, handle, (long)buffer, (long)size);
}

long vx_write(int handle, const void *buffer, size_t size) {
    return syscall3(VX_SYS_WRITE, handle, (long)buffer, (long)size);
}

long vx_seek(int handle, long offset, int whence) {
    return syscall3(VX_SYS_SEEK, handle, offset, whence);
}

long vx_stat(const char *path, struct vx_stat *stat) {
    return syscall3(VX_SYS_STAT, (long)path, (long)strlen(path), (long)stat);
}

long vx_handle_stat(int handle, struct vx_stat *stat) {
    return syscall2(VX_SYS_HANDLE_STAT, handle, (long)stat);
}

long vx_read_dir(int handle, struct vx_dir_entry *entries, size_t count) {
    return syscall3(VX_SYS_READ_DIR, handle, (long)entries, (long)count);
}

long vx_mkdir(const char *path) {
    return syscall2(VX_SYS_MKDIR, (long)path, (long)strlen(path));
}

long vx_remove(const char *path) {
    return syscall2(VX_SYS_REMOVE, (long)path, (long)strlen(path));
}
