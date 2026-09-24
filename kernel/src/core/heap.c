#include <stdbool.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

/*
 * Kernel heap.
 *
 * Small allocations (up to 1 KiB) come from slab caches, one per power-of-two
 * size. A slab is one page: a header followed by equal-sized objects, with the
 * free ones chained in a list. kfree() finds the header by rounding the pointer
 * down to its page.
 *
 * Larger allocations take whole pages from the buddy allocator, with a small
 * header in front so kfree() knows how many.
 *
 * heap_lock makes it safe from any CPU and from interrupt handlers.
 */

#define SLAB_MAGIC 0x51ab51ab
#define LARGE_MAGIC 0x1a26e000
#define MIN_SHIFT 4  /* 16 bytes */
#define MAX_SHIFT 10 /* 1 KiB */
#define CACHE_COUNT (MAX_SHIFT - MIN_SHIFT + 1)
#define SLAB_HEADER_SIZE 64

struct slab_cache;

struct slab {
    uint32_t magic;
    uint16_t in_use;
    uint16_t capacity;
    struct slab_cache *cache;
    struct slab *next;
    struct slab *prev;
    void *free_list;
};

struct slab_cache {
    size_t object_size;
    struct slab *partial; /* Slabs with at least one free object. */
    struct slab *full;
    struct slab *empty;   /* At most one fully free slab, kept to avoid churn. */
};

struct large_header {
    uint32_t magic;
    uint32_t order;
    uint64_t size;
};

_Static_assert(sizeof(struct slab) <= SLAB_HEADER_SIZE, "slab header too big");
_Static_assert(sizeof(struct large_header) == 16, "large header must keep 16-byte alignment");

static struct spinlock heap_lock = SPINLOCK_INIT;
static struct slab_cache caches[CACHE_COUNT];
static uint64_t slab_pages, large_pages, live_allocations;

static void list_insert(struct slab **head, struct slab *slab) {
    slab->prev = NULL;
    slab->next = *head;
    if (*head) {
        (*head)->prev = slab;
    }
    *head = slab;
}

static void list_unlink(struct slab **head, struct slab *slab) {
    if (slab->prev) {
        slab->prev->next = slab->next;
    } else {
        *head = slab->next;
    }
    if (slab->next) {
        slab->next->prev = slab->prev;
    }
    slab->next = slab->prev = NULL;
}

static struct slab *slab_create(struct slab_cache *cache) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return NULL;
    }
    struct slab *slab = phys_to_virt(phys);
    *slab = (struct slab){
        .magic = SLAB_MAGIC,
        .capacity = (PAGE_SIZE - SLAB_HEADER_SIZE) / cache->object_size,
        .cache = cache,
    };
    /* Chain every object into the free list. */
    uint8_t *objects = (uint8_t *)slab + SLAB_HEADER_SIZE;
    for (int i = slab->capacity - 1; i >= 0; i--) {
        void **object = (void **)(objects + i * cache->object_size);
        *object = slab->free_list;
        slab->free_list = object;
    }
    slab_pages++;
    return slab;
}

static void slab_destroy(struct slab *slab) {
    slab->magic = 0;
    pmm_free(virt_to_phys(slab), 0);
    slab_pages--;
}

static void *slab_alloc(struct slab_cache *cache) {
    struct slab *slab = cache->partial;
    if (!slab) {
        if (cache->empty) {
            slab = cache->empty;
            cache->empty = NULL;
        } else if (!(slab = slab_create(cache))) {
            return NULL;
        }
        list_insert(&cache->partial, slab);
    }
    void **object = slab->free_list;
    slab->free_list = *object;
    if (++slab->in_use == slab->capacity) {
        list_unlink(&cache->partial, slab);
        list_insert(&cache->full, slab);
    }
    return object;
}

static void slab_free(struct slab *slab, void *ptr) {
    struct slab_cache *cache = slab->cache;
    uint64_t offset = (uint64_t)ptr - (uint64_t)slab - SLAB_HEADER_SIZE;
    if ((uint64_t)ptr < (uint64_t)slab + SLAB_HEADER_SIZE || offset % cache->object_size != 0 ||
        slab->in_use == 0) {
        panic("kfree: bad pointer %p", ptr);
    }
    if (slab->in_use == slab->capacity) {
        list_unlink(&cache->full, slab);
        list_insert(&cache->partial, slab);
    }
    *(void **)ptr = slab->free_list;
    slab->free_list = ptr;
    if (--slab->in_use == 0) {
        list_unlink(&cache->partial, slab);
        if (cache->empty) {
            slab_destroy(slab);
        } else {
            cache->empty = slab;
        }
    }
}

static void *large_alloc(size_t size) {
    unsigned order = 0;
    while ((PAGE_SIZE << order) < size + sizeof(struct large_header)) {
        if (++order > PMM_MAX_ORDER) {
            return NULL;
        }
    }
    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return NULL;
    }
    struct large_header *header = phys_to_virt(phys);
    *header = (struct large_header){.magic = LARGE_MAGIC, .order = order, .size = size};
    large_pages += 1ULL << order;
    return header + 1;
}

void *kmalloc(size_t size) {
    if (size == 0) {
        size = 1;
    }
    void *ptr;
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    if (size <= (1U << MAX_SHIFT)) {
        unsigned shift = MIN_SHIFT;
        while ((1UL << shift) < size) {
            shift++;
        }
        struct slab_cache *cache = &caches[shift - MIN_SHIFT];
        cache->object_size = 1UL << shift;
        ptr = slab_alloc(cache);
    } else {
        ptr = large_alloc(size);
    }
    if (ptr) {
        live_allocations++;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return ptr;
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }
    void *page = (void *)((uint64_t)ptr & ~(PAGE_SIZE - 1));
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t magic = *(uint32_t *)page;
    if (magic == SLAB_MAGIC) {
        slab_free(page, ptr);
    } else if (magic == LARGE_MAGIC && ptr == (struct large_header *)page + 1) {
        struct large_header *header = page;
        header->magic = 0;
        large_pages -= 1ULL << header->order;
        pmm_free(virt_to_phys(header), header->order);
    } else {
        panic("kfree: %p did not come from kmalloc", ptr);
    }
    live_allocations--;
    spin_unlock_irqrestore(&heap_lock, flags);
}

void heap_trim(void) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    for (int i = 0; i < CACHE_COUNT; i++) {
        if (caches[i].empty) {
            slab_destroy(caches[i].empty);
            caches[i].empty = NULL;
        }
    }
    spin_unlock_irqrestore(&heap_lock, flags);
}

void heap_get_stats(struct heap_stats *stats) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    stats->slab_pages = slab_pages;
    stats->large_pages = large_pages;
    stats->allocations = live_allocations;
    spin_unlock_irqrestore(&heap_lock, flags);
}
