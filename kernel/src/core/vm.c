#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include "paging.h"

/* Where vm_map() looks for room: downward from here, above the program. */
#define MAP_TOP 0x00007f0000000000ULL
#define MAP_BOTTOM 0x0000000100000000ULL

static uint64_t page_down(uint64_t address) {
    return address & ~(PAGE_SIZE - 1);
}

static uint64_t page_up(uint64_t address) {
    return (address + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static bool is_active(struct address_space *as) {
    return (read_cr3() & PTE_ADDR_MASK) == as->pml4_phys;
}

/* Returns the page table entry for a user address, creating the tables on
 * the way if `create`. */
static uint64_t *user_pte(struct address_space *as, uint64_t virt, bool create) {
    uint64_t *table = as->pml4;
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *entry = &table[(virt >> shift) & 0x1ff];
        if (!(*entry & PTE_PRESENT)) {
            if (!create) {
                return NULL;
            }
            uint64_t phys = pmm_alloc(0);
            if (!phys) {
                return NULL;
            }
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            *entry = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        table = phys_to_virt(*entry & PTE_ADDR_MASK);
    }
    return &table[(virt >> 12) & 0x1ff];
}

static uint64_t pte_flags(unsigned flags, bool writable) {
    uint64_t pte = PTE_PRESENT | PTE_USER;
    if ((flags & VM_WRITE) && writable) {
        pte |= PTE_WRITE;
    }
    if (!(flags & VM_EXEC)) {
        pte |= vmm_nx_bit();
    }
    return pte;
}

static void flush(struct address_space *as, uint64_t virt) {
    if (is_active(as)) {
        invlpg(virt);
    }
}

/* ---- Area list ---- */

static struct vm_area *find_area(struct address_space *as, uint64_t address) {
    for (struct vm_area *area = as->areas; area; area = area->next) {
        if (address >= area->start && address < area->end) {
            return area;
        }
    }
    return NULL;
}

static void insert_area(struct address_space *as, struct vm_area *area) {
    struct vm_area **link = &as->areas;
    while (*link && (*link)->start < area->start) {
        link = &(*link)->next;
    }
    area->next = *link;
    *link = area;
}

static struct vm_area *new_area(uint64_t start, uint64_t end, unsigned flags) {
    struct vm_area *area = kmalloc(sizeof(*area));
    if (area) {
        *area = (struct vm_area){.start = start, .end = end, .flags = flags};
    }
    return area;
}

/* Splits the area containing `address` (if any) so an area boundary falls there. */
static bool split_at(struct address_space *as, uint64_t address) {
    struct vm_area *area = find_area(as, address);
    if (!area || area->start == address) {
        return true;
    }
    struct vm_area *tail = new_area(address, area->end, area->flags);
    if (!tail) {
        return false;
    }
    area->end = address;
    tail->next = area->next;
    area->next = tail;
    return true;
}

/* Merges neighbouring areas with the same flags, to keep the list short. */
static void merge_areas(struct address_space *as) {
    for (struct vm_area *area = as->areas; area && area->next;) {
        struct vm_area *next = area->next;
        if (area->end == next->start && area->flags == next->flags) {
            area->end = next->end;
            area->next = next->next;
            kfree(next);
        } else {
            area = next;
        }
    }
}

/* ---- Creating and destroying ---- */

struct address_space *vm_create(void) {
    struct address_space *as = kzalloc(sizeof(*as));
    uint64_t phys = as ? pmm_alloc(0) : 0;
    if (!phys) {
        kfree(as);
        return NULL;
    }
    as->refs = 1;
    as->pml4_phys = phys;
    as->pml4 = phys_to_virt(phys);
    memset(as->pml4, 0, PAGE_SIZE / 2);
    /* The kernel half is shared: every address space points at the same tables. */
    memcpy(as->pml4 + 256, (uint64_t *)phys_to_virt(vmm_kernel_pml4_phys()) + 256, PAGE_SIZE / 2);
    return as;
}

static void free_tables(uint64_t *table, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t phys = table[i] & PTE_ADDR_MASK;
        if (level > 1) {
            free_tables(phys_to_virt(phys), level - 1);
            pmm_free(phys, 0);
        } else {
            page_ref_put(phys); /* A user page, maybe shared with a forked process. */
        }
    }
}

void vm_get(struct address_space *as) {
    __atomic_add_fetch(&as->refs, 1, __ATOMIC_RELAXED);
}

static void vm_destroy(struct address_space *as);

void vm_put(struct address_space *as) {
    if (__atomic_sub_fetch(&as->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        vm_destroy(as);
    }
}

static void vm_destroy(struct address_space *as) {
    for (int i = 0; i < 256; i++) {
        if (as->pml4[i] & PTE_PRESENT) {
            free_tables(phys_to_virt(as->pml4[i] & PTE_ADDR_MASK), 3);
            pmm_free(as->pml4[i] & PTE_ADDR_MASK, 0);
        }
    }
    pmm_free(as->pml4_phys, 0);
    while (as->areas) {
        struct vm_area *next = as->areas->next;
        kfree(as->areas);
        as->areas = next;
    }
    kfree(as);
}

/* Shares every present page of `from` with `to`, read-only in both. */
static bool share_tables(uint64_t *from, uint64_t *to_pml4, int level, uint64_t base) {
    for (int i = 0; i < 512; i++) {
        if (!(from[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t address = base + ((uint64_t)i << (12 + 9 * (level - 1)));
        if (level > 1) {
            if (!share_tables(phys_to_virt(from[i] & PTE_ADDR_MASK), to_pml4, level - 1, address)) {
                return false;
            }
            continue;
        }
        from[i] &= ~PTE_WRITE;
        struct address_space to = {.pml4 = to_pml4};
        uint64_t *pte = user_pte(&to, address, true);
        if (!pte) {
            return false;
        }
        *pte = from[i];
        page_ref_get(from[i] & PTE_ADDR_MASK);
    }
    return true;
}

struct address_space *vm_fork(struct address_space *parent) {
    struct address_space *child = vm_create();
    if (!child) {
        return NULL;
    }
    uint64_t flags = spin_lock_irqsave(&parent->lock);
    bool ok = true;
    for (struct vm_area *area = parent->areas; area && ok; area = area->next) {
        struct vm_area *copy = new_area(area->start, area->end, area->flags);
        ok = copy != NULL;
        if (copy) {
            insert_area(child, copy);
        }
    }
    child->heap_start = parent->heap_start;
    child->heap_end = parent->heap_end;
    for (int i = 0; i < 256 && ok; i++) {
        if (parent->pml4[i] & PTE_PRESENT) {
            ok = share_tables(phys_to_virt(parent->pml4[i] & PTE_ADDR_MASK), child->pml4, 3,
                              (uint64_t)i << 39);
        }
    }
    spin_unlock_irqrestore(&parent->lock, flags);
    if (is_active(parent)) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(parent->pml4_phys) : "memory"); /* Flush. */
    }
    if (!ok) {
        vm_put(child);
        return NULL;
    }
    return child;
}

/* ---- Adding and removing areas ---- */

int vm_add_area(struct address_space *as, uint64_t start, uint64_t end, unsigned flags) {
    start = page_down(start);
    end = page_up(end);
    if (start < USER_BASE || end > USER_END || start >= end) {
        return -VX_EINVAL;
    }
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    int error = split_at(as, start) && split_at(as, end) ? 0 : -VX_ENOMEM;
    /* Existing parts inside the range take the extra flags; gaps get new areas. */
    uint64_t cursor = start;
    for (struct vm_area *area = as->areas; area && !error && cursor < end; area = area->next) {
        if (area->end <= cursor) {
            continue;
        }
        if (area->start >= end) {
            break;
        }
        if (area->start > cursor) {
            struct vm_area *gap = new_area(cursor, area->start, flags);
            if (!gap) {
                error = -VX_ENOMEM;
                break;
            }
            insert_area(as, gap);
        }
        area->flags |= flags;
        cursor = area->end;
    }
    if (!error && cursor < end) {
        struct vm_area *gap = new_area(cursor, end, flags);
        if (gap) {
            insert_area(as, gap);
        } else {
            error = -VX_ENOMEM;
        }
    }
    merge_areas(as);
    spin_unlock_irqrestore(&as->lock, lock_flags);
    return error;
}

static bool range_free(struct address_space *as, uint64_t start, uint64_t end) {
    for (struct vm_area *area = as->areas; area; area = area->next) {
        if (area->start < end && start < area->end) {
            return false;
        }
    }
    return true;
}

uint64_t vm_map(struct address_space *as, uint64_t size, unsigned flags) {
    size = page_up(size);
    if (size == 0 || size > MAP_TOP - MAP_BOTTOM) {
        return 0;
    }
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    uint64_t candidate = MAP_TOP - size;
    while (candidate >= MAP_BOTTOM && !range_free(as, candidate, candidate + size)) {
        /* Move below the lowest area that overlaps the candidate. */
        uint64_t lowest = candidate;
        for (struct vm_area *area = as->areas; area; area = area->next) {
            if (area->start < candidate + size && candidate < area->end && area->start < lowest) {
                lowest = area->start;
            }
        }
        if (lowest < size) {
            candidate = 0;
            break;
        }
        candidate = lowest - size;
    }
    spin_unlock_irqrestore(&as->lock, lock_flags);
    if (candidate < MAP_BOTTOM || vm_add_area(as, candidate, candidate + size, flags)) {
        return 0;
    }
    return candidate;
}

/* Removes the pages of [start, end) from the page tables. */
static void unmap_pages(struct address_space *as, uint64_t start, uint64_t end) {
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t *pte = user_pte(as, page, false);
        if (pte && (*pte & PTE_PRESENT)) {
            uint64_t phys = *pte & PTE_ADDR_MASK;
            *pte = 0;
            flush(as, page);
            page_ref_put(phys);
        }
    }
}

int vm_unmap(struct address_space *as, uint64_t start, uint64_t size) {
    uint64_t end = page_up(start + size);
    start = page_down(start);
    if (start < USER_BASE || end > USER_END || start >= end) {
        return -VX_EINVAL;
    }
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    if (!split_at(as, start) || !split_at(as, end)) {
        spin_unlock_irqrestore(&as->lock, lock_flags);
        return -VX_ENOMEM;
    }
    for (struct vm_area **link = &as->areas; *link;) {
        struct vm_area *area = *link;
        if (area->start >= start && area->end <= end) {
            *link = area->next;
            kfree(area);
        } else {
            link = &area->next;
        }
    }
    unmap_pages(as, start, end);
    spin_unlock_irqrestore(&as->lock, lock_flags);
    return 0;
}

int vm_map_fixed(struct address_space *as, uint64_t start, uint64_t size, unsigned flags) {
    int error = vm_unmap(as, start, size);
    return error ? error : vm_add_area(as, start, start + size, flags);
}

int vm_protect(struct address_space *as, uint64_t start, uint64_t size, unsigned flags) {
    uint64_t end = page_up(start + size);
    start = page_down(start);
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    int error = split_at(as, start) && split_at(as, end) ? 0 : -VX_ENOMEM;
    for (uint64_t page = start; !error && page < end; page += PAGE_SIZE) {
        if (!find_area(as, page)) {
            error = -VX_ENOMEM; /* Linux says ENOMEM for unmapped parts too. */
        }
    }
    for (struct vm_area *area = as->areas; area && !error; area = area->next) {
        if (area->start >= start && area->end <= end) {
            area->flags = flags;
        }
    }
    for (uint64_t page = start; !error && page < end; page += PAGE_SIZE) {
        uint64_t *pte = user_pte(as, page, false);
        if (pte && (*pte & PTE_PRESENT)) {
            /* Writable pages become writable on their next write fault, which
             * also takes care of pages still shared after a fork. */
            *pte = (*pte & PTE_ADDR_MASK) | pte_flags(flags, false);
            flush(as, page);
        }
    }
    merge_areas(as);
    spin_unlock_irqrestore(&as->lock, lock_flags);
    return error;
}

uint64_t vm_set_heap_end(struct address_space *as, uint64_t end) {
    if (end < as->heap_start) {
        return as->heap_end;
    }
    uint64_t old_top = page_up(as->heap_end), new_top = page_up(end);
    if (new_top > old_top) {
        uint64_t lock_flags = spin_lock_irqsave(&as->lock);
        bool free = range_free(as, old_top, new_top);
        spin_unlock_irqrestore(&as->lock, lock_flags);
        if (!free || vm_add_area(as, old_top, new_top, VM_WRITE)) {
            return as->heap_end;
        }
    } else if (new_top < old_top) {
        vm_unmap(as, new_top, old_top - new_top);
    }
    as->heap_end = end;
    return end;
}

/* ---- Faults ---- */

static uint64_t resolve(struct address_space *as, uint64_t address, bool write) {
    uint64_t page = page_down(address);
    struct vm_area *area = find_area(as, page);
    if (!area || (write && !(area->flags & VM_WRITE))) {
        return 0;
    }
    uint64_t *pte = user_pte(as, page, true);
    if (!pte) {
        return 0;
    }
    if (!(*pte & PTE_PRESENT)) {
        uint64_t phys = page_ref_new(); /* First touch: a fresh zeroed page. */
        if (!phys) {
            return 0;
        }
        *pte = phys | pte_flags(area->flags, true);
        return phys;
    }
    uint64_t phys = *pte & PTE_ADDR_MASK;
    if (write && !(*pte & PTE_WRITE)) {
        if (page_ref_count(phys) > 1) {
            /* Shared since a fork: this process gets its own copy. */
            uint64_t copy = page_ref_new();
            if (!copy) {
                return 0;
            }
            memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_SIZE);
            page_ref_put(phys);
            phys = copy;
        }
        *pte = phys | pte_flags(area->flags, true);
        flush(as, page);
    }
    return phys;
}

uint64_t vm_page_for(struct address_space *as, uint64_t address, bool write) {
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    uint64_t phys = resolve(as, address, write);
    spin_unlock_irqrestore(&as->lock, lock_flags);
    return phys;
}

bool vm_handle_fault(struct address_space *as, uint64_t address, bool write) {
    return as && address >= USER_BASE && address < USER_END && vm_page_for(as, address, write) != 0;
}

bool vm_write(struct address_space *as, uint64_t address, const void *data, size_t size) {
    for (size_t done = 0; done < size;) {
        uint64_t phys = vm_page_for(as, address + done, true);
        if (!phys) {
            return false;
        }
        size_t within = (address + done) % PAGE_SIZE;
        size_t n = PAGE_SIZE - within < size - done ? PAGE_SIZE - within : size - done;
        memcpy((uint8_t *)phys_to_virt(phys) + within, (const uint8_t *)data + done, n);
        done += n;
    }
    return true;
}

void vm_for_each_area(struct address_space *as,
                      void (*fn)(const struct vm_area *area, void *arg), void *arg) {
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    for (struct vm_area *area = as->areas; area; area = area->next) {
        fn(area, arg);
    }
    spin_unlock_irqrestore(&as->lock, lock_flags);
}

uint64_t vm_resident_bytes(struct address_space *as) {
    uint64_t pages = 0;
    uint64_t lock_flags = spin_lock_irqsave(&as->lock);
    for (struct vm_area *area = as->areas; area; area = area->next) {
        for (uint64_t page = area->start; page < area->end; page += PAGE_SIZE) {
            uint64_t *pte = user_pte(as, page, false);
            pages += pte && (*pte & PTE_PRESENT);
        }
    }
    spin_unlock_irqrestore(&as->lock, lock_flags);
    return pages * PAGE_SIZE;
}
