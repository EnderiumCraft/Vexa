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
