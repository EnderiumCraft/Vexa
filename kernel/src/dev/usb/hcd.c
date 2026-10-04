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

void hcd_irq_wake(struct hcd_irq *irq) {
    __atomic_fetch_add(&irq->count, 1, __ATOMIC_RELEASE);
    wait_queue_wake_all(&irq->queue);
}

#define RECHECK_MS 10 /* With interrupts: in case one goes missing. */

bool hcd_wait(struct hcd_irq *irq, bool (*done)(void *arg), void *arg, uint32_t timeout_ms) {
    uint64_t deadline = timer_ms() + timeout_ms;
    for (int spins = 0;; spins++) {
        if (done(arg)) {
            return true;
        }
        uint64_t now = timer_ms();
        if (now > deadline) {
            return done(arg);
        }
        if (irq && irq->enabled) {
            uint64_t wait = deadline - now < RECHECK_MS ? deadline - now + 1 : RECHECK_MS;
            wait_queue_wait_timeout(&irq->queue, done, arg, wait, false);
        } else if (spins < 4) {
            thread_yield(); /* (Short transfers are often done by now.) */
        } else {
            thread_sleep_ms(1);
        }
    }
}

static bool interrupted(void *arg) {
    struct hcd_irq *irq = arg;
    return __atomic_load_n(&irq->count, __ATOMIC_ACQUIRE) != irq->seen;
}

void hcd_idle(struct hcd_irq *irq, uint32_t poll_ms, uint32_t idle_ms) {
    if (!irq || !irq->enabled) {
        thread_sleep_ms(poll_ms);
        return;
    }
    /* (Interrupts since the last round count too.) */
    wait_queue_wait_timeout(&irq->queue, interrupted, irq, idle_ms, false);
    irq->seen = __atomic_load_n(&irq->count, __ATOMIC_ACQUIRE);
}
