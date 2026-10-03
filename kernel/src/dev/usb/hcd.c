/* What the EHCI, UHCI and OHCI drivers share: memory below 4 GiB, device
 * addresses, and waiting by checking. */
#include <vexa/arch.h>
#include <vexa/mm.h>
#include <vexa/sched.h>
#include <vexa/string.h>

#include "hcd.h"

void *hcd_pages(unsigned order) {
    uint64_t phys = pmm_alloc_below(order, 0x100000000ULL);
    if (!phys) {
        return NULL;
    }
    void *p = phys_to_virt(phys);
    memset(p, 0, PAGE_SIZE << order);
    return p;
}

void hcd_free_pages(void *pages, unsigned order) {
    if (pages) {
        pmm_free(virt_to_phys(pages), order);
    }
}

uint32_t hcd_phys(const void *p) {
    return (uint32_t)virt_to_phys(p);
}

int hcd_new_address(struct hcd_addresses *a) {
    for (int address = 1; address < 128; address++) {
        if (!(a->used[address / 8] & (1U << (address % 8)))) {
            a->used[address / 8] |= (uint8_t)(1U << (address % 8));
            return address;
        }
    }
    return 0;
}

void hcd_free_address(struct hcd_addresses *a, int address) {
    if (address > 0 && address < 128) {
        a->used[address / 8] &= (uint8_t)~(1U << (address % 8));
    }
}

bool hcd_wait(bool (*done)(void *arg), void *arg, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms() + timeout_ms;
    for (int spins = 0;; spins++) {
        if (done(arg)) {
            return true;
        }
        if (timer_ms() > deadline) {
            return done(arg);
        }
        if (spins < 4) {
            thread_yield(); /* (Short transfers are often done by now.) */
        } else {
            thread_sleep_ms(1);
        }
    }
}
