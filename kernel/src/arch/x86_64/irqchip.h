#ifndef VEXA_ARCH_X86_64_IRQCHIP_H
#define VEXA_ARCH_X86_64_IRQCHIP_H

#include <stdbool.h>
#include <stdint.h>

/* Interrupt controller internals shared by irq.c, apic.c, pic.c and timer.c. */

/* Legacy 8259 PIC (pic.c). */
void pic_init(void); /* Remaps IRQs to VECTOR_ISA_BASE and masks them all. */
void pic_unmask(uint8_t irq);
void pic_eoi(uint8_t irq);
bool pic_is_spurious(uint8_t irq);

/* Local APIC and I/O APIC (apic.c). */
bool apic_init(void); /* Returns false if the MADT or an I/O APIC is missing. */
uint32_t apic_cpu_count(void);
void ioapic_route_isa_irq(uint8_t irq, uint8_t vector);
void lapic_eoi(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

#define LAPIC_ID 0x20
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xb0
#define LAPIC_SVR 0xf0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE 0x3e0

#define LAPIC_LVT_MASKED (1U << 16)
#define LAPIC_TIMER_PERIODIC (1U << 17)

#endif
