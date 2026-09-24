#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>

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

/* Defined in isr.S: one entry stub per vector. */
extern uint64_t isr_stub_table[256];

void irq_dispatch(struct interrupt_frame *frame); /* irq.c */

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND Range Exceeded",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security", "Reserved",
};

static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist) {
    idt[vector] = (struct idt_entry){
        .offset_low = handler & 0xffff,
        .selector = GDT_KERNEL_CODE,
        .ist = ist,
        .type_attr = 0x8e, /* Present, ring 0, 64-bit interrupt gate. */
        .offset_mid = (handler >> 16) & 0xffff,
        .offset_high = handler >> 32,
    };
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) {
        idt_set_gate((uint8_t)v, isr_stub_table[v], 0);
    }
    /* These can happen when the current stack is unusable (a double fault is
     * how a kernel stack overflow ends), so they switch to their own stacks. */
    idt_set_gate(2, isr_stub_table[2], IST_NMI);
    idt_set_gate(8, isr_stub_table[8], IST_DOUBLE_FAULT);
    idt_set_gate(18, isr_stub_table[18], IST_MACHINE_CHECK);
    struct idt_pointer idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

/* Page fault error code bits: present, write, user, reserved bit, instruction fetch. */
static const char *page_fault_description(uint64_t error) {
    if (error & (1 << 3)) {
        return "page fault: reserved bit set in a page table entry\n";
    }
    if (error & (1 << 4)) {
        return error & 1 ? "page fault: executing a no-execute page\n"
                         : "page fault: executing an unmapped address\n";
    }
    if (error & (1 << 1)) {
        return error & 1 ? "page fault: writing a read-only page\n"
                         : "page fault: writing an unmapped address\n";
    }
    return error & 1 ? "page fault: reading a protected page\n"
                     : "page fault: reading an unmapped address\n";
}

/* Called from isr_common in isr.S. */
void interrupt_dispatch(struct interrupt_frame *frame) {
    if (frame->vector >= 32) {
        irq_dispatch(frame);
        return;
    }
    if (frame->vector == 3) {
        kprintf("[idt] breakpoint at rip=%p, resuming\n", (void *)frame->rip);
        return;
    }

    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    const char *detail = "";
    if ((frame->vector == 14 || frame->vector == 8) && vmm_is_stack_guard(cr2)) {
        detail = "kernel stack overflow (hit the guard page)\n";
    } else if (frame->vector == 14) {
        detail = page_fault_description(frame->error_code);
    }
    panic("CPU exception %lu (%s)\n%s"
          "  error=%lx rip=%p cs=%lx rflags=%lx\n"
          "  rsp=%p cr2=%p\n"
          "  rax=%lx rbx=%lx rcx=%lx rdx=%lx",
          frame->vector, exception_names[frame->vector & 31], detail,
          frame->error_code, (void *)frame->rip, frame->cs, frame->rflags,
          (void *)frame->rsp, (void *)cr2,
          frame->rax, frame->rbx, frame->rcx, frame->rdx);
}
