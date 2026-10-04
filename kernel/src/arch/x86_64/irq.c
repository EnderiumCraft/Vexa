#include <stdbool.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/random.h>
#include <vexa/sched.h>
#include <vexa/signal.h>
#include <vexa/spinlock.h>
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

/* ---- Shared interrupt lines (PCI INTx, the ACPI SCI) ----
 * Each line is a GSI (or, without an APIC, an ISA IRQ) with up to
 * LINE_HANDLERS handlers; every one is asked, and says whether its device
 * had something. A line with too many interrupts nobody claims in a second
 * is masked (a device nobody handles is holding it, or it's routed wrong),
 * and its drivers keep polling. */

#define MAX_LINES 32
#define LINE_HANDLERS 8
#define UNCLAIMED_LIMIT 20000 /* In one second. */

struct irq_line {
    uint32_t gsi;
    uint8_t vector;
    bool masked;
    int count;
    shared_irq_handler_t handlers[LINE_HANDLERS];
    void *args[LINE_HANDLERS];
    uint32_t unclaimed; /* In this second. */
    uint64_t second;    /* timer_ms() / 1000 that it counts. */
};

static struct irq_line lines[MAX_LINES];
static int line_count;
static struct irq_line *line_of_vector[256];
static struct spinlock lines_lock = SPINLOCK_INIT;

static void line_dispatch(struct interrupt_frame *frame) {
    struct irq_line *line = line_of_vector[(uint8_t)frame->vector];
    if (!line) {
        return;
    }
    bool claimed = false;
    for (int i = 0; i < line->count; i++) {
        claimed |= line->handlers[i](line->args[i]);
    }
    uint64_t second = timer_ms() / 1000;
    if (second != line->second) {
        line->second = second;
        line->unclaimed = 0;
    }
    line->unclaimed += !claimed;
    if (line->unclaimed >= UNCLAIMED_LIMIT && !line->masked) {
        line->masked = true;
        if (using_apic) {
            ioapic_mask_gsi(line->gsi);
        } else {
            pic_mask((uint8_t)line->gsi);
        }
        kprintf("[irq] interrupt line %u keeps interrupting for nothing: masked (its devices "
                "are polled)\n", line->gsi);
    }
}

bool irq_attach_line(uint32_t gsi, bool level, bool active_low, shared_irq_handler_t handler,
                     void *arg) {
    if (!using_apic && gsi >= 16) {
        return false;
    }
    uint64_t flags = spin_lock_irqsave(&lines_lock);
    struct irq_line *line = NULL;
    for (int i = 0; i < line_count; i++) {
        if (lines[i].gsi == gsi) {
            line = &lines[i];
        }
    }
    bool ok = true;
    if (!line) {
        int vector = using_apic ? irq_alloc_vector() : VECTOR_ISA_BASE + (int)gsi;
        if (line_count == MAX_LINES || vector < 0 ||
            (!using_apic && irq_handlers[vector] && irq_handlers[vector] != line_dispatch)) {
            ok = false;
        } else {
            line = &lines[line_count++];
            line->gsi = gsi;
            line->vector = (uint8_t)vector;
            line_of_vector[vector] = line;
            irq_handlers[vector] = line_dispatch;
        }
    }
    if (ok && line->count == LINE_HANDLERS) {
        ok = false;
    }
    if (ok) {
        line->handlers[line->count] = handler;
        line->args[line->count] = arg;
        line->count++;
        if (line->count == 1) {
            if (using_apic) {
                ok = ioapic_route_gsi(gsi, line->vector, level, active_low);
            } else {
                if (level) {
                    pic_set_level((uint8_t)gsi);
                }
                pic_unmask((uint8_t)gsi);
            }
        }
    }
    spin_unlock_irqrestore(&lines_lock, flags);
    return ok;
}

uint32_t irq_isa_gsi(uint8_t irq, bool *level, bool *active_low) {
    return using_apic ? ioapic_isa_gsi(irq, level, active_low) : irq;
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
    random_interrupt(vector);
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
