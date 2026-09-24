#include <stdint.h>
#include <vexa/arch.h>

/* Task state segment. In long mode it only holds stack pointers: rsp0 for
 * entering the kernel from user mode (Phase 3), and the interrupt stack table
 * (IST) used for exceptions that must not run on a possibly broken stack. */
struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

#define IST_STACK_SIZE 16384

static struct tss tss;
static uint8_t ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));

/* Selector order matters for syscall/sysret (Phase 3): user data must come
 * right before user code. */
static uint64_t gdt[] = {
    0x0000000000000000, /* Null. */
    0x00af9a000000ffff, /* 0x08: kernel code, 64-bit. */
    0x00cf92000000ffff, /* 0x10: kernel data. */
    0x00cff2000000ffff, /* 0x18: user data. */
    0x00affa000000ffff, /* 0x20: user code, 64-bit. */
    0, 0,               /* 0x28: TSS (a 16-byte descriptor, filled in below). */
};

struct __attribute__((packed)) gdt_pointer {
    uint16_t limit;
    uint64_t base;
};

static void set_tss_descriptor(void) {
    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[GDT_TSS / 8] = (limit & 0xffff) | ((base & 0xffffff) << 16) |
                       (0x89ULL << 40) | /* Present, 64-bit TSS (available). */
                       (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[GDT_TSS / 8 + 1] = base >> 32;
}

void gdt_init(void) {
    for (int i = 0; i < 3; i++) {
        tss.ist[i] = (uint64_t)&ist_stacks[i][IST_STACK_SIZE];
    }
    tss.iomap_base = sizeof(tss); /* No I/O permission bitmap. */
    set_tss_descriptor();

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
        "ltr %w3\n"
        :
        : "m"(gdtr), "i"((uint64_t)GDT_KERNEL_CODE), "i"((uint16_t)GDT_KERNEL_DATA),
          "r"((uint16_t)GDT_TSS)
        : "rax", "memory");
}
