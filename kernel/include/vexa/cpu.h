#ifndef VEXA_CPU_H
#define VEXA_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CPUS 64

struct thread;

/* Task state segment. In long mode it only holds stack pointers: rsp[0] for
 * entering the kernel from user mode, and the interrupt stack table (IST)
 * for exceptions that must not run on a possibly broken stack. */
struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

/* Per-CPU data. The GS segment base points at this CPU's struct while in the
 * kernel, so cpu_current() is a single instruction. */
struct cpu {
    struct cpu *self;        /* Offset 0: read by cpu_current(). */
    uint64_t user_rsp;       /* Offset 8: syscall entry stashes the user stack here. */
    uint64_t kernel_rsp;     /* Offset 16: top of the current thread's kernel stack. */
    struct thread *current;
    struct thread *idle;
    struct thread *prev;     /* The thread we just switched away from. */
    uint32_t id;             /* 0 = the bootstrap CPU. */
    uint32_t lapic_id;
    volatile bool online;
    volatile bool need_resched;
    uint32_t slice_left;     /* Timer ticks before the current thread is preempted. */
    uint64_t idle_ticks;
    uint64_t busy_ticks;
    struct address_space *active_as; /* Whose page tables are loaded (NULL: the kernel's). */
    volatile bool tlb_flush_pending; /* Another CPU changed page tables we may cache. */
    uint64_t gdt[7];
    struct tss tss;
};

/* Offsets used from assembly (syscall.S). */
#define CPU_USER_RSP 8
#define CPU_KERNEL_RSP 16

extern struct cpu cpus[MAX_CPUS];

/* Makes cpu_current() work on this CPU. The very first thing a CPU does:
 * locks and page table switches use it. */
void set_gs_base(struct cpu *cpu);

static inline struct cpu *cpu_current(void) {
    struct cpu *cpu;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

/* Sets up the bootstrap CPU: its GDT, TSS and GS base. Called early in boot. */
void cpu_init_bsp(void);
/* Sets up an application processor's descriptor tables, GS base and features. */
void cpu_init_ap(struct cpu *cpu);
/* Enables CPU features every core needs (FPU/SSE, PAT, SMEP/SMAP, syscall). */
void cpu_enable_features(struct cpu *cpu);
uint32_t cpu_online_count(void);

#endif
