#include <stdbool.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/sched.h>
#include <vexa/signal.h>
#include "irqchip.h"

static irq_handler_t irq_handlers[256];
static bool using_apic;

void interrupt_controller_init(void) {
    pic_init();
    using_apic = apic_init();
    if (!using_apic) {
        kprintf("[irq] using the legacy 8259 PIC instead (one CPU only)\n");
    }
}

bool interrupt_controller_is_apic(void) {
    return using_apic;
}

uint32_t arch_cpu_count(void) {
    return using_apic ? apic_cpu_count() : 1;
}

void irq_register(uint8_t vector, irq_handler_t handler) {
    irq_handlers[vector] = handler;
}

#define FIRST_DEVICE_VECTOR 0x40
#define LAST_DEVICE_VECTOR 0xef

int irq_alloc_vector(void) {
    static int next = FIRST_DEVICE_VECTOR;
    int vector = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    return vector <= LAST_DEVICE_VECTOR ? vector : -1;
}

void isa_irq_enable(uint8_t irq, irq_handler_t handler) {
    irq_register(VECTOR_ISA_BASE + irq, handler);
    if (using_apic) {
        ioapic_route_isa_irq(irq, VECTOR_ISA_BASE + irq);
    } else {
        pic_unmask(irq);
    }
}

/* Called by interrupt_dispatch() for every vector from 32 up. */
void irq_dispatch(struct interrupt_frame *frame) {
    uint8_t vector = (uint8_t)frame->vector;
    bool isa = vector >= VECTOR_ISA_BASE && vector < VECTOR_ISA_BASE + 16;
    uint8_t irq = vector - VECTOR_ISA_BASE;

    if (vector == VECTOR_SPURIOUS) {
        return; /* Spurious APIC interrupts must not be acknowledged. */
    }
    if (!using_apic && isa && pic_is_spurious(irq)) {
        return;
    }
    if (irq_handlers[vector]) {
        irq_handlers[vector](frame);
    } else if (isa && using_apic) {
        return; /* From the masked 8259 PIC, so spurious; nothing to acknowledge. */
    } else {
        kprintf("[irq] unexpected interrupt on vector %u\n", vector);
    }

    if (using_apic) {
        lapic_eoi();
    } else if (isa) {
        pic_eoi(irq);
    }
    /* After the acknowledgement: this may switch to another thread. */
    sched_preempt_if_needed();
    if (frame->cs & 3) {
        signal_deliver(frame); /* Back to user mode: act on any signals first. */
    }
}
