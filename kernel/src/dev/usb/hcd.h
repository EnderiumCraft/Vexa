#ifndef VEXA_USB_HCD_H
#define VEXA_USB_HCD_H

/*
 * What the older host controller drivers (EHCI, UHCI, OHCI) share. Those
 * controllers take 32-bit addresses, so everything they read or write is in
 * memory below 4 GiB, and they have no MSI: their drivers poll.
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

/* Waits (checking each millisecond) until done(arg); false on a timeout. */
bool hcd_wait(bool (*done)(void *arg), void *arg, uint32_t timeout_ms);

/* The setup packet's direction and length. */
static inline bool hcd_setup_in(const uint8_t setup[8]) {
    return setup[0] & USB_DIR_IN;
}

void ehci_init(void);
void uhci_init(void);
void ohci_init(void);

#endif
