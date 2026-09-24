#include <stdbool.h>
#include <vexa/acpi.h>
#include <vexa/arch.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include "lapic.h"

#define IA32_APIC_BASE_MSR 0x1b
#define IA32_APIC_BASE_ENABLE (1ULL << 11)


#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_VER 0x01
#define IOAPIC_REDTBL(n) (0x10 + 2 * (n))
#define IOAPIC_MASKED (1U << 16)
#define IOAPIC_ACTIVE_LOW (1U << 13)
#define IOAPIC_LEVEL (1U << 15)

#define MADT_LOCAL_APIC 0
#define MADT_IO_APIC 1
#define MADT_SOURCE_OVERRIDE 2
#define MADT_LOCAL_APIC_OVERRIDE 5

#define MAX_IOAPICS 8
#define MAX_OVERRIDES 16

struct __attribute__((packed)) madt {
    struct acpi_sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t entries[];
};

struct __attribute__((packed)) madt_entry {
    uint8_t type;
    uint8_t length;
};

struct __attribute__((packed)) madt_local_apic {
    struct madt_entry entry;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
};

struct __attribute__((packed)) madt_io_apic {
    struct madt_entry entry;
    uint8_t id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
};

struct __attribute__((packed)) madt_source_override {
    struct madt_entry entry;
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

struct __attribute__((packed)) madt_local_apic_override {
    struct madt_entry entry;
    uint16_t reserved;
    uint64_t address;
};

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    uint32_t gsi_count;
};

struct source_override {
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
};

static volatile uint32_t *lapic;
static struct ioapic ioapics[MAX_IOAPICS];
static int ioapic_count;
static struct source_override overrides[MAX_OVERRIDES];
static int override_count;
static uint32_t cpu_count;

uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
    lapic[reg / 4] = value;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

uint32_t apic_cpu_count(void) {
    return cpu_count;
}

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg) {
    io->regs[IOAPIC_REGSEL / 4] = reg;
    return io->regs[IOAPIC_WINDOW / 4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t value) {
    io->regs[IOAPIC_REGSEL / 4] = reg;
    io->regs[IOAPIC_WINDOW / 4] = value;
}

/* Remaps the legacy 8259 PICs away from the exception vectors, then masks them.
 * All interrupts go through the APICs instead. */
static void pic_disable(void) {
    outb(0x20, 0x11); /* ICW1: initialize, expect ICW4. */
    outb(0xa0, 0x11);
    outb(0x21, VECTOR_LEGACY_PIC_BASE); /* ICW2: vector offsets. */
    outb(0xa1, VECTOR_LEGACY_PIC_BASE + 8);
    outb(0x21, 0x04); /* ICW3: slave on IRQ 2. */
    outb(0xa1, 0x02);
    outb(0x21, 0x01); /* ICW4: 8086 mode. */
    outb(0xa1, 0x01);
    outb(0x21, 0xff); /* Mask everything. */
    outb(0xa1, 0xff);
}

static void parse_madt(const struct madt *madt, uint64_t *lapic_phys) {
    *lapic_phys = madt->lapic_address;
    const uint8_t *p = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;
    while (p + sizeof(struct madt_entry) <= end) {
        const struct madt_entry *entry = (const struct madt_entry *)p;
        if (entry->length < sizeof(struct madt_entry)) {
            break;
        }
        switch (entry->type) {
        case MADT_LOCAL_APIC: {
            const struct madt_local_apic *e = (const void *)entry;
            if (e->flags & 0x3) { /* Enabled, or can be brought online. */
                cpu_count++;
            }
            break;
        }
        case MADT_IO_APIC: {
            const struct madt_io_apic *e = (const void *)entry;
            if (ioapic_count < MAX_IOAPICS) {
                struct ioapic *io = &ioapics[ioapic_count++];
                io->regs = map_phys(e->address, PAGE_SIZE, MAP_UNCACHED);
                io->gsi_base = e->gsi_base;
                io->gsi_count = ((ioapic_read(io, IOAPIC_VER) >> 16) & 0xff) + 1;
            }
            break;
        }
        case MADT_SOURCE_OVERRIDE: {
            const struct madt_source_override *e = (const void *)entry;
            if (override_count < MAX_OVERRIDES) {
                overrides[override_count++] = (struct source_override){
                    .irq = e->source, .gsi = e->gsi, .flags = e->flags};
            }
            break;
        }
        case MADT_LOCAL_APIC_OVERRIDE: {
            const struct madt_local_apic_override *e = (const void *)entry;
            *lapic_phys = e->address;
            break;
        }
        }
        p += entry->length;
    }
}

void apic_init(void) {
    const struct madt *madt = (const struct madt *)acpi_find_table("APIC");
    if (!madt) {
        panic("APIC: no MADT in the ACPI tables");
    }
    uint64_t lapic_phys;
    parse_madt(madt, &lapic_phys);
    if (ioapic_count == 0) {
        panic("APIC: no I/O APIC found");
    }

    pic_disable();

    wrmsr(IA32_APIC_BASE_MSR, rdmsr(IA32_APIC_BASE_MSR) | IA32_APIC_BASE_ENABLE);
    lapic = map_phys(lapic_phys, PAGE_SIZE, MAP_UNCACHED);
    lapic_write(LAPIC_TPR, 0);                        /* Accept all priorities. */
    lapic_write(LAPIC_SVR, 0x100 | VECTOR_SPURIOUS);  /* Software-enable the APIC. */

    for (int i = 0; i < ioapic_count; i++) {
        for (uint32_t n = 0; n < ioapics[i].gsi_count; n++) {
            ioapic_write(&ioapics[i], IOAPIC_REDTBL(n), IOAPIC_MASKED);
            ioapic_write(&ioapics[i], IOAPIC_REDTBL(n) + 1, 0);
        }
    }

    kprintf("[apic] %u CPU(s), local APIC id %u at %p, %d I/O APIC(s), %d IRQ override(s)\n",
            cpu_count, lapic_id(), (void *)lapic_phys, ioapic_count, override_count);
}

void ioapic_route_isa_irq(uint8_t irq, uint8_t vector) {
    /* ISA IRQs are edge-triggered and active-high unless the MADT overrides it. */
    uint32_t gsi = irq;
    uint32_t flags = 0;
    for (int i = 0; i < override_count; i++) {
        if (overrides[i].irq == irq) {
            gsi = overrides[i].gsi;
            if ((overrides[i].flags & 0x3) == 0x3) {
                flags |= IOAPIC_ACTIVE_LOW;
            }
            if (((overrides[i].flags >> 2) & 0x3) == 0x3) {
                flags |= IOAPIC_LEVEL;
            }
        }
    }

    for (int i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi >= io->gsi_base && gsi < io->gsi_base + io->gsi_count) {
            uint32_t n = gsi - io->gsi_base;
            ioapic_write(io, IOAPIC_REDTBL(n) + 1, lapic_id() << 24);
            ioapic_write(io, IOAPIC_REDTBL(n), vector | flags);
            return;
        }
    }
    kprintf("[apic] warning: no I/O APIC handles GSI %u (IRQ %u)\n", gsi, irq);
}
