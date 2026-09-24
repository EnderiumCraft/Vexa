#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>

struct __attribute__((packed)) idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct __attribute__((packed)) idt_pointer {
    uint16_t limit;
    uint64_t base;
};

static struct idt_entry idt[256];

/* Defined in isr.S: one stub per CPU exception vector 0-31. */
extern uint64_t isr_stub_table[32];

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND Range Exceeded",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security", "Reserved",
};

static void idt_set_gate(uint8_t vector, uint64_t handler) {
    idt[vector] = (struct idt_entry){
        .offset_low = handler & 0xffff,
        .selector = GDT_KERNEL_CODE,
        .ist = 0,
        .type_attr = 0x8e, /* Present, ring 0, 64-bit interrupt gate. */
        .offset_mid = (handler >> 16) & 0xffff,
        .offset_high = handler >> 32,
    };
}

void idt_init(void) {
    for (uint8_t v = 0; v < 32; v++) {
        idt_set_gate(v, isr_stub_table[v]);
    }
    struct idt_pointer idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

/* Called from isr_common in isr.S. */
void interrupt_dispatch(struct interrupt_frame *frame) {
    if (frame->vector == 3) {
        kprintf("[idt] breakpoint at rip=%p, resuming\n", (void *)frame->rip);
        return;
    }

    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    panic("CPU exception %lu (%s)\n"
          "  error=%lx rip=%p cs=%lx rflags=%lx\n"
          "  rsp=%p cr2=%p\n"
          "  rax=%lx rbx=%lx rcx=%lx rdx=%lx",
          frame->vector, exception_names[frame->vector & 31],
          frame->error_code, (void *)frame->rip, frame->cs, frame->rflags,
          (void *)frame->rsp, (void *)cr2,
          frame->rax, frame->rbx, frame->rcx, frame->rdx);
}
