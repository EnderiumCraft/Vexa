#include <stddef.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/memtest.h>
#include <vexa/mm.h>

#define HEAP_SLOTS 4096
#define PAGE_SLOTS 256
#define MAX_PAGE_ORDER 5

struct heap_slot {
    uint8_t *ptr;
    size_t size;
    uint8_t stamp;
};

struct page_slot {
    uint64_t phys;
    unsigned order;
};

static uint64_t rng_state;

static uint64_t rng(void) {
    /* xorshift64 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Mostly small objects, like a real kernel, with some that need whole pages. */
static size_t random_size(void) {
    uint64_t r = rng() % 100;
    if (r < 70) {
        return 1 + rng() % 256;
    }
    if (r < 95) {
        return 257 + rng() % (4096 - 256);
    }
    return 4097 + rng() % (64 * 1024);
}

/* How many bytes to mark at each end: up to 8, never letting the two ends overlap. */
static size_t stamp_length(const struct heap_slot *slot) {
    size_t n = (slot->size + 1) / 2;
    return n < 8 ? n : 8;
}

/* Marks the first and last bytes, which is where overlapping or misplaced
 * allocations would collide. */
static void stamp(struct heap_slot *slot) {
    for (size_t i = 0; i < stamp_length(slot); i++) {
        slot->ptr[i] = (uint8_t)(slot->stamp + i);
        slot->ptr[slot->size - 1 - i] = (uint8_t)(slot->stamp + i);
    }
}

static bool stamp_intact(const struct heap_slot *slot) {
    for (size_t i = 0; i < stamp_length(slot); i++) {
        if (slot->ptr[i] != (uint8_t)(slot->stamp + i) ||
            slot->ptr[slot->size - 1 - i] != (uint8_t)(slot->stamp + i)) {
            return false;
        }
    }
    return true;
}

static bool release_heap_slot(struct heap_slot *slot) {
    bool ok = stamp_intact(slot);
    if (!ok) {
        kprintf("memtest: object at %p (%lu bytes) was overwritten\n", (void *)slot->ptr,
                slot->size);
    }
    kfree(slot->ptr);
    slot->ptr = NULL;
    return ok;
}

static bool release_page_slot(struct page_slot *slot) {
    bool ok = *(uint64_t *)phys_to_virt(slot->phys) == slot->phys;
    if (!ok) {
        kprintf("memtest: page block at %p was overwritten\n", (void *)slot->phys);
    }
    pmm_free(slot->phys, slot->order);
    slot->phys = 0;
    return ok;
}

bool memtest_run(uint64_t operations) {
    struct heap_stats stats;
    heap_trim();
    heap_get_stats(&stats);
    uint64_t free_before = pmm_free_pages();
    uint64_t allocations_before = stats.allocations;
    uint64_t start_ms = timer_ms();
    rng_state = 0x9e3779b97f4a7c15ULL ^ start_ms;

    struct heap_slot *heap_slots = kzalloc(HEAP_SLOTS * sizeof(*heap_slots));
    struct page_slot *page_slots = kzalloc(PAGE_SLOTS * sizeof(*page_slots));
    if (!heap_slots || !page_slots) {
        kprintf("memtest: could not allocate bookkeeping\n");
        kfree(heap_slots);
        kfree(page_slots);
        return false;
    }

    bool ok = true;
    uint64_t heap_allocs = 0, page_allocs = 0, bytes = 0;
    for (uint64_t op = 0; op < operations && ok; op++) {
        if (rng() % 16 == 0) {
            struct page_slot *slot = &page_slots[rng() % PAGE_SLOTS];
            if (slot->phys) {
                ok = release_page_slot(slot);
            } else {
                slot->order = rng() % (MAX_PAGE_ORDER + 1);
                slot->phys = pmm_alloc(slot->order);
                if (slot->phys) {
                    *(uint64_t *)phys_to_virt(slot->phys) = slot->phys;
                    page_allocs++;
                }
            }
            continue;
        }
        struct heap_slot *slot = &heap_slots[rng() % HEAP_SLOTS];
        if (slot->ptr) {
            ok = release_heap_slot(slot);
        } else {
            slot->size = random_size();
            slot->stamp = (uint8_t)rng();
            slot->ptr = kmalloc(slot->size);
            if (!slot->ptr) {
                kprintf("memtest: kmalloc(%lu) failed\n", slot->size);
                ok = false;
                break;
            }
            stamp(slot);
            heap_allocs++;
            bytes += slot->size;
        }
    }

    for (int i = 0; i < HEAP_SLOTS; i++) {
        if (heap_slots[i].ptr && !release_heap_slot(&heap_slots[i])) {
            ok = false;
        }
    }
    for (int i = 0; i < PAGE_SLOTS; i++) {
        if (page_slots[i].phys && !release_page_slot(&page_slots[i])) {
            ok = false;
        }
    }
    kfree(heap_slots);
    kfree(page_slots);
    heap_trim();

    heap_get_stats(&stats);
    if (stats.allocations != allocations_before) {
        kprintf("memtest: %ld heap allocations leaked\n",
                (int64_t)(stats.allocations - allocations_before));
        ok = false;
    }
    if (pmm_free_pages() != free_before) {
        kprintf("memtest: %ld pages leaked\n", (int64_t)(free_before - pmm_free_pages()));
        ok = false;
    }
    kprintf("memtest: %s: %lu heap allocations (%lu MiB), %lu page blocks, %lu ms\n",
            ok ? "passed" : "FAILED", heap_allocs, bytes / (1024 * 1024), page_allocs,
            timer_ms() - start_ms);
    return ok;
}
