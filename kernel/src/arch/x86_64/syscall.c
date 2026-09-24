#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/kprintf.h>
#include <vexa/process.h>

#define IA32_EFER_MSR 0xc0000080
#define IA32_STAR_MSR 0xc0000081
#define IA32_LSTAR_MSR 0xc0000082
#define IA32_FMASK_MSR 0xc0000084
#define EFER_SCE (1ULL << 0)

/* Flags cleared on entry: interrupts, trap, direction, alignment check, nested task. */
#define SYSCALL_FLAGS_MASK 0x44700

void syscall_entry(void);                                               /* syscall.S */
__attribute__((noreturn)) void return_to_user(struct interrupt_frame *f); /* syscall.S */

void syscall_init_cpu(void) {
    wrmsr(IA32_EFER_MSR, rdmsr(IA32_EFER_MSR) | EFER_SCE);
    /* syscall loads CS/SS from bits 32-47; sysret would use bits 48-63 (+16 for
     * CS, +8 for SS), which is why user data precedes user code in the GDT. */
    wrmsr(IA32_STAR_MSR, ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(IA32_LSTAR_MSR, (uint64_t)syscall_entry);
    wrmsr(IA32_FMASK_MSR, SYSCALL_FLAGS_MASK);
}

void syscall_dispatch(struct interrupt_frame *frame) {
    struct process *process = process_current();
    if (!process) {
        panic("system call from a thread without a process");
    }
    process->personality->syscall(frame);
}

void enter_user_mode(uint64_t entry, uint64_t stack) {
    struct interrupt_frame frame = {
        .rip = entry,
        .cs = GDT_USER_CODE | 3,
        .rflags = RFLAGS_INTERRUPTS_ON,
        .rsp = stack,
        .ss = GDT_USER_DATA | 3,
    };
    interrupts_disable();
    return_to_user(&frame);
}
