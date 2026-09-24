#ifndef VEXA_BLOCK_H
#define VEXA_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A disk, or a partition of one. Drivers fill in the geometry and the two
 * transfer functions; buffers passed to them are in the direct map and never
 * cross more than PMM_MAX_ORDER pages. */
struct block_device {
    char name[16];          /* e.g. "vda", "vda1", "sda", "nvme0n1" */
    uint64_t sector_count;
    uint32_t sector_size;   /* Bytes; 512 or 4096. */
    int (*read)(struct block_device *device, uint64_t sector, uint32_t count, void *buffer);
    int (*write)(struct block_device *device, uint64_t sector, uint32_t count,
                 const void *buffer);
    void *driver_data;
    struct block_device *parent; /* Partitions: the whole disk. */
    uint64_t first_sector;       /* Partitions: where they start on the disk. */
    struct block_device *next;
};

/* Adds a disk: finds its partitions (GPT or MBR) and makes /dev entries. */
void block_register(struct block_device *device);
struct block_device *block_find(const char *name);
struct block_device *block_first(void); /* Iterate with ->next. */
uint64_t block_size_bytes(struct block_device *device);

/* Byte-granular access through the block cache. Writes go straight through
 * to the disk, so nothing is lost if the machine stops. Return 0 or -VX_E*. */
int block_read_bytes(struct block_device *device, uint64_t offset, void *buffer, size_t size);
int block_write_bytes(struct block_device *device, uint64_t offset, const void *buffer,
                      size_t size);

#endif
