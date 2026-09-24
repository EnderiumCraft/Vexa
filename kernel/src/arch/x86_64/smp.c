#include <vexa/arch.h>
#include <vexa/cmdline.h>
#include <vexa/cpu.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/sched.h>
#include <vexa/smp.h>
#include "irqchip.h"

#define AP_STACK_SIZE (64 * 1024)
#define AP_START_TIMEOUT_MS 2000

static volatile uint32_t aps_ready;
static volatile bool smp_active;

__attribute__((noreturn)) static void halt_this_cpu(struct interrupt_frame *frame) {
    (void)frame;
    cpu_halt_forever();
}

/* Runs on the application processor, on a Vexa kernel stack. */
__attribute__((noreturn)) static void ap_main(struct cpu *cpu) {
    cpu_init_ap(cpu);
    lapic_init_ap();
    sched_init_cpu(cpu);
    timer_init_ap();
    cpu->online = true;
    __atomic_add_fetch(&aps_ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        __asm__ volatile("sti; hlt"); /* This CPU's idle thread. */
    }
}

/* Where Limine sends each application processor: still on the bootloader's
 * stack and page tables, both of which are about to be reclaimed. */
static void ap_entry(struct limine_mp_info *info) {
    struct cpu *cpu = (struct cpu *)info->extra_argument;
    vmm_activate(NULL);
    uint64_t stack = vmm_alloc_kernel_stack(AP_STACK_SIZE);
    __asm__ volatile(
        "mov %0, %%rsp\n"
        "xor %%ebp, %%ebp\n"
        "call *%1\n"
        :
        : "r"(stack), "r"(ap_main), "D"(cpu)
        : "memory");
    __builtin_unreachable();
}

void smp_start(struct limine_mp_response *mp) {
    cpus[0].lapic_id = mp ? mp->bsp_lapic_id : 0;
    if (!mp || mp->cpu_count <= 1) {
        return;
    }
    if (!interrupt_controller_is_apic()) {
        kprintf("[smp] using 1 of %lu CPUs: other CPUs need an APIC\n", mp->cpu_count);
        return;
    }
    if (cmdline_has("nosmp")) {
        kprintf("[smp] using 1 of %lu CPUs: disabled by nosmp\n", mp->cpu_count);
        return;
    }
    irq_register(VECTOR_HALT, halt_this_cpu);

    uint32_t started = 0;
    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id) {
            continue;
        }
        if (started + 1 >= MAX_CPUS) {
            kprintf("[smp] only %d CPUs are supported; ignoring the rest\n", MAX_CPUS);
            break;
        }
        struct cpu *cpu = &cpus[++started];
        cpu->id = started;
        cpu->lapic_id = info->lapic_id;
        info->extra_argument = (uint64_t)cpu;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    uint64_t deadline = timer_ms() + AP_START_TIMEOUT_MS;
    while (__atomic_load_n(&aps_ready, __ATOMIC_ACQUIRE) < started && timer_ms() < deadline) {
        __asm__ volatile("pause");
    }
    if (aps_ready < started) {
        /* A CPU that never reported may still be on bootloader memory, which
         * the kernel is about to reuse. Better to stop here than corrupt it. */
        panic("smp: only %u of %u CPUs started", aps_ready, started);
    }
    smp_active = true;
    kprintf("[smp] %u CPUs online\n", started + 1);
}

void smp_stop_other_cpus(void) {
    if (smp_active) {
        lapic_send_ipi_all_but_self(VECTOR_HALT);
    }
}
