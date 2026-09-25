#include <vexa/arch.h>
#include <vexa/cpu.h>
#include <vexa/mm.h>
#include "irqchip.h"

/*
 * TLB shootdowns. Each CPU caches page table entries in its TLB; when a
 * process's mappings change (memory unmapped, made read-only, copied on
 * write), other CPUs running threads of that process must drop what they
 * cached before the old pages are reused.
 *
 * Every address space records which CPUs have it loaded (vmm_activate keeps
 * that up to date). tlb_shootdown() flags those CPUs, interrupts them, and
 * waits until each has reloaded CR3. A CPU answers from the interrupt or,
 * with interrupts off, from any spinlock it is spinning on, and the waiting
 * CPU answers its own requests too, so shootdowns can't deadlock.
 */

volatile uint32_t tlb_requests_possible; /* Set once other CPUs run. */

void tlb_service(void) {
    struct cpu *cpu = cpu_current();
    if (cpu->tlb_flush_pending) {
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax", "memory");
        __atomic_store_n(&cpu->tlb_flush_pending, false, __ATOMIC_RELEASE);
    }
}

static void tlb_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    tlb_service();
}

void tlb_init(void) {
    irq_register(VECTOR_TLB, tlb_interrupt);
    __atomic_store_n(&tlb_requests_possible, 1, __ATOMIC_RELEASE);
}

void tlb_shootdown(struct address_space *as) {
    if (!tlb_requests_possible) {
        return;
    }
    struct cpu *self = cpu_current();
    uint64_t mask = __atomic_load_n(&as->active_cpus, __ATOMIC_ACQUIRE) & ~(1ULL << self->id);
    if (!mask) {
        return; /* Only this CPU uses it: the caller's invlpg was enough. */
    }
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if ((mask >> i) & 1) {
            __atomic_store_n(&cpus[i].tlb_flush_pending, true, __ATOMIC_RELEASE);
        }
    }
    lapic_send_ipi_all_but_self(VECTOR_TLB);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        while (((mask >> i) & 1) && __atomic_load_n(&cpus[i].tlb_flush_pending, __ATOMIC_ACQUIRE)) {
            tlb_service(); /* Someone may be waiting for us at the same time. */
            __asm__ volatile("pause");
        }
    }
}
