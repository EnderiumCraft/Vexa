#include <stdbool.h>
#include <stddef.h>
#include <vexa/acpi.h>
#include <vexa/arch.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/power.h>
#include <vexa/string.h>

/*
 * Restarting: the PS/2 controller's reset line, then the ACPI reset
 * register's classic cousin (port 0xCF9), then a triple fault.
 *
 * Turning off: ACPI's "soft off" state, S5. The FADT says where the PM1
 * control registers are; the sleep type for S5 is in the DSDT's \_S5_
 * package, which is found by looking for its name rather than by running
 * AML (every firmware writes it the same few ways). Without ACPI, the
 * ports virtual machines use are tried (QEMU, Bochs, VirtualBox), and
 * then the CPU just halts.
 */

/* The FADT's fields we need (offsets from the start of the table). */
#define FADT_DSDT 40      /* 32-bit address */
#define FADT_SMI_CMD 48   /* 32-bit port */
#define FADT_ACPI_ENABLE 52
#define FADT_PM1A_CNT 64  /* 32-bit port */
#define FADT_PM1B_CNT 68
#define FADT_X_DSDT 140   /* 64-bit address (ACPI 2.0+) */

#define SLP_EN (1 << 13)
#define SCI_EN 1

static uint32_t read32(const uint8_t *table, size_t offset) {
    uint32_t value;
    memcpy(&value, table + offset, 4);
    return value;
}

/* One of the package's numbers: a Zero, One, or a byte, word or dword. */
static const uint8_t *aml_number(const uint8_t *p, uint32_t *value) {
    switch (*p) {
    case 0x00: *value = 0; return p + 1;             /* ZeroOp */
    case 0x01: *value = 1; return p + 1;             /* OneOp */
    case 0x0a: *value = p[1]; return p + 2;          /* BytePrefix */
    case 0x0b: *value = p[1] | p[2] << 8; return p + 3; /* WordPrefix */
    case 0x0c: memcpy(value, p + 1, 4); return p + 5; /* DWordPrefix */
    }
    *value = 0;
    return p + 1;
}

/* The S5 sleep types from the DSDT: false if there's no \_S5_. */
static bool find_s5(const struct acpi_sdt_header *dsdt, uint32_t *type_a, uint32_t *type_b) {
    const uint8_t *start = (const uint8_t *)dsdt + sizeof(*dsdt);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;
    for (const uint8_t *p = start; p + 8 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0) {
            continue;
        }
        /* NameOp (0x08) before it (maybe with a root "\"), a package after. */
        if (!(p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\')) || p[4] != 0x12) {
            continue;
        }
        const uint8_t *q = p + 5;
        q += ((*q & 0xc0) >> 6) + 1; /* The package's length. */
        q++;                         /* How many elements. */
        q = aml_number(q, type_a);
        aml_number(q, type_b);
        return true;
    }
    return false;
}

static void acpi_off(void) {
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP");
    if (!fadt) {
        kprintf("[power] no ACPI FADT\n");
        return;
    }
    const struct acpi_sdt_header *header = (const struct acpi_sdt_header *)fadt;
    uint64_t dsdt_phys = read32(fadt, FADT_DSDT);
    if (header->revision >= 2 && header->length >= FADT_X_DSDT + 8) {
        uint64_t x;
        memcpy(&x, fadt + FADT_X_DSDT, 8);
        dsdt_phys = x ? x : dsdt_phys;
    }
    if (!dsdt_phys) {
        return;
    }
    const struct acpi_sdt_header *dsdt = map_phys(dsdt_phys, sizeof(*dsdt), MAP_WRITEBACK);
    dsdt = map_phys(dsdt_phys, dsdt->length, MAP_WRITEBACK);
    uint32_t type_a, type_b;
    if (!find_s5(dsdt, &type_a, &type_b)) {
        kprintf("[power] no \\_S5_ in the DSDT\n");
        return;
    }
    uint16_t pm1a = (uint16_t)read32(fadt, FADT_PM1A_CNT);
    uint16_t pm1b = (uint16_t)read32(fadt, FADT_PM1B_CNT);
    uint32_t smi_cmd = read32(fadt, FADT_SMI_CMD);
    uint8_t acpi_enable = fadt[FADT_ACPI_ENABLE];
    if (!pm1a) {
        return;
    }
    /* ACPI mode first, if the firmware hasn't switched to it. */
    if (!(inw(pm1a) & SCI_EN) && smi_cmd && acpi_enable) {
        outb((uint16_t)smi_cmd, acpi_enable);
        for (int i = 0; i < 1000000 && !(inw(pm1a) & SCI_EN); i++) {
            __asm__ volatile("pause");
        }
    }
    kprintf("[power] ACPI S5: PM1a control 0x%x, sleep type %u\n", pm1a, type_a);
    outw(pm1a, (uint16_t)(type_a << 10 | SLP_EN));
    if (pm1b) {
        outw(pm1b, (uint16_t)(type_b << 10 | SLP_EN));
    }
}

void power_off(void) {
    kprintf("Turning off...\n");
    interrupts_disable();
    acpi_off();
    /* Virtual machines' own ways. */
    outw(0x604, 0x2000);  /* QEMU (q35 and newer) */
    outw(0xb004, 0x2000); /* QEMU (older), Bochs */
    outw(0x4004, 0x3400); /* VirtualBox */
    kprintf("It's now safe to turn off the machine.\n");
    cpu_halt_forever();
}

void power_restart(void) {
    kprintf("Restarting...\n");
    interrupts_disable();
    outb(0x64, 0xfe);     /* The reset line, through the PS/2 controller. */
    outb(0xcf9, 0x06);    /* The chipset's reset control. */
    /* An empty IDT and a fault: the CPU resets. */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } empty_idt = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty_idt));
    cpu_halt_forever();
}
