#ifndef VEXA_MM_H
#define VEXA_MM_H

#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#define PAGE_SIZE 4096ULL

enum map_cache {
    MAP_WRITEBACK, /* Normal memory, e.g. ACPI tables. */
    MAP_UNCACHED,  /* Device registers (MMIO). */
};

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

/* Early memory support, used until the real allocators arrive in Phase 2. */
void mm_early_init(struct limine_memmap_response *memmap, uint64_t hhdm);

/* Hands out zeroed physical pages from usable memory. Never frees. */
uint64_t early_alloc_page(void);

/* Makes [phys, phys + size) reachable at phys_to_virt(phys). The bootloader only
 * maps RAM there, so device registers and some ACPI tables need this first. */
void *map_phys(uint64_t phys, size_t size, enum map_cache cache);

#endif
