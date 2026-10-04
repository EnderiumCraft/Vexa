#ifndef VEXA_USB_HCD_H
#define VEXA_USB_HCD_H

/*
 * What the older host controller drivers (EHCI, UHCI, OHCI) share. Those
 * controllers take 32-bit addresses, so everything they read or write is in
 * memory below 4 GiB, and they have no MSI: their interrupt is a legacy PCI
 * one (INTx), routed by the ACPI tables. Without it, their drivers poll.
 */
#include <stdbool.h>
#include <stdint.h>
#include <vexa/usb.h>

/* Zeroed pages below 4 GiB (2^order of them), or NULL. */
void *hcd_pages(unsigned order);
void hcd_free_pages(void *pages, unsigned order);
uint32_t hcd_phys(const void *p); /* (Of memory from hcd_pages.) */

/* Device addresses (1 to 127) on one controller. */
struct hcd_addresses {
    uint8_t used[16];
};
int hcd_new_address(struct hcd_addresses *a); /* 0 if all are taken. */
void hcd_free_address(struct hcd_addresses *a, int address);

/* A controller's interrupt, if it has one. */
struct hcd_irq {
    bool enabled;             /* Attached: waits sleep until interrupts. */
    struct wait_queue queue;  /* Woken by each. */
    volatile uint32_t count;  /* How many there have been. */
    uint32_t seen;            /* The count at the controller thread's last round. */
};

/* From the interrupt handler, once the controller's status is cleared. */
void hcd_irq_wake(struct hcd_irq *irq);
/* Waits until done(arg); false on a timeout. With interrupts it sleeps until
 * each one (rechecking every few milliseconds in case one is missed);
 * without, it checks each millisecond. */
bool hcd_wait(struct hcd_irq *irq, bool (*done)(void *arg), void *arg, uint32_t timeout_ms);
/* The controller's thread between rounds: `poll_ms` without interrupts;
 * with them, until the next one (or `idle_ms`). */
void hcd_idle(struct hcd_irq *irq, uint32_t poll_ms, uint32_t idle_ms);

/* The setup packet's direction and length. */
static inline bool hcd_setup_in(const uint8_t setup[8]) {
    return setup[0] & USB_DIR_IN;
}

void ehci_init(void);
void uhci_init(void);
void ohci_init(void);

#endif
