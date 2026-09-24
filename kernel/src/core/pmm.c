#include <stdbool.h>
#include <limine.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

/*
 * Buddy allocator. Free memory is kept as blocks of 2^order pages, each aligned
 * to its own size, on one free list per order. Freeing a block merges it with
 * its "buddy" (the neighbouring block of the same size) whenever that is free
 * too, so large contiguous blocks come back over time.
 *
 * pmm_lock makes it safe from any CPU and from interrupt handlers.
 */

#define PAGE_NOT_FREE 0xff
/* Memory below 1 MiB stays reserved: starting other CPUs needs some later. */
#define LOW_MEMORY_LIMIT 0x100000
#define MAX_RECLAIMABLE 64

struct free_block {
    struct free_block *next;
    struct free_block *prev;
};

/* Circular lists with a sentinel head per order. */
static struct spinlock pmm_lock = SPINLOCK_INIT;
static struct free_block free_lists[PMM_MAX_ORDER + 1];
/* For each page: the order of the free block starting there, or PAGE_NOT_FREE. */
static uint8_t *page_state;
static uint64_t page_count; /* Pages covered by page_state. */
static uint64_t total_pages;
static uint64_t free_pages;

static const struct mem_range *memory_map;
static size_t memory_map_count;
static struct mem_range reclaimable[MAX_RECLAIMABLE];
static size_t reclaimable_count;

static struct free_block *block_at(uint64_t page) {
    return phys_to_virt(page << PAGE_SHIFT);
}

static void list_push(unsigned order, uint64_t page) {
    struct free_block *block = block_at(page), *head = &free_lists[order];
    block->next = head->next;
    block->prev = head;
    head->next->prev = block;
    head->next = block;
    page_state[page] = (uint8_t)order;
}

static void list_remove(uint64_t page) {
    struct free_block *block = block_at(page);
    block->prev->next = block->next;
    block->next->prev = block->prev;
    page_state[page] = PAGE_NOT_FREE;
}

static void free_block(uint64_t page, unsigned order) {
    free_pages += 1ULL << order;
    while (order < PMM_MAX_ORDER) {
        uint64_t buddy = page ^ (1ULL << order);
        if (buddy >= page_count || page_state[buddy] != order) {
            break;
        }
        list_remove(buddy);
        page &= ~(1ULL << order); /* The merged block starts at the lower of the two. */
        order++;
    }
    list_push(order, page);
}

/* Adds [base, end) to the allocator as the largest aligned blocks that fit. */
static void add_range(uint64_t base, uint64_t end) {
    if (base < LOW_MEMORY_LIMIT) {
        base = LOW_MEMORY_LIMIT;
    }
    uint64_t page = (base + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t last = end >> PAGE_SHIFT;
    if (last > page_count) {
        last = page_count;
    }
    while (page < last) {
        unsigned order = 0;
        while (order < PMM_MAX_ORDER && (page & (1ULL << order)) == 0 &&
               page + (2ULL << order) <= last) {
            order++;
        }
        total_pages += 1ULL << order;
        free_block(page, order);
        page += 1ULL << order;
    }
}

void pmm_init(const struct mem_range *ranges, size_t count) {
    memory_map = ranges;
    memory_map_count = count;
    for (unsigned order = 0; order <= PMM_MAX_ORDER; order++) {
        free_lists[order].next = free_lists[order].prev = &free_lists[order];
    }

    /* page_state must cover every page we might ever hand out. */
    uint64_t highest = 0;
    for (size_t i = 0; i < count; i++) {
        if (ranges[i].type == LIMINE_MEMMAP_USABLE ||
            ranges[i].type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            uint64_t end = ranges[i].base + ranges[i].length;
            highest = end > highest ? end : highest;
        }
    }
    page_count = highest >> PAGE_SHIFT;

    /* Carve the page_state array out of the first usable range big enough. */
    uint64_t state_size = (page_count + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t state_phys = 0;
    for (size_t i = 0; i < count && !state_phys; i++) {
        uint64_t base = ranges[i].base < LOW_MEMORY_LIMIT ? LOW_MEMORY_LIMIT : ranges[i].base;
        base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (ranges[i].type == LIMINE_MEMMAP_USABLE &&
            base + state_size <= ranges[i].base + ranges[i].length) {
            state_phys = base;
        }
    }
    if (!state_phys) {
        panic("pmm: no room for %lu KiB of page metadata", state_size / 1024);
    }
    page_state = phys_to_virt(state_phys);
    memset(page_state, PAGE_NOT_FREE, state_size);

    for (size_t i = 0; i < count; i++) {
        uint64_t base = ranges[i].base, end = base + ranges[i].length;
        if (ranges[i].type == LIMINE_MEMMAP_USABLE) {
            if (base <= state_phys && state_phys < end) {
                add_range(base, state_phys);
                add_range(state_phys + state_size, end);
            } else {
                add_range(base, end);
            }
        } else if (ranges[i].type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
                   reclaimable_count < MAX_RECLAIMABLE) {
            reclaimable[reclaimable_count++] = ranges[i];
        }
    }
    kprintf("[pmm] %lu MiB free for allocation (%lu KiB of metadata)\n",
            free_pages * PAGE_SIZE / (1024 * 1024), state_size / 1024);
}

void pmm_reclaim_bootloader_memory(void) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t before = free_pages;
    for (size_t i = 0; i < reclaimable_count; i++) {
        add_range(reclaimable[i].base, reclaimable[i].base + reclaimable[i].length);
    }
    reclaimable_count = 0;
    spin_unlock_irqrestore(&pmm_lock, flags);
    kprintf("[pmm] reclaimed %lu KiB from the bootloader\n",
            (free_pages - before) * PAGE_SIZE / 1024);
}

uint64_t pmm_alloc(unsigned order) {
    if (order > PMM_MAX_ORDER) {
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    unsigned found = order;
    while (found <= PMM_MAX_ORDER && free_lists[found].next == &free_lists[found]) {
        found++;
    }
    if (found > PMM_MAX_ORDER) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return 0;
    }
    uint64_t page = virt_to_phys(free_lists[found].next) >> PAGE_SHIFT;
    list_remove(page);
    /* Split the block, returning the upper halves to the free lists. */
    while (found > order) {
        found--;
        list_push(found, page + (1ULL << found));
    }
    free_pages -= 1ULL << order;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return page << PAGE_SHIFT;
}

void pmm_free(uint64_t phys, unsigned order) {
    uint64_t page = phys >> PAGE_SHIFT;
    if ((phys & (PAGE_SIZE - 1)) || order > PMM_MAX_ORDER || page + (1ULL << order) > page_count ||
        (page & ((1ULL << order) - 1)) || phys < LOW_MEMORY_LIMIT) {
        panic("pmm_free: bad block %p (order %u)", (void *)phys, order);
    }
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    if (page_state[page] != PAGE_NOT_FREE) {
        panic("pmm_free: double free of %p", (void *)phys);
    }
    free_block(page, order);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_alloc_zeroed_page(void) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        panic("out of physical memory");
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

const struct mem_range *pmm_memory_map(size_t *count) {
    *count = memory_map_count;
    return memory_map;
}

uint64_t pmm_total_pages(void) {
    return total_pages;
}

uint64_t pmm_free_pages(void) {
    return free_pages;
}
