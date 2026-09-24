#ifndef VEXA_MM_H
#define VEXA_MM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE ((uint64_t)4096)
#define PAGE_SHIFT 12

/* Largest block the page allocator hands out: 2^PMM_MAX_ORDER pages (4 MiB). */
#define PMM_MAX_ORDER 10

extern uint64_t hhdm_offset;

/* Physical memory is mapped at hhdm_offset + phys (the "direct map"). */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

static inline uint64_t virt_to_phys(const void *virt) {
    return (uint64_t)virt - hhdm_offset;
}

/* A physical memory range, copied out of the bootloader's memory map. */
struct mem_range {
    uint64_t base;
    uint64_t length;
    uint64_t type; /* LIMINE_MEMMAP_* */
};

/* ---- Physical page allocator (pmm.c): a buddy allocator. ---- */

/* `ranges` must stay valid; the allocator keeps pointing at it. */
void pmm_init(const struct mem_range *ranges, size_t count);
/* The memory map the allocator was initialized with. */
const struct mem_range *pmm_memory_map(size_t *count);
/* Gives the bootloader's memory to the allocator. Only safe once nothing uses
 * it any more: the kernel runs on its own page tables and stack. */
void pmm_reclaim_bootloader_memory(void);
/* Returns the physical address of 2^order contiguous pages, or 0 if out of memory. */
uint64_t pmm_alloc(unsigned order);
void pmm_free(uint64_t phys, unsigned order);
/* One zeroed page; panics when memory runs out. For page tables and such. */
uint64_t pmm_alloc_zeroed_page(void);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);

/* ---- Virtual memory (vmm.c): the kernel's page tables. ---- */

enum map_cache {
    MAP_WRITEBACK,     /* Normal memory. */
    MAP_WRITECOMBINE,  /* Framebuffers. */
    MAP_UNCACHED,      /* Device registers (MMIO). */
};

#define VMM_WRITE (1U << 0)
#define VMM_EXEC (1U << 1)

/* Builds Vexa's own page tables (direct map and kernel image) and switches to them. */
void vmm_init(const struct mem_range *ranges, size_t count, uint64_t kernel_phys,
              uint64_t kernel_virt);
/* Makes [phys, phys + size) reachable at phys_to_virt(phys). RAM is already
 * mapped there; device registers and some ACPI tables need this first. */
void *map_phys(uint64_t phys, size_t size, enum map_cache cache);
/* A process's page tables: its own lower half, the kernel's upper half. */
struct address_space {
    uint64_t *pml4;
    uint64_t pml4_phys;
};

struct address_space *vmm_create_address_space(void);
/* Frees every user page and page table. It must not be active on any CPU. */
void vmm_destroy_address_space(struct address_space *as);
/* Loads `as`, or the kernel-only tables for NULL. */
void vmm_activate(struct address_space *as);
/* Maps a zeroed page at user address `virt` (flags: VMM_WRITE, VMM_EXEC). If a
 * page is already there, adds the flags to it. Returns the page's physical
 * address, or 0 if out of memory or `virt` is not a user address. */
uint64_t vmm_map_user_page(struct address_space *as, uint64_t virt, unsigned flags);
/* True if [virt, virt + size) is mapped, user-accessible memory in the active
 * address space (and writable, if `write`). */
bool vmm_user_range_mapped(uint64_t virt, uint64_t size, bool write);

/* Allocates a kernel stack with an unmapped guard page below it. Returns its top. */
uint64_t vmm_alloc_kernel_stack(size_t size);
/* True if `virt` falls in a kernel stack guard page (i.e. a stack overflowed). */
bool vmm_is_stack_guard(uint64_t virt);

/* ---- Kernel heap (heap.c): slab caches for small objects. ---- */

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);
/* Returns cached empty slabs to the page allocator. */
void heap_trim(void);

struct heap_stats {
    uint64_t slab_pages;  /* Pages holding small objects. */
    uint64_t large_pages; /* Pages holding allocations over 1 KiB. */
    uint64_t allocations; /* Live allocations. */
};
void heap_get_stats(struct heap_stats *stats);

#endif
