#include <vexa/acpi.h>
#include <vexa/cpu.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/string.h>

/* PCI configuration space comes two ways: memory-mapped (ECAM, described by
 * ACPI's MCFG table, 4 KiB per function) or through I/O ports 0xcf8/0xcfc
 * (the first 256 bytes only). Devices are found by walking from bus 0 through
 * PCI-to-PCI bridges. */

#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_CLASS_REVISION 0x08
#define PCI_HEADER_TYPE 0x0e
#define PCI_BAR0 0x10
#define PCI_SECONDARY_BUS 0x19
#define PCI_CAPABILITIES 0x34

#define COMMAND_IO (1 << 0)
#define COMMAND_MEMORY (1 << 1)
#define COMMAND_BUS_MASTER (1 << 2)
#define COMMAND_INTX_DISABLE (1 << 10)
#define STATUS_CAPABILITIES (1 << 4)

#define CAP_MSI 0x05
#define CAP_MSIX 0x11

struct __attribute__((packed)) mcfg_entry {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
};

static uint64_t ecam_base;
static uint8_t ecam_start_bus, ecam_end_bus;
static struct pci_device *devices;
static int device_count;

static uint32_t legacy_address(struct pci_device *d, uint16_t offset) {
    return 0x80000000U | (uint32_t)d->bus << 16 | (uint32_t)d->slot << 11 |
           (uint32_t)d->function << 8 | (offset & 0xfc);
}

uint32_t pci_read32(struct pci_device *d, uint16_t offset) {
    if (d->config) {
        return *(volatile uint32_t *)(d->config + offset);
    }
    __asm__ volatile("outl %0, %1" : : "a"(legacy_address(d, offset)), "Nd"((uint16_t)0xcf8));
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"((uint16_t)0xcfc));
    return value;
}

void pci_write32(struct pci_device *d, uint16_t offset, uint32_t value) {
    if (d->config) {
        *(volatile uint32_t *)(d->config + offset) = value;
        return;
    }
    __asm__ volatile("outl %0, %1" : : "a"(legacy_address(d, offset)), "Nd"((uint16_t)0xcf8));
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"((uint16_t)0xcfc));
}

uint16_t pci_read16(struct pci_device *d, uint16_t offset) {
    return (uint16_t)(pci_read32(d, offset & ~3) >> ((offset & 2) * 8));
}

void pci_write16(struct pci_device *d, uint16_t offset, uint16_t value) {
    uint32_t word = pci_read32(d, offset & ~3);
    int shift = (offset & 2) * 8;
    word = (word & ~(0xffffU << shift)) | ((uint32_t)value << shift);
    pci_write32(d, offset & ~3, word);
}

uint8_t pci_read8(struct pci_device *d, uint16_t offset) {
    return (uint8_t)(pci_read32(d, offset & ~3) >> ((offset & 3) * 8));
}

void pci_enable(struct pci_device *d) {
    pci_write16(d, PCI_COMMAND, pci_read16(d, PCI_COMMAND) | COMMAND_MEMORY | COMMAND_BUS_MASTER);
}

volatile void *pci_map_bar(struct pci_device *d, int bar) {
    if (bar < 0 || bar > 5 || d->bar_is_io[bar] || !d->bar[bar]) {
        return NULL;
    }
    return map_phys(d->bar[bar], d->bar_size[bar], MAP_UNCACHED);
}

uint8_t pci_find_capability(struct pci_device *d, uint8_t id, uint8_t after) {
    if (!(pci_read16(d, PCI_STATUS) & STATUS_CAPABILITIES)) {
        return 0;
    }
    uint8_t offset = after ? pci_read8(d, after + 1) : pci_read8(d, PCI_CAPABILITIES);
    for (int guard = 0; offset && guard < 48; guard++) {
        offset &= 0xfc;
        if (pci_read8(d, offset) == id) {
            return offset;
        }
        offset = pci_read8(d, offset + 1);
    }
    return 0;
}

/* Sizes the BARs: write all ones, read back which bits stuck. */
static void read_bars(struct pci_device *d, int count) {
    uint16_t command = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, command & ~(COMMAND_IO | COMMAND_MEMORY));
    for (int i = 0; i < count; i++) {
        uint16_t reg = PCI_BAR0 + i * 4;
        uint32_t original = pci_read32(d, reg);
        pci_write32(d, reg, 0xffffffff);
        uint32_t mask = pci_read32(d, reg);
        pci_write32(d, reg, original);
        if (original & 1) {
            d->bar_is_io[i] = true;
            d->bar[i] = original & ~3U;
            d->bar_size[i] = (~(mask & ~3U) + 1) & 0xffff;
            continue;
        }
        bool is_64 = ((original >> 1) & 3) == 2;
        uint64_t base = original & ~0xfULL;
        uint64_t size_mask = 0xffffffff00000000ULL | (mask & ~0xfU);
        if (is_64 && i + 1 < count) {
            uint32_t high = pci_read32(d, reg + 4);
            pci_write32(d, reg + 4, 0xffffffff);
            uint32_t high_mask = pci_read32(d, reg + 4);
            pci_write32(d, reg + 4, high);
            base |= (uint64_t)high << 32;
            size_mask = (uint64_t)high_mask << 32 | (mask & ~0xfU);
        }
        d->bar[i] = base;
        d->bar_size[i] = mask ? ~size_mask + 1 : 0;
        if (is_64) {
            i++; /* The next BAR holds the upper half. */
        }
    }
    pci_write16(d, PCI_COMMAND, command);
}

static void scan_bus(uint8_t bus, int depth);

static void probe_function(uint8_t bus, uint8_t slot, uint8_t function, int depth) {
    struct pci_device probe = {.bus = bus, .slot = slot, .function = function};
    if (ecam_base) {
        if (bus < ecam_start_bus || bus > ecam_end_bus) {
            return;
        }
        uint64_t address = ecam_base + ((uint64_t)(bus - ecam_start_bus) << 20 |
                                        (uint64_t)slot << 15 | (uint64_t)function << 12);
        probe.config = map_phys(address, PAGE_SIZE, MAP_UNCACHED);
    }
    if (pci_read16(&probe, PCI_VENDOR_ID) == 0xffff) {
        return;
    }
    struct pci_device *d = kzalloc(sizeof(*d));
    if (!d) {
        return;
    }
    *d = probe;
    uint32_t id = pci_read32(d, PCI_VENDOR_ID);
    d->vendor_id = id & 0xffff;
    d->device_id = id >> 16;
    uint32_t class = pci_read32(d, PCI_CLASS_REVISION);
    d->class_code = class >> 24;
    d->subclass = (class >> 16) & 0xff;
    d->prog_if = (class >> 8) & 0xff;
    uint8_t header = pci_read8(d, PCI_HEADER_TYPE) & 0x7f;
    read_bars(d, header == 0 ? 6 : header == 1 ? 2 : 0);

    struct pci_device **link = &devices;
    while (*link) {
        link = &(*link)->next;
    }
    *link = d;
    device_count++;

    /* A PCI-to-PCI bridge: the devices behind it are on its secondary bus. */
    if (d->class_code == 0x06 && d->subclass == 0x04 && header == 1 && depth < 16) {
        uint8_t secondary = pci_read8(d, PCI_SECONDARY_BUS);
        if (secondary > bus) {
            scan_bus(secondary, depth + 1);
        }
    }
}

static void scan_bus(uint8_t bus, int depth) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        struct pci_device probe = {.bus = bus, .slot = slot};
        if (ecam_base) {
            if (bus < ecam_start_bus || bus > ecam_end_bus) {
                return;
            }
            probe.config = map_phys(ecam_base + ((uint64_t)(bus - ecam_start_bus) << 20 |
                                                 (uint64_t)slot << 15),
                                    PAGE_SIZE, MAP_UNCACHED);
        }
        if (pci_read16(&probe, PCI_VENDOR_ID) == 0xffff) {
            continue;
        }
        bool multifunction = pci_read8(&probe, PCI_HEADER_TYPE) & 0x80;
        for (uint8_t function = 0; function < (multifunction ? 8 : 1); function++) {
            probe_function(bus, slot, function, depth);
        }
    }
}

void pci_init(void) {
    const struct acpi_sdt_header *mcfg = acpi_find_table("MCFG");
    if (mcfg && mcfg->length >= sizeof(*mcfg) + 8 + sizeof(struct mcfg_entry)) {
        const struct mcfg_entry *entry = (const void *)((const uint8_t *)mcfg + sizeof(*mcfg) + 8);
        if (entry->segment == 0) {
            ecam_base = entry->base;
            ecam_start_bus = entry->start_bus;
            ecam_end_bus = entry->end_bus;
        }
    }
    scan_bus(0, 0);
    kprintf("[pci] %d devices (%s)\n", device_count,
            ecam_base ? "memory-mapped config" : "legacy config ports");
}

struct pci_device *pci_first(void) {
    return devices;
}

/* ---- MSI and MSI-X ---- */

static uint64_t msi_address(void) {
    return 0xfee00000ULL | ((uint64_t)cpus[0].lapic_id << 12);
}

bool pci_enable_msi(struct pci_device *d, irq_handler_t handler) {
    if (!interrupt_controller_is_apic()) {
        return false;
    }
    uint8_t msix = pci_find_capability(d, CAP_MSIX, 0);
    uint8_t msi = pci_find_capability(d, CAP_MSI, 0);
    if (!msix && !msi) {
        return false;
    }
    int vector = irq_alloc_vector();
    if (vector < 0) {
        return false;
    }
    irq_register((uint8_t)vector, handler);

    if (msix) {
        uint32_t table = pci_read32(d, msix + 4);
        int bir = table & 7;
        volatile uint8_t *bar = pci_map_bar(d, bir);
        if (bar) {
            volatile uint32_t *entry = (volatile uint32_t *)(bar + (table & ~7U));
            entry[0] = (uint32_t)msi_address();
            entry[1] = (uint32_t)(msi_address() >> 32);
            entry[2] = (uint32_t)vector;
            entry[3] = 0; /* Unmasked. */
            uint16_t control = pci_read16(d, msix + 2);
            control = (control | (1 << 15)) & ~(1 << 14); /* Enable; clear function mask. */
            pci_write16(d, msix + 2, control);
            pci_write16(d, PCI_COMMAND, pci_read16(d, PCI_COMMAND) | COMMAND_INTX_DISABLE);
            return true;
        }
    }
    if (msi) {
        uint16_t control = pci_read16(d, msi + 2);
        bool is_64 = control & (1 << 7);
        pci_write32(d, msi + 4, (uint32_t)msi_address());
        if (is_64) {
            pci_write32(d, msi + 8, (uint32_t)(msi_address() >> 32));
            pci_write16(d, msi + 12, (uint16_t)vector);
        } else {
            pci_write16(d, msi + 8, (uint16_t)vector);
        }
        control = (control & ~(7 << 4)) | 1; /* One message; enable. */
        pci_write16(d, msi + 2, control);
        pci_write16(d, PCI_COMMAND, pci_read16(d, PCI_COMMAND) | COMMAND_INTX_DISABLE);
        return true;
    }
    return false;
}

const char *pci_class_name(struct pci_device *d) {
    switch (d->class_code) {
    case 0x01:
        switch (d->subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller (AHCI)";
        case 0x08: return "NVMe controller";
        default: return "storage controller";
        }
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x06:
        switch (d->subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default: return "bridge";
        }
    case 0x0c:
        return d->subclass == 0x03 ? "USB controller" : d->subclass == 0x05 ? "SMBus controller"
                                                                              : "serial bus controller";
    default: return "device";
    }
}

/* ---- Waiting for devices ---- */

void device_wait(struct device_waiter *waiter, bool (*done)(void *arg), void *arg) {
    if (waiter->has_interrupt) {
        wait_queue_wait(&waiter->queue, done, arg);
        return;
    }
    while (!done(arg)) {
        thread_yield();
    }
}

void device_wake(struct device_waiter *waiter) {
    wait_queue_wake_all(&waiter->queue);
}
