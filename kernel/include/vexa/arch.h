#ifndef VEXA_ARCH_H
#define VEXA_ARCH_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

/* Interrupt vector layout. */
#define VECTOR_LEGACY_PIC_BASE 0x20 /* 0x20-0x2f: masked 8259 PIC, spurious only. */
#define VECTOR_TIMER 0x30
#define VECTOR_KEYBOARD 0x31
#define VECTOR_SPURIOUS 0xff

/* Register state pushed by the ISR stubs in isr.S. Order must match. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

void gdt_init(void);
void idt_init(void);
void irq_register(uint8_t vector, irq_handler_t handler);

/* Local APIC and I/O APIC, configured from the ACPI MADT. */
void apic_init(void);
void lapic_eoi(void);
uint32_t lapic_id(void);
uint32_t apic_cpu_count(void);
/* Routes a legacy ISA IRQ (0-15) to `vector` on this CPU. */
void ioapic_route_isa_irq(uint8_t irq, uint8_t vector);

/* Local APIC timer, calibrated against the PIT. Ticks once per millisecond. */
void timer_init(void);
uint64_t timer_ms(void);
void timer_sleep_ms(uint64_t ms);

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t read_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void interrupts_enable(void) {
    __asm__ volatile("sti");
}

static inline void interrupts_disable(void) {
    __asm__ volatile("cli");
}

/* Sleeps until the next interrupt. Interrupts must be enabled. */
static inline void cpu_wait_for_interrupt(void) {
    __asm__ volatile("hlt");
}

#endif
