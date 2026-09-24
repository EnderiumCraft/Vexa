#ifndef VEXA_ARCH_X86_64_LAPIC_H
#define VEXA_ARCH_X86_64_LAPIC_H

#include <stdint.h>

/* Local APIC registers shared between apic.c and timer.c. */
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

uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

#endif
