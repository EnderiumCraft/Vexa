#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE (1ULL << 1)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_HUGE (1ULL << 7)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL

uint64_t hhdm_offset;

static struct limine_memmap_response *early_memmap;
static uint64_t early_entry;
static uint64_t early_next;

void mm_early_init(struct limine_memmap_response *memmap, uint64_t hhdm) {
    early_memmap = memmap;
    hhdm_offset = hhdm;
    early_entry = 0;
    early_next = 0;
}

uint64_t early_alloc_page(void) {
    for (; early_entry < early_memmap->entry_count; early_entry++) {
        struct limine_memmap_entry *e = early_memmap->entries[early_entry];
        /* Stay above 1 MiB: low memory is useful later for starting other CPUs. */
        if (e->type != LIMINE_MEMMAP_USABLE || e->base + e->length <= 0x100000) {
            continue;
        }
        if (early_next < e->base || early_next < 0x100000) {
            early_next = e->base > 0x100000 ? e->base : 0x100000;
        }
        early_next = (early_next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (early_next + PAGE_SIZE <= e->base + e->length) {
            uint64_t page = early_next;
            early_next += PAGE_SIZE;
            memset(phys_to_virt(page), 0, PAGE_SIZE);
            return page;
        }
    }
    panic("early_alloc_page: out of memory");
}

/* Returns the next-level table for `entry`, creating it if needed.
 * Returns NULL if `entry` is already a huge page (so the address is mapped). */
static uint64_t *next_table(uint64_t *entry) {
    if (!(*entry & PTE_PRESENT)) {
        *entry = early_alloc_page() | PTE_PRESENT | PTE_WRITE;
    } else if (*entry & PTE_HUGE) {
        return NULL;
    }
    return phys_to_virt(*entry & PTE_ADDR_MASK);
}

void *map_phys(uint64_t phys, size_t size, enum map_cache cache) {
    uint64_t flags = PTE_PRESENT | PTE_WRITE;
    if (cache == MAP_UNCACHED) {
        flags |= PTE_PCD | PTE_PWT; /* PAT entry 3: uncacheable. */
    }
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t *pml4 = phys_to_virt(read_cr3() & PTE_ADDR_MASK);

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t virt = (uint64_t)phys_to_virt(page);
        uint64_t *pdpt = next_table(&pml4[(virt >> 39) & 0x1ff]);
        uint64_t *pd = pdpt ? next_table(&pdpt[(virt >> 30) & 0x1ff]) : NULL;
        uint64_t *pt = pd ? next_table(&pd[(virt >> 21) & 0x1ff]) : NULL;
        if (!pt) {
            continue; /* Covered by a huge page. */
        }
        uint64_t *pte = &pt[(virt >> 12) & 0x1ff];
        if (!(*pte & PTE_PRESENT)) {
            *pte = page | flags;
            invlpg(virt);
        }
    }
    return phys_to_virt(phys);
}
