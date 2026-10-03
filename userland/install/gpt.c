/* A GUID partition table: a protective MBR, the header and 128 entries at
 * the start, and their copies at the end. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "install.h"

struct __attribute__((packed)) gpt_header {
    char signature[8];
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t current_lba, backup_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
};

struct __attribute__((packed)) gpt_entry {
    uint8_t type[16], unique[16];
    uint64_t first, last, attributes;
    uint16_t name[36];
};

/* Type GUIDs, as they're stored (the first three parts little-endian). */
static const uint8_t esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t linux_type[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                       0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

#define ENTRIES 128

static void write_at(int disk, uint64_t offset, const void *data, size_t size) {
    if (pwrite(disk, data, size, (off_t)offset) != (ssize_t)size) {
        fail("writing the partition table failed");
    }
}

static void set_name(struct gpt_entry *e, const char *name) {
    for (int i = 0; name[i] && i < 36; i++) {
        e->name[i] = (uint16_t)name[i];
    }
}

void write_gpt(int disk, struct layout *l, uint64_t esp_bytes) {
    uint32_t ss = l->sector_size;
    uint64_t entry_sectors = (ENTRIES * sizeof(struct gpt_entry) + ss - 1) / ss;
    uint64_t align = 1024 * 1024 / ss; /* Partitions start on 1 MiB boundaries. */
    uint64_t last_usable = l->sectors - 2 - entry_sectors;
    l->esp_first = align;
    l->esp_sectors = esp_bytes / ss;
    l->root_first = l->esp_first + l->esp_sectors;
    l->root_first = (l->root_first + align - 1) / align * align;
    if (l->root_first + align * 64 > last_usable) {
        fail("the disk is too small");
    }
    l->root_sectors = (last_usable + 1 - l->root_first) / (4096 / ss) * (4096 / ss);

    static struct gpt_entry entries[ENTRIES];
    memset(entries, 0, sizeof(entries));
    memcpy(entries[0].type, esp_type, 16);
    random_bytes(entries[0].unique, 16);
    entries[0].first = l->esp_first;
    entries[0].last = l->esp_first + l->esp_sectors - 1;
    set_name(&entries[0], "EFI system");
    memcpy(entries[1].type, linux_type, 16);
    random_bytes(entries[1].unique, 16);
    entries[1].first = l->root_first;
    entries[1].last = l->root_first + l->root_sectors - 1;
    set_name(&entries[1], "Vexa");

    struct gpt_header header;
    memset(&header, 0, sizeof(header));
    memcpy(header.signature, "EFI PART", 8);
    header.revision = 0x00010000;
    header.header_size = sizeof(header);
    header.first_usable = 2 + entry_sectors;
    header.last_usable = last_usable;
    random_bytes(header.disk_guid, 16);
    header.entry_count = ENTRIES;
    header.entry_size = sizeof(struct gpt_entry);
    header.entries_crc = crc32(entries, sizeof(entries));

    /* The protective MBR: one partition of type EE over the whole disk. */
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    uint8_t *p = mbr + 446;
    uint64_t count = l->sectors - 1 > 0xffffffffULL ? 0xffffffffULL : l->sectors - 1;
    p[1] = 0x00; p[2] = 0x02; p[3] = 0x00; /* CHS 0/0/2 */
    p[4] = 0xee;
    p[5] = 0xff; p[6] = 0xff; p[7] = 0xff;
    p[8] = 1;
    memcpy(p + 12, &count, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    static uint8_t zero[4096];
    write_at(disk, 0, zero, ss);
    write_at(disk, 0, mbr, sizeof(mbr));

    /* The copy at the end first, then the primary. */
    header.current_lba = l->sectors - 1;
    header.backup_lba = 1;
    header.entries_lba = l->sectors - 1 - entry_sectors;
    header.header_crc = 0;
    header.header_crc = crc32(&header, sizeof(header));
    write_at(disk, header.entries_lba * ss, entries, sizeof(entries));
    write_at(disk, (l->sectors - 1) * ss, zero, ss);
    write_at(disk, (l->sectors - 1) * ss, &header, sizeof(header));

    header.current_lba = 1;
    header.backup_lba = l->sectors - 1;
    header.entries_lba = 2;
    header.header_crc = 0;
    header.header_crc = crc32(&header, sizeof(header));
    write_at(disk, 2 * (uint64_t)ss, entries, sizeof(entries));
    write_at(disk, ss, zero, ss);
    write_at(disk, ss, &header, sizeof(header));
    /* (Old file systems' signatures at the start of each partition go.) */
    static uint8_t blank[65536];
    write_at(disk, l->esp_first * ss, blank, sizeof(blank));
    write_at(disk, l->root_first * ss, blank, sizeof(blank));
}
