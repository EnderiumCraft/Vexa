#ifndef VEXA_ARCH_H
#define VEXA_ARCH_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

void gdt_init(void);
void idt_init(void);

/* Register state pushed by the ISR stubs in isr.S. Order must match. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

#endif
