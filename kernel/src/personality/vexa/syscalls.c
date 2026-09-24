#include <stddef.h>
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/uaccess.h>

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

static const syscall_fn syscalls[] = {
    [VX_SYS_EXIT] = sys_exit,
    [VX_SYS_LOG] = sys_log,
    [VX_SYS_YIELD] = sys_yield,
    [VX_SYS_SLEEP] = sys_sleep,
    [VX_SYS_PROCESS_ID] = sys_process_id,
    [VX_SYS_UPTIME] = sys_uptime,
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
