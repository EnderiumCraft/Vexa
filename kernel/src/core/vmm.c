#include <limine.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_HUGE (1ULL << 7)     /* In a PD or PDPT entry: maps 2 MiB / 1 GiB. */
#define PTE_PAT_4K (1ULL << 7)   /* In a PT entry: third PAT index bit. */
#define PTE_PAT_2M (1ULL << 12)  /* The same bit, in a 2 MiB entry. */
#define PTE_NX (1ULL << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL

#define LARGE_PAGE_SIZE (2ULL * 1024 * 1024)

#define IA32_EFER_MSR 0xc0000080
#define EFER_NXE (1ULL << 11)

/* Kernel stacks live in their own 512 GiB slot of the address space. */
#define KERNEL_STACKS_BASE 0xffffff0000000000ULL
#define KERNEL_STACKS_END 0xffffff8000000000ULL

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[],
    __rodata_end[], __data_start[], __kernel_end[];

uint64_t hhdm_offset;

/* Protects the kernel's page tables, and user ones while being changed. */
static struct spinlock vmm_lock = SPINLOCK_INIT;
static uint64_t *kernel_pml4;
static uint64_t nx_bit;
static uint64_t next_stack = KERNEL_STACKS_BASE;

static uint64_t cache_bits(enum map_cache cache) {
    switch (cache) {
    case MAP_WRITECOMBINE: return PTE_PAT_4K | PTE_PWT; /* PAT index 5. */
    case MAP_UNCACHED: return PTE_PCD | PTE_PWT;        /* PAT index 3. */
    default: return 0;                                  /* PAT index 0: WB. */
    }
}

/* Returns the table that `entry` points to, creating it if `create` is set.
 * Returns NULL if the entry is missing or maps a huge page. */
static uint64_t *next_table(uint64_t *entry, bool create) {
    if (!(*entry & PTE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        *entry = pmm_alloc_zeroed_page() | PTE_PRESENT | PTE_WRITE;
    } else if (*entry & PTE_HUGE) {
        return NULL;
    }
    return phys_to_virt(*entry & PTE_ADDR_MASK);
}

/* Returns the page directory entry covering `virt`, or NULL if a 1 GiB page covers it. */
static uint64_t *pd_entry(uint64_t virt, bool create) {
    uint64_t *pdpt = next_table(&kernel_pml4[(virt >> 39) & 0x1ff], create);
    uint64_t *pd = pdpt ? next_table(&pdpt[(virt >> 30) & 0x1ff], create) : NULL;
    return pd ? &pd[(virt >> 21) & 0x1ff] : NULL;
}

static uint64_t *pt_entry(uint64_t virt, bool create) {
    uint64_t *pde = pd_entry(virt, create);
    uint64_t *pt = pde ? next_table(pde, create) : NULL;
    return pt ? &pt[(virt >> 12) & 0x1ff] : NULL;
}

static uint64_t flags_for(unsigned vmm_flags, enum map_cache cache) {
    uint64_t flags = PTE_PRESENT | cache_bits(cache);
    if (vmm_flags & VMM_WRITE) {
        flags |= PTE_WRITE;
    }
    if (!(vmm_flags & VMM_EXEC)) {
        flags |= nx_bit;
    }
    return flags;
}

/* Maps [virt, virt + size) to [phys, phys + size). Uses 2 MiB pages where
 * possible. Pages that are already mapped are left alone. */
static void map_range(uint64_t virt, uint64_t phys, uint64_t size, unsigned vmm_flags,
                      enum map_cache cache) {
    uint64_t flags = flags_for(vmm_flags, cache);
    uint64_t end = virt + size;
    while (virt < end) {
        if ((virt | phys) % LARGE_PAGE_SIZE == 0 && end - virt >= LARGE_PAGE_SIZE) {
            uint64_t *pde = pd_entry(virt, true);
            if (pde && !(*pde & PTE_PRESENT)) {
                uint64_t large_flags = flags;
                if (large_flags & PTE_PAT_4K) {
                    large_flags = (large_flags & ~PTE_PAT_4K) | PTE_PAT_2M;
                }
                *pde = phys | large_flags | PTE_HUGE;
                virt += LARGE_PAGE_SIZE;
                phys += LARGE_PAGE_SIZE;
                continue;
            }
        }
        uint64_t *pte = pt_entry(virt, true);
        if (pte && !(*pte & PTE_PRESENT)) {
            *pte = phys | flags;
        }
        virt += PAGE_SIZE;
        phys += PAGE_SIZE;
    }
}

static void map_kernel_section(const char *start, const char *end, uint64_t kernel_phys,
                               uint64_t kernel_virt, unsigned vmm_flags) {
    uint64_t virt = (uint64_t)start & ~(PAGE_SIZE - 1);
    uint64_t size = (((uint64_t)end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - virt;
    map_range(virt, kernel_phys + (virt - kernel_virt), size, vmm_flags, MAP_WRITEBACK);
}

void vmm_init(const struct mem_range *ranges, size_t count, uint64_t kernel_phys,
              uint64_t kernel_virt) {
    if (rdmsr(IA32_EFER_MSR) & EFER_NXE) {
        nx_bit = PTE_NX;
    }
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    bool has_pat = d & (1U << 16); /* cpu_enable_features() programmed it. */

    kernel_pml4 = phys_to_virt(pmm_alloc_zeroed_page());
    /* Give the whole upper half its page directory pointer tables now. Every
     * address space copies these 256 entries, so kernel mappings made later
     * show up everywhere without touching each process. */
    for (int i = 256; i < 512; i++) {
        kernel_pml4[i] = pmm_alloc_zeroed_page() | PTE_PRESENT | PTE_WRITE;
    }

    /* The direct map: all RAM, ACPI memory and the framebuffer. Device
     * registers are added on demand by map_phys(). */
    uint64_t highest = 0;
    for (size_t i = 0; i < count; i++) {
        enum map_cache cache = MAP_WRITEBACK;
        switch (ranges[i].type) {
        case LIMINE_MEMMAP_FRAMEBUFFER:
            cache = has_pat ? MAP_WRITECOMBINE : MAP_UNCACHED;
            break;
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
            break;
        default:
            continue;
        }
        uint64_t base = ranges[i].base & ~(PAGE_SIZE - 1);
        uint64_t end = (ranges[i].base + ranges[i].length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        map_range((uint64_t)phys_to_virt(base), base, end - base, VMM_WRITE, cache);
        highest = end > highest ? end : highest;
    }
    if ((uint64_t)phys_to_virt(highest) > KERNEL_STACKS_BASE) {
        panic("vmm: direct map would overlap the kernel stack area");
    }

    /* The kernel image, with the least access each part needs. */
    map_kernel_section(__kernel_start, __text_start, kernel_phys, kernel_virt, VMM_WRITE);
    map_kernel_section(__text_start, __text_end, kernel_phys, kernel_virt, VMM_EXEC);
    map_kernel_section(__rodata_start, __rodata_end, kernel_phys, kernel_virt, 0);
    map_kernel_section(__data_start, __kernel_end, kernel_phys, kernel_virt, VMM_WRITE);

    __asm__ volatile("mov %0, %%cr3" : : "r"(virt_to_phys(kernel_pml4)) : "memory");
    kprintf("[vmm] kernel page tables active (NX %s, PAT %s)\n",
            nx_bit ? "on" : "unsupported", has_pat ? "on" : "unsupported");
}

void *map_phys(uint64_t phys, size_t size, enum map_cache cache) {
    uint64_t lock_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t flags = flags_for(VMM_WRITE, cache);
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t virt = (uint64_t)phys_to_virt(page);
        uint64_t *pte = pt_entry(virt, true);
        if (pte && !(*pte & PTE_PRESENT)) { /* NULL: inside a 2 MiB page, already mapped. */
            *pte = page | flags;
            invlpg(virt);
        }
    }
    spin_unlock_irqrestore(&vmm_lock, lock_flags);
    return phys_to_virt(phys);
}

uint64_t vmm_alloc_kernel_stack(size_t size) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t lock_flags = spin_lock_irqsave(&vmm_lock);
    if (next_stack + PAGE_SIZE + size > KERNEL_STACKS_END) {
        panic("vmm: out of kernel stack address space");
    }
    uint64_t bottom = next_stack + PAGE_SIZE; /* Leave the guard page unmapped. */
    for (uint64_t virt = bottom; virt < bottom + size; virt += PAGE_SIZE) {
        *pt_entry(virt, true) = pmm_alloc_zeroed_page() | flags_for(VMM_WRITE, MAP_WRITEBACK);
    }
    next_stack = bottom + size;
    uint64_t top = next_stack;
    spin_unlock_irqrestore(&vmm_lock, lock_flags);
    return top;
}

bool vmm_is_stack_guard(uint64_t virt) {
    if (virt < KERNEL_STACKS_BASE || virt >= next_stack) {
        return false;
    }
    uint64_t *pte = pt_entry(virt, false);
    return !pte || !(*pte & PTE_PRESENT);
}

/* ---- User address spaces ---- */

static uint64_t *active_pml4(void) {
    return phys_to_virt(read_cr3() & PTE_ADDR_MASK);
}

/* Like pt_entry(), but in a user address space and creating user-accessible
 * intermediate tables. */
static uint64_t *user_pte(uint64_t *pml4, uint64_t virt, bool create) {
    uint64_t *table = pml4;
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
        } else if (*entry & PTE_HUGE) {
            return NULL;
        }
        table = phys_to_virt(*entry & PTE_ADDR_MASK);
    }
    return &table[(virt >> 12) & 0x1ff];
}

struct address_space *vmm_create_address_space(void) {
    struct address_space *as = kzalloc(sizeof(*as));
    uint64_t phys = as ? pmm_alloc(0) : 0;
    if (!phys) {
        kfree(as);
        return NULL;
    }
    as->pml4_phys = phys;
    as->pml4 = phys_to_virt(phys);
    memset(as->pml4, 0, PAGE_SIZE / 2);
    memcpy(as->pml4 + 256, kernel_pml4 + 256, PAGE_SIZE / 2);
    return as;
}

static void free_table(uint64_t *table, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t phys = table[i] & PTE_ADDR_MASK;
        if (level > 1) {
            free_table(phys_to_virt(phys), level - 1);
        }
        pmm_free(phys, 0); /* A lower-level table, or (level 1) a user page. */
    }
}

void vmm_destroy_address_space(struct address_space *as) {
    for (int i = 0; i < 256; i++) {
        if (as->pml4[i] & PTE_PRESENT) {
            uint64_t *pdpt = phys_to_virt(as->pml4[i] & PTE_ADDR_MASK);
            free_table(pdpt, 3);
            pmm_free(as->pml4[i] & PTE_ADDR_MASK, 0);
        }
    }
    pmm_free(as->pml4_phys, 0);
    kfree(as);
}

void vmm_activate(struct address_space *as) {
    uint64_t phys = as ? as->pml4_phys : virt_to_phys(kernel_pml4);
    if ((read_cr3() & PTE_ADDR_MASK) != phys) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
    }
}

uint64_t vmm_map_user_page(struct address_space *as, uint64_t virt, unsigned flags) {
    virt &= ~(PAGE_SIZE - 1);
    if (virt < USER_BASE || virt >= USER_END) {
        return 0;
    }
    uint64_t lock_flags = spin_lock_irqsave(&vmm_lock);
    uint64_t phys = 0;
    uint64_t *pte = user_pte(as->pml4, virt, true);
    if (pte) {
        if (*pte & PTE_PRESENT) {
            phys = *pte & PTE_ADDR_MASK;
            if (flags & VMM_WRITE) {
                *pte |= PTE_WRITE;
            }
            if (flags & VMM_EXEC) {
                *pte &= ~PTE_NX;
            }
        } else if ((phys = pmm_alloc(0))) {
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            *pte = phys | flags_for(flags, MAP_WRITEBACK) | PTE_USER;
        }
    }
    spin_unlock_irqrestore(&vmm_lock, lock_flags);
    return phys;
}

bool vmm_user_range_mapped(uint64_t virt, uint64_t size, bool write) {
    if (virt < USER_BASE || virt + size < virt || virt + size > USER_END) {
        return false;
    }
    uint64_t *pml4 = active_pml4();
    uint64_t lock_flags = spin_lock_irqsave(&vmm_lock);
    bool ok = true;
    for (uint64_t page = virt & ~(PAGE_SIZE - 1); ok && page < virt + size; page += PAGE_SIZE) {
        uint64_t *pte = user_pte(pml4, page, false);
        ok = pte && (*pte & PTE_PRESENT) && (*pte & PTE_USER) && (!write || (*pte & PTE_WRITE));
    }
    spin_unlock_irqrestore(&vmm_lock, lock_flags);
    return ok;
}
