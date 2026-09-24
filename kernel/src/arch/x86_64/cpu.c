#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/fpu.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/uaccess.h>

#define IA32_PAT_MSR 0x277
#define IA32_GS_BASE_MSR 0xc0000101
#define IA32_KERNEL_GS_BASE_MSR 0xc0000102

/* Page attribute table: the same layout Limine promises, set explicitly.
 * Index: 0 WB, 1 WT, 2 UC-, 3 UC, 4 WP, 5 WC, 6 UC-, 7 UC. */
#define PAT_LAYOUT 0x0007010500070406ULL

#define IA32_EFER_MSR 0xc0000080
#define EFER_NXE (1ULL << 11)
#define CR0_WP (1ULL << 16)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

#define IST_STACK_SIZE 16384

struct cpu cpus[MAX_CPUS];
bool smap_enabled;

/* The bootstrap CPU's exception stacks, needed before memory management works. */
static uint8_t bsp_ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));

void gdt_load(struct cpu *cpu, const uint64_t ist_tops[3]); /* gdt.c */
void idt_load(void);                                        /* idt.c */
void syscall_init_cpu(void);                                /* syscall.c */

void cpu_enable_features(struct cpu *cpu) {
    uint32_t a, b, c, d;
    /* The page tables use no-execute bits, and read-only pages must also stop
     * the kernel: set both explicitly rather than trust the bootloader did. */
    cpuid(0x80000001, &a, &b, &c, &d);
    if (d & (1U << 20)) {
        wrmsr(IA32_EFER_MSR, rdmsr(IA32_EFER_MSR) | EFER_NXE);
    }
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | CR0_WP));

    cpuid(1, &a, &b, &c, &d);
    if (d & (1U << 16)) {
        wrmsr(IA32_PAT_MSR, PAT_LAYOUT);
    }
    fpu_init_cpu();

    /* SMEP stops the kernel from running user memory as code; SMAP stops it
     * from reading or writing user memory except inside user_access_begin/end. */
    cpuid(7, &a, &b, &c, &d);
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (b & (1U << 7)) {
        cr4 |= CR4_SMEP;
    }
    if (b & (1U << 20)) {
        cr4 |= CR4_SMAP;
        if (cpu->id == 0) {
            smap_enabled = true;
        }
    }
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    syscall_init_cpu();
}

static void set_gs_base(struct cpu *cpu) {
    cpu->self = cpu;
    wrmsr(IA32_GS_BASE_MSR, (uint64_t)cpu);
    wrmsr(IA32_KERNEL_GS_BASE_MSR, 0); /* The user's GS base, swapped in by swapgs. */
}

void cpu_init_bsp(void) {
    struct cpu *cpu = &cpus[0];
    cpu->id = 0;
    uint64_t ist_tops[3];
    for (int i = 0; i < 3; i++) {
        ist_tops[i] = (uint64_t)&bsp_ist_stacks[i][IST_STACK_SIZE];
    }
    gdt_load(cpu, ist_tops);
    idt_load();
    set_gs_base(cpu);
    cpu_enable_features(cpu);
    cpu->online = true;
}

void cpu_init_ap(struct cpu *cpu) {
    uint64_t ist_tops[3];
    for (int i = 0; i < 3; i++) {
        ist_tops[i] = vmm_alloc_kernel_stack(IST_STACK_SIZE);
    }
    gdt_load(cpu, ist_tops);
    idt_load();
    set_gs_base(cpu);
    cpu_enable_features(cpu);
}

/* Called by the scheduler just before switching stacks from prev to next. */
void arch_prepare_switch(struct cpu *cpu, struct thread *prev, struct thread *next) {
    if (prev->fpu_state) {
        fpu_save(prev->fpu_state);
    }
    if (next->fpu_state) {
        fpu_restore(next->fpu_state);
    }
    vmm_activate(next->process ? next->process->address_space : NULL);
    /* Where the CPU switches stacks on entry from user mode (interrupts use the
     * TSS, system calls read kernel_rsp). */
    cpu->kernel_rsp = next->stack_top;
    cpu->tss.rsp[0] = next->stack_top;
}

uint32_t cpu_online_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        count += cpus[i].online;
    }
    return count;
}
