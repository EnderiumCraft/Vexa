/* A new ext3 file system, as `mke2fs -t ext3 -b 4096 -I 128` would make it:
 * 4 KiB blocks in groups of 32768, an inode for every 64 KiB, superblock
 * copies in groups 0, 1 and the powers of 3, 5 and 7 ("sparse super"), an
 * empty root directory with lost+found, and a journal (inode 8: 16 MiB, or
 * 32 MiB from 1 GiB up), which makes it crash-proof. Only the features
 * Vexa's ext2 driver knows (typed directory entries, sparse superblocks,
 * large files, the journal). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vexa/syscall.h>
#include "install.h"

#define BLOCK 4096
#define BLOCKS_PER_GROUP 32768
#define INODE_SIZE 128
#define BYTES_PER_INODE 65536
#define FIRST_INODE 11 /* Below it, reserved: 2 is the root directory, 8 the journal. */
#define JOURNAL_INODE 8

static int fd;
static uint8_t block[BLOCK];

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void write_block(uint64_t number, const void *data) {
    if (pwrite(fd, data, BLOCK, (off_t)(number * BLOCK)) != BLOCK) {
        fail("writing the file system failed (block %llu)", (unsigned long long)number);
    }
}

static bool has_super(uint32_t group) {
    if (group <= 1) {
        return true;
    }
    for (uint32_t base = 3; base <= 7; base += 2) {
        uint64_t power = base;
        while (power < group) {
            power *= base;
        }
        if (power == group) {
            return true;
        }
    }
    return false;
}

static void set_bits(uint8_t *bitmap, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) {
        bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
    }
}

static void put_inode_dir(uint8_t *p, uint16_t mode, uint16_t links, uint32_t data_block,
                          uint32_t now) {
    memset(p, 0, INODE_SIZE);
    put16(p + 0, mode);
    put32(p + 4, BLOCK);       /* Size. */
    put32(p + 8, now);         /* atime */
    put32(p + 12, now);        /* ctime */
    put32(p + 16, now);        /* mtime */
    put16(p + 26, links);
    put32(p + 28, BLOCK / 512); /* Blocks (in 512-byte units). */
    put32(p + 40, data_block);
}

static void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The journal: its blocks (zeroed, the first its superblock), and the block
 * map pointing at them (indirect blocks after them). Fills in its inode. */
static void make_journal(uint32_t first, uint32_t count, uint32_t maps, const uint8_t uuid[16],
                         uint8_t *inode, uint32_t now) {
    memset(block, 0, BLOCK);
    for (uint32_t b = 1; b < count; b++) {
        write_block(first + b, block);
    }
    put32be(block + 0, 0xc03b3998); /* JBD2's magic. */
    put32be(block + 4, 4);          /* Superblock, version 2. */
    put32be(block + 12, BLOCK);
    put32be(block + 16, count);
    put32be(block + 20, 1);         /* The log starts after it. */
    put32be(block + 24, 1);         /* Its first transaction's number. */
    memcpy(block + 48, uuid, 16);
    put32be(block + 64, 1);         /* One file system uses it... */
    memcpy(block + 0x100, uuid, 16); /* ...this one. */
    write_block(first, block);

    memset(inode, 0, INODE_SIZE);
    put16(inode + 0, 0100600);
    put32(inode + 4, count * BLOCK);
    put32(inode + 8, now);
    put32(inode + 12, now);
    put32(inode + 16, now);
    put16(inode + 26, 1);
    put32(inode + 28, (count + maps) * (BLOCK / 512));
    uint32_t per = BLOCK / 4, next_map = first + count;
    for (uint32_t i = 0; i < 12 && i < count; i++) {
        put32(inode + 40 + 4 * i, first + i);
    }
    uint32_t done = 12;
    if (done < count) { /* Single indirect. */
        uint32_t single = next_map++;
        put32(inode + 40 + 48, single);
        memset(block, 0, BLOCK);
        for (uint32_t i = 0; i < per && done < count; i++, done++) {
            put32(block + 4 * i, first + done);
        }
        write_block(single, block);
    }
    if (done < count) { /* Double indirect. */
        uint32_t dbl = next_map++;
        put32(inode + 40 + 52, dbl);
        uint8_t *top = calloc(1, BLOCK);
        if (!top) {
            fail("out of memory");
        }
        for (uint32_t j = 0; j < per && done < count; j++) {
            uint32_t ind = next_map++;
            put32(top + 4 * j, ind);
            memset(block, 0, BLOCK);
            for (uint32_t i = 0; i < per && done < count; i++, done++) {
                put32(block + 4 * i, first + done);
            }
            write_block(ind, block);
        }
        write_block(dbl, top);
        free(top);
    }
}

/* How many indirect blocks a file of `count` blocks needs (up to double). */
static uint32_t map_blocks(uint32_t count) {
    uint32_t per = BLOCK / 4;
    if (count <= 12) {
        return 0;
    }
    if (count <= 12 + per) {
        return 1;
    }
    return 2 + (count - 12 - per + per - 1) / per;
}

static int put_entry(uint8_t *p, uint32_t inode, uint16_t rec_len, const char *name) {
    size_t n = strlen(name);
    put32(p, inode);
    put16(p + 4, rec_len);
    p[6] = (uint8_t)n;
    p[7] = 2; /* A directory. */
    memcpy(p + 8, name, n);
    return rec_len;
}

void make_ext2(int partition, uint64_t bytes, const char *label, uint8_t uuid[16]) {
    fd = partition;
    uint64_t total = bytes / BLOCK;
    if (total > 0xffffffffULL) {
        total = 0xffffffffULL; /* (16 TiB: as far as these block numbers go.) */
    }
    uint32_t blocks = (uint32_t)total;
    uint32_t inodes_per_group = (uint32_t)((uint64_t)BLOCKS_PER_GROUP * BLOCK / BYTES_PER_INODE);
    uint32_t itable_blocks = inodes_per_group * INODE_SIZE / BLOCK;
    uint32_t groups = (blocks + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP;
    uint32_t gdt_blocks = (groups * 32 + BLOCK - 1) / BLOCK;
    /* A last group too small for its own bookkeeping is left off. */
    uint32_t last = blocks - (groups - 1) * BLOCKS_PER_GROUP;
    uint32_t last_overhead = (has_super(groups - 1) ? 1 + gdt_blocks : 0) + 2 + itable_blocks;
    if (groups > 1 && last < last_overhead + 64) {
        groups--;
        blocks = groups * BLOCKS_PER_GROUP;
        gdt_blocks = (groups * 32 + BLOCK - 1) / BLOCK;
    }
    if (blocks < 1024) {
        fail("the partition is too small for a file system");
    }
    uint32_t now = (uint32_t)vx_time();
    random_bytes(uuid, 16);
    uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40); /* A version 4 UUID. */
    uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);

    /* Where everything goes, group by group. */
    uint8_t *gdt = calloc(gdt_blocks, BLOCK);
    if (!gdt) {
        fail("out of memory");
    }
    uint32_t journal_blocks = blocks >= 262144 ? 8192 : 4096;
    uint32_t journal_maps = map_blocks(journal_blocks);
    uint32_t free_blocks = 0, root_block = 0, lost_block = 0, journal_first = 0;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t start = g * BLOCKS_PER_GROUP;
        uint32_t size = g == groups - 1 ? blocks - start : BLOCKS_PER_GROUP;
        uint32_t meta = has_super(g) ? 1 + gdt_blocks : 0;
        uint32_t used = meta + 2 + itable_blocks;
        if (g == 0) {
            root_block = start + used;
            lost_block = root_block + 1;
            journal_first = lost_block + 1;
            used += 2 + journal_blocks + journal_maps;
            if (used + 64 > size) {
                fail("the partition is too small for a file system");
            }
        }
        uint8_t *d = gdt + g * 32;
        put32(d + 0, start + meta);           /* Block bitmap. */
        put32(d + 4, start + meta + 1);       /* Inode bitmap. */
        put32(d + 8, start + meta + 2);       /* Inode table. */
        put16(d + 12, (uint16_t)(size - used));
        put16(d + 14, (uint16_t)(g == 0 ? inodes_per_group - FIRST_INODE : inodes_per_group));
        put16(d + 16, (uint16_t)(g == 0 ? 2 : 0)); /* Directories. */
        free_blocks += size - used;

        /* The bitmaps (what's past the end of the group counts as used). */
        memset(block, 0, BLOCK);
        set_bits(block, 0, used);
        set_bits(block, size, BLOCK * 8);
        write_block(start + meta, block);
        memset(block, 0, BLOCK);
        if (g == 0) {
            set_bits(block, 0, FIRST_INODE);
        }
        set_bits(block, inodes_per_group, BLOCK * 8);
        write_block(start + meta + 1, block);
        /* An empty inode table. */
        memset(block, 0, BLOCK);
        for (uint32_t b = 0; b < itable_blocks; b++) {
            write_block(start + meta + 2 + b, block);
        }
        progress((int)((uint64_t)(g + 1) * 100 / groups));
    }

    /* The journal (inode 8), the root directory (2) and lost+found (11). */
    uint32_t itable0 = (has_super(0) ? 1 + gdt_blocks : 0) + 2;
    uint8_t journal_inode[INODE_SIZE];
    make_journal(journal_first, journal_blocks, journal_maps, uuid, journal_inode, now);
    memset(block, 0, BLOCK);
    put_inode_dir(block + (2 - 1) * INODE_SIZE, 040755, 3, root_block, now);
    put_inode_dir(block + (FIRST_INODE - 1) * INODE_SIZE, 040700, 2, lost_block, now);
    memcpy(block + (JOURNAL_INODE - 1) * INODE_SIZE, journal_inode, INODE_SIZE);
    write_block(itable0, block);
    memset(block, 0, BLOCK);
    int at = put_entry(block, 2, 12, ".");
    at += put_entry(block + at, 2, 12, "..");
    put_entry(block + at, FIRST_INODE, (uint16_t)(BLOCK - at), "lost+found");
    write_block(root_block, block);
    memset(block, 0, BLOCK);
    at = put_entry(block, FIRST_INODE, 12, ".");
    put_entry(block + at, 2, (uint16_t)(BLOCK - at), "..");
    write_block(lost_block, block);

    /* The superblock, and its copies with their group's number. */
    uint8_t super[1024];
    memset(super, 0, sizeof(super));
    put32(super + 0, inodes_per_group * groups);
    put32(super + 4, blocks);
    put32(super + 8, blocks / 20); /* 5% for the superuser. */
    put32(super + 12, free_blocks);
    put32(super + 16, inodes_per_group * groups - FIRST_INODE);
    put32(super + 20, 0);       /* First data block (0 with 4 KiB blocks). */
    put32(super + 24, 2);       /* 1024 << 2 = 4096 */
    put32(super + 28, 2);
    put32(super + 32, BLOCKS_PER_GROUP);
    put32(super + 36, BLOCKS_PER_GROUP);
    put32(super + 40, inodes_per_group);
    put32(super + 48, now);     /* Written. */
    put16(super + 54, 0xffff);  /* No forced checks. */
    put16(super + 56, 0xef53);
    put16(super + 58, 1);       /* Clean. */
    put16(super + 60, 1);       /* On errors: carry on. */
    put32(super + 64, now);     /* Last checked. */
    put32(super + 76, 1);       /* Revision 1 ("dynamic"). */
    put32(super + 84, FIRST_INODE);
    put16(super + 88, INODE_SIZE);
    put32(super + 92, 0x0004);  /* A journal. */
    put32(super + 96, 0x0002);  /* Typed directory entries. */
    put32(super + 100, 0x0003); /* Sparse superblocks, large files. */
    memcpy(super + 104, uuid, 16);
    strncpy((char *)super + 120, label, 16);
    put32(super + 224, JOURNAL_INODE);
    super[253] = 1; /* A copy of the journal inode's block map, for e2fsck: */
    memcpy(super + 268, journal_inode + 40, 15 * 4);
    put32(super + 268 + 15 * 4, 0);                    /* (Size, high.) */
    put32(super + 268 + 16 * 4, journal_blocks * BLOCK); /* (Size.) */
    for (uint32_t g = 0; g < groups; g++) {
        if (!has_super(g)) {
            continue;
        }
        put16(super + 90, (uint16_t)g);
        uint32_t start = g * BLOCKS_PER_GROUP;
        memset(block, 0, BLOCK);
        memcpy(block + (g == 0 ? 1024 : 0), super, sizeof(super));
        write_block(start, block);
        for (uint32_t b = 0; b < gdt_blocks; b++) {
            write_block(start + 1 + b, gdt + (size_t)b * BLOCK);
        }
    }
    free(gdt);
    fsync(fd);
}
