#include <vexa/abi.h>
#include <vexa/block.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/string.h>

/*
 * Block devices and the block cache.
 *
 * The cache holds 4 KiB chunks of whole disks (partitions map onto their disk,
 * so they share it). Reads fill it; writes update it and go straight to the
 * disk ("write-through"), so there is never unsaved data to lose.
 */

#define CHUNK_SIZE 4096
#define CACHE_ENTRIES 512 /* 2 MiB */

struct cache_entry {
    struct block_device *device;
    uint64_t chunk;
    uint64_t last_used;
    uint8_t *data;
};

static struct mutex cache_lock = MUTEX_INIT;
static struct cache_entry cache[CACHE_ENTRIES];
static uint64_t use_clock;
static struct block_device *devices;

uint64_t block_size_bytes(struct block_device *device) {
    return device->sector_count * device->sector_size;
}

struct block_device *block_first(void) {
    return devices;
}

struct block_device *block_find(const char *name) {
    for (struct block_device *device = devices; device; device = device->next) {
        if (strcmp(device->name, name) == 0) {
            return device;
        }
    }
    return NULL;
}

/* Reads or writes the part of a chunk that exists on the disk. */
static int chunk_io(struct block_device *disk, uint64_t chunk, uint8_t *data, bool write,
                    uint32_t first, uint32_t count) {
    uint32_t per_chunk = CHUNK_SIZE / disk->sector_size;
    uint64_t sector = chunk * per_chunk + first;
    if (sector >= disk->sector_count) {
        return 0;
    }
    if (sector + count > disk->sector_count) {
        count = disk->sector_count - sector;
    }
    uint8_t *at = data + first * disk->sector_size;
    return write ? disk->write(disk, sector, count, at) : disk->read(disk, sector, count, at);
}

/* Returns the cache entry for a chunk, reading it from disk if needed. */
static struct cache_entry *get_chunk(struct block_device *disk, uint64_t chunk, int *error) {
    struct cache_entry *victim = NULL;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        struct cache_entry *entry = &cache[i];
        if (entry->device == disk && entry->chunk == chunk) {
            entry->last_used = ++use_clock;
            return entry;
        }
        if (!victim || !entry->device || (victim->device && entry->last_used < victim->last_used)) {
            victim = entry;
        }
    }
    if (!victim->data) {
        uint64_t phys = pmm_alloc(0);
        if (!phys) {
            *error = -VX_ENOMEM;
            return NULL;
        }
        victim->data = phys_to_virt(phys);
    }
    victim->device = NULL;
    memset(victim->data, 0, CHUNK_SIZE);
    int result = chunk_io(disk, chunk, victim->data, false, 0, CHUNK_SIZE / disk->sector_size);
    if (result) {
        kprintf("[block] read error on %s near byte %lu\n", disk->name, chunk * CHUNK_SIZE);
        *error = -VX_EIO;
        return NULL;
    }
    victim->device = disk;
    victim->chunk = chunk;
    victim->last_used = ++use_clock;
    return victim;
}

static struct block_device *whole_disk(struct block_device *device, uint64_t *offset) {
    if (device->parent) {
        *offset += device->first_sector * device->sector_size;
        return device->parent;
    }
    return device;
}

static int transfer(struct block_device *device, uint64_t offset, void *buffer, size_t size,
                    bool write) {
    if (offset > block_size_bytes(device) || size > block_size_bytes(device) - offset) {
        return -VX_EINVAL;
    }
    struct block_device *disk = whole_disk(device, &offset);
    if (write && !disk->write) {
        return -VX_EROFS;
    }
    mutex_lock(&cache_lock);
    int error = 0;
    for (size_t done = 0; done < size && !error;) {
        uint64_t chunk = (offset + done) / CHUNK_SIZE;
        uint32_t within = (offset + done) % CHUNK_SIZE;
        size_t n = CHUNK_SIZE - within < size - done ? CHUNK_SIZE - within : size - done;
        struct cache_entry *entry = get_chunk(disk, chunk, &error);
        if (!entry) {
            break;
        }
        if (write) {
            memcpy(entry->data + within, (const uint8_t *)buffer + done, n);
            uint32_t first = within / disk->sector_size;
            uint32_t last = (within + n - 1) / disk->sector_size;
            if (chunk_io(disk, chunk, entry->data, true, first, last - first + 1)) {
                kprintf("[block] write error on %s near byte %lu\n", disk->name, offset + done);
                entry->device = NULL; /* Don't trust the cached copy any more. */
                error = -VX_EIO;
            }
        } else {
            memcpy((uint8_t *)buffer + done, entry->data + within, n);
        }
        done += n;
    }
    mutex_unlock(&cache_lock);
    return error;
}

int block_read_bytes(struct block_device *device, uint64_t offset, void *buffer, size_t size) {
    return transfer(device, offset, buffer, size, false);
}

int block_write_bytes(struct block_device *device, uint64_t offset, const void *buffer,
                      size_t size) {
    return transfer(device, offset, (void *)buffer, size, true);
}

/* ---- Partitions ---- */

static int partition_read(struct block_device *p, uint64_t sector, uint32_t count, void *buffer) {
    return p->parent->read(p->parent, p->first_sector + sector, count, buffer);
}

static int partition_write(struct block_device *p, uint64_t sector, uint32_t count,
                           const void *buffer) {
    return p->parent->write(p->parent, p->first_sector + sector, count, buffer);
}

static void add_device(struct block_device *device) {
    struct block_device **link = &devices;
    while (*link) {
        link = &(*link)->next;
    }
    device->next = NULL;
    *link = device;
    devfs_add_block_device(device);
}

static void add_partition(struct block_device *disk, int number, uint64_t first, uint64_t last) {
    if (first == 0 || last < first || last >= disk->sector_count) {
        return;
    }
    struct block_device *part = kzalloc(sizeof(*part));
    if (!part) {
        return;
    }
    /* "vda" -> "vda1", but "nvme0n1" -> "nvme0n1p1" so the numbers don't run together. */
    size_t n = strlen(disk->name);
    memcpy(part->name, disk->name, n);
    if (disk->name[n - 1] >= '0' && disk->name[n - 1] <= '9') {
        part->name[n++] = 'p';
    }
    if (number >= 10) {
        part->name[n++] = (char)('0' + number / 10);
    }
    part->name[n++] = (char)('0' + number % 10);
    part->sector_count = last - first + 1;
    part->sector_size = disk->sector_size;
    part->read = partition_read;
    part->write = disk->write ? partition_write : NULL;
    part->parent = disk;
    part->first_sector = first;
    add_device(part);
}

struct __attribute__((packed)) gpt_header {
    char signature[8]; /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
};

struct __attribute__((packed)) gpt_entry {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
};

struct __attribute__((packed)) mbr_entry {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t first_lba;
    uint32_t sector_count;
};

static bool scan_gpt(struct block_device *disk) {
    struct gpt_header header;
    if (block_read_bytes(disk, disk->sector_size, &header, sizeof(header)) ||
        memcmp(header.signature, "EFI PART", 8) != 0 || header.entry_size < sizeof(struct gpt_entry)) {
        return false;
    }
    int number = 0;
    for (uint32_t i = 0; i < header.entry_count && i < 128; i++) {
        struct gpt_entry entry;
        uint64_t at = header.entries_lba * disk->sector_size + (uint64_t)i * header.entry_size;
        if (block_read_bytes(disk, at, &entry, sizeof(entry))) {
            break;
        }
        static const uint8_t unused[16];
        if (memcmp(entry.type_guid, unused, 16) != 0) {
            add_partition(disk, ++number, entry.first_lba, entry.last_lba);
        }
    }
    return true;
}

static void scan_mbr(struct block_device *disk) {
    uint8_t sector[512];
    if (disk->sector_size != 512 || block_read_bytes(disk, 0, sector, sizeof(sector)) ||
        sector[510] != 0x55 || sector[511] != 0xaa) {
        return;
    }
    const struct mbr_entry *entries = (const void *)(sector + 446);
    for (int i = 0; i < 4; i++) {
        /* Skip empty and extended partitions (logical partitions: later). */
        if (entries[i].type == 0 || entries[i].type == 0x05 || entries[i].type == 0x0f ||
            entries[i].type == 0x85) {
            continue;
        }
        /* A disk that is itself a file system (no partition table) can still
         * end in 0x55aa by chance; a real MBR entry must fit on the disk. */
        uint64_t first = entries[i].first_lba, count = entries[i].sector_count;
        if (count && first + count <= disk->sector_count) {
            add_partition(disk, i + 1, first, first + count - 1);
        }
    }
}

void block_register(struct block_device *device) {
    kprintf("[block] %s: %lu MiB (%u-byte sectors)\n", device->name,
            block_size_bytes(device) / (1024 * 1024), device->sector_size);
    add_device(device);
    if (!scan_gpt(device)) {
        scan_mbr(device);
    }
}
