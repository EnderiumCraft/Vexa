#ifndef VEXA_MM_H
#define VEXA_MM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/spinlock.h>

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
/* User pages are reference counted, so copy-on-write can share them between
 * address spaces. page_ref_new() returns a zeroed page with one reference;
 * the page is freed when the last reference is put. */
uint64_t page_ref_new(void);
void page_ref_get(uint64_t phys);
void page_ref_put(uint64_t phys);
uint32_t page_ref_count(uint64_t phys);
/* For device memory mapped into programs (a framebuffer): an extra reference
 * that is never put, in case the page lies within RAM's range. */
void page_ref_pin(uint64_t phys);
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
/* Loads a process's page tables, or the kernel-only tables for NULL. */
struct address_space;
void vmm_activate(struct address_space *as);

/* Allocates a kernel stack with an unmapped guard page below it. Returns its top. */
uint64_t vmm_alloc_kernel_stack(size_t size);
/* True if `virt` falls in a kernel stack guard page (i.e. a stack overflowed). */
bool vmm_is_stack_guard(uint64_t virt);

/* ---- User address spaces (vm.c) ---- */

/*
 * A process's memory is a list of areas (the program's segments, its stack,
 * its heap, memory from vx_map/mmap). Pages inside an area are only allocated
 * when first touched, and after a fork both processes share pages until one
 * writes ("copy on write").
 */

#define VM_WRITE 0x1
#define VM_EXEC 0x2
#define VM_SHARED 0x4 /* Pages stay shared (not copy-on-write) across fork. */

struct vm_area {
    uint64_t start, end; /* Page aligned, end exclusive. */
    unsigned flags;      /* VM_* (every area is readable). */
    struct vm_area *next;
};

struct address_space {
    uint64_t *pml4;
    uint64_t pml4_phys;
    struct spinlock lock;
    uint32_t refs;         /* The process, plus anyone looking at it (/proc). */
    uint64_t active_cpus;  /* Bit n: CPU n has these page tables loaded. */
    struct vm_area *areas; /* Sorted by address. */
    uint64_t heap_start, heap_end; /* The brk heap. */
};

/* Makes every other CPU using `as` drop its cached translations, and waits
 * until they have (arch/x86_64/tlb.c). Call after changing or removing page
 * table entries, before freeing the pages they pointed to. */
void tlb_shootdown(struct address_space *as);

/* A new, empty address space with one reference. */
struct address_space *vm_create(void);
void vm_get(struct address_space *as);
/* Drops a reference; the last one frees every page and table, so by then it
 * must not be active on any CPU. */
void vm_put(struct address_space *as);
/* A copy for fork(): pages are shared read-only until either side writes. */
struct address_space *vm_fork(struct address_space *parent);
/* Adds [start, end) (page aligned) with `flags`. Where it overlaps existing
 * areas, those parts get the union of both flags (ELF segments can share a page). */
int vm_add_area(struct address_space *as, uint64_t start, uint64_t end, unsigned flags);
/* Finds room for `size` bytes and adds an area there. Returns 0 if full. */
uint64_t vm_map(struct address_space *as, uint64_t size, unsigned flags);
/* Fills a shared area (from vm_map/vm_map_fixed with VM_SHARED) with pages:
 * pages[i] (whose reference the mapping takes over), or fresh zeroed pages
 * where pages is NULL or an entry is 0. */
int vm_populate_shared(struct address_space *as, uint64_t start, size_t count,
                       const uint64_t *pages);
/* Maps exactly at `start`, replacing whatever was there. */
int vm_map_fixed(struct address_space *as, uint64_t start, uint64_t size, unsigned flags);
int vm_unmap(struct address_space *as, uint64_t start, uint64_t size);
int vm_protect(struct address_space *as, uint64_t start, uint64_t size, unsigned flags);
/* Sets the end of the brk heap; returns the (possibly unchanged) end. */
uint64_t vm_set_heap_end(struct address_space *as, uint64_t end);
/* Makes the page at `address` present (and writable, if `write`), allocating
 * or copying it as needed. Returns its physical address, or 0 if the address
 * isn't in an area that allows it. Works on inactive address spaces too. */
uint64_t vm_page_for(struct address_space *as, uint64_t address, bool write);
/* Page fault handler for user addresses. Returns false for a real fault. */
bool vm_handle_fault(struct address_space *as, uint64_t address, bool write);
/* Copies into another (inactive) address space, e.g. to set up a new stack. */
bool vm_write(struct address_space *as, uint64_t address, const void *data, size_t size);
uint64_t vm_resident_bytes(struct address_space *as);
/* If `address` is in a shared area (VM_SHARED), stores the physical address
 * behind it (faulting the page in) and returns true. */
bool vm_shared_physical(struct address_space *as, uint64_t address, uint64_t *phys);
/* Calls `fn` for each area, with the address space locked (no sleeping). */
void vm_for_each_area(struct address_space *as,
                      void (*fn)(const struct vm_area *area, void *arg), void *arg);

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
