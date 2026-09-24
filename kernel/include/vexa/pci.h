#ifndef VEXA_PCI_H
#define VEXA_PCI_H

#include <stdbool.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/sched.h>

struct pci_device {
    uint8_t bus, slot, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if;
    uint64_t bar[6];      /* Physical base address (memory BARs) or port (I/O BARs). */
    uint64_t bar_size[6];
    bool bar_is_io[6];
    volatile uint8_t *config; /* Memory-mapped config space, or NULL for port I/O. */
    struct pci_device *next;
};

/* Finds every PCI device (through ACPI's MCFG table, or legacy I/O ports). */
void pci_init(void);
struct pci_device *pci_first(void); /* Iterate with ->next. */

uint32_t pci_read32(struct pci_device *device, uint16_t offset);
void pci_write32(struct pci_device *device, uint16_t offset, uint32_t value);
uint16_t pci_read16(struct pci_device *device, uint16_t offset);
void pci_write16(struct pci_device *device, uint16_t offset, uint16_t value);
uint8_t pci_read8(struct pci_device *device, uint16_t offset);

/* Turns on memory decoding and bus mastering (DMA). */
void pci_enable(struct pci_device *device);
/* Maps a memory BAR (uncached) and returns its address. */
volatile void *pci_map_bar(struct pci_device *device, int bar);
/* Returns the offset of a capability, starting after `after` (0 = from the start), or 0. */
uint8_t pci_find_capability(struct pci_device *device, uint8_t id, uint8_t after);

/* Routes the device's first MSI-X vector (or its MSI) to `handler` on the
 * bootstrap CPU. Returns false if neither is available (or there is no APIC),
 * in which case the driver should poll. */
bool pci_enable_msi(struct pci_device *device, irq_handler_t handler);

const char *pci_class_name(struct pci_device *device);

/* Waiting for a device to finish something: sleeps until its interrupt when it
 * has one, otherwise keeps checking (yielding the CPU in between). */
struct device_waiter {
    bool has_interrupt;
    struct wait_queue queue;
};

void device_wait(struct device_waiter *waiter, bool (*done)(void *arg), void *arg);
void device_wake(struct device_waiter *waiter); /* From the interrupt handler. */

#endif
