#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/cpu.h>

/* Each CPU has its own GDT because each needs its own TSS. Selector order
 * matters for syscall/sysret: user data must come right before user code. */
static const uint64_t gdt_template[5] = {
    0x0000000000000000, /* Null. */
    0x00af9a000000ffff, /* 0x08: kernel code, 64-bit. */
    0x00cf92000000ffff, /* 0x10: kernel data. */
    0x00cff2000000ffff, /* 0x18: user data. */
    0x00affa000000ffff, /* 0x20: user code, 64-bit. */
    /* 0x28: TSS, a 16-byte descriptor filled in per CPU. */
};

struct __attribute__((packed)) gdt_pointer {
    uint16_t limit;
    uint64_t base;
};

void gdt_load(struct cpu *cpu, const uint64_t ist_tops[3]) {
    for (int i = 0; i < 5; i++) {
        cpu->gdt[i] = gdt_template[i];
    }
    for (int i = 0; i < 3; i++) {
        cpu->tss.ist[i] = ist_tops[i];
    }
    cpu->tss.iomap_base = sizeof(cpu->tss); /* No I/O permission bitmap. */

    uint64_t base = (uint64_t)&cpu->tss;
    uint64_t limit = sizeof(cpu->tss) - 1;
    cpu->gdt[GDT_TSS / 8] = (limit & 0xffff) | ((base & 0xffffff) << 16) |
                            (0x89ULL << 40) | /* Present, 64-bit TSS (available). */
                            (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    cpu->gdt[GDT_TSS / 8 + 1] = base >> 32;

    struct gdt_pointer gdtr = {
        .limit = sizeof(cpu->gdt) - 1,
        .base = (uint64_t)cpu->gdt,
    };
    __asm__ volatile(
        "lgdt %0\n"
        /* Reload CS with a far return, then the data segments. GS is loaded
         * with a null selector; its base is set through an MSR afterwards. */
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %2, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorl %%eax, %%eax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "ltr %w3\n"
        :
        : "m"(gdtr), "i"((uint64_t)GDT_KERNEL_CODE), "i"((uint16_t)GDT_KERNEL_DATA),
          "r"((uint16_t)GDT_TSS)
        : "rax", "memory");
}
