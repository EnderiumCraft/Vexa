#include <stdint.h>
#include <vexa/arch.h>

/* Flat 64-bit segments. A TSS and user segments come later (Phase 2). */
static uint64_t gdt[] = {
    0x0000000000000000, /* Null. */
    0x00af9a000000ffff, /* 0x08: kernel code, 64-bit. */
    0x00cf92000000ffff, /* 0x10: kernel data. */
};

struct __attribute__((packed)) gdt_pointer {
    uint16_t limit;
    uint64_t base;
};

void gdt_init(void) {
    struct gdt_pointer gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)gdt,
    };
    __asm__ volatile(
        "lgdt %0\n"
        /* Reload CS with a far return, then the data segments. */
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %2, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        :
        : "m"(gdtr), "i"((uint64_t)GDT_KERNEL_CODE), "i"((uint16_t)GDT_KERNEL_DATA)
        : "rax", "memory");
}
