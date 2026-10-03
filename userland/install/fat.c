/* A FAT32 file system with a few files in it: the EFI system partition, where
 * UEFI firmware finds Limine (EFI/BOOT/BOOTX64.EFI) and Limine finds its
 * configuration and the kernel. Files and directories are laid out one after
 * another; names that aren't plain 8.3 capitals get long-name entries. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "install.h"

#define MAX_NODES 64
#define MAX_CHILDREN 16

struct node {
    char name[64];
    bool dir;
    const struct fat_file *file;
    struct node *parent;
    struct node *children[MAX_CHILDREN];
    int child_count;
    uint32_t cluster, clusters; /* Where it is, and how many. */
    char short_name[11];
};

static struct node nodes[MAX_NODES];
static int node_count;
static int fd;
static uint32_t sector_size, cluster_bytes, data_start, *fat;
static uint16_t fat_date, fat_time;

static struct node *child(struct node *dir, const char *name, size_t length, bool is_dir) {
    for (int i = 0; i < dir->child_count; i++) {
        if (strlen(dir->children[i]->name) == length &&
            !strncmp(dir->children[i]->name, name, length)) {
            return dir->children[i];
        }
    }
    if (node_count == MAX_NODES || dir->child_count == MAX_CHILDREN) {
        fail("too many files for the boot partition");
    }
    struct node *n = &nodes[node_count++];
    memset(n, 0, sizeof(*n));
    memcpy(n->name, name, length < 63 ? length : 63);
    n->dir = is_dir;
    n->parent = dir;
    dir->children[dir->child_count++] = n;
    return n;
}

static bool plain_83(const char *name) {
    const char *dot = strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot && strchr(dot + 1, '.'))) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (*p != '.' && !((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
                           *p == '-')) {
            return false;
        }
    }
    return true;
}

static char upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static void make_short_name(struct node *n, int number) {
    memset(n->short_name, ' ', 11);
    const char *dot = strrchr(n->name, '.');
    if (plain_83(n->name)) {
        for (int i = 0; n->name[i] && n->name + i != dot; i++) {
            n->short_name[i] = n->name[i];
        }
        for (int i = 0; dot && dot[1 + i]; i++) {
            n->short_name[8 + i] = dot[1 + i];
        }
        return;
    }
    int j = 0;
    for (const char *p = n->name; *p && p != dot && j < 6; p++) {
        char c = upper(*p);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            n->short_name[j++] = c;
        }
    }
    n->short_name[j++] = '~';
    n->short_name[j] = (char)('0' + number);
    for (int i = 0; dot && dot[1 + i] && i < 3; i++) {
        n->short_name[8 + i] = upper(dot[1 + i]);
    }
}

static uint8_t checksum(const char short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)short_name[i]);
    }
    return sum;
}

/* Directory entries a node takes in its parent: its long name's, then its own. */
static int entries_for(const struct node *n) {
    return plain_83(n->name) ? 1 : 1 + (int)(strlen(n->name) + 12) / 13;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put_short(uint8_t *e, const char name[11], uint8_t attributes, uint32_t cluster,
                      uint32_t size) {
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = attributes;
    put16(e + 14, fat_time);
    put16(e + 16, fat_date);
    put16(e + 18, fat_date);
    put16(e + 20, (uint16_t)(cluster >> 16));
    put16(e + 22, fat_time);
    put16(e + 24, fat_date);
    put16(e + 26, (uint16_t)cluster);
    put32(e + 28, size);
}

static uint8_t *put_node(uint8_t *e, const struct node *n) {
    if (!plain_83(n->name)) {
        int pieces = entries_for(n) - 1;
        size_t length = strlen(n->name);
        uint8_t sum = checksum(n->short_name);
        for (int piece = pieces; piece >= 1; piece--, e += 32) {
            memset(e, 0, 32);
            e[0] = (uint8_t)(piece | (piece == pieces ? 0x40 : 0));
            e[11] = 0x0f;
            e[13] = sum;
            static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; k++) {
                size_t at = (size_t)(piece - 1) * 13 + (size_t)k;
                uint16_t c = at < length ? (uint8_t)n->name[at] : at == length ? 0 : 0xffff;
                put16(e + offsets[k], c);
            }
        }
    }
    put_short(e, n->short_name, n->dir ? 0x10 : 0x20, n->cluster,
              n->dir ? 0 : n->file->size);
    return e + 32;
}

static void write_cluster(uint32_t cluster, const void *data, uint32_t length) {
    static uint8_t buffer[65536];
    memset(buffer, 0, cluster_bytes);
    memcpy(buffer, data, length);
    off_t at = (off_t)(data_start + (uint64_t)(cluster - 2) * (cluster_bytes / sector_size)) *
               sector_size;
    if (pwrite(fd, buffer, cluster_bytes, at) != (ssize_t)cluster_bytes) {
        fail("writing the boot partition failed");
    }
}

static void write_run(uint32_t first, const void *data, uint32_t length) {
    off_t at = (off_t)(data_start + (uint64_t)(first - 2) * (cluster_bytes / sector_size)) *
               sector_size;
    if (length && pwrite(fd, data, length, at) != (ssize_t)length) {
        fail("writing the boot partition failed");
    }
}

static uint32_t next_cluster = 2;

static void allocate(struct node *n) {
    uint32_t bytes;
    if (n->dir) {
        int entries = n->parent ? 2 : 1; /* ".", ".." (the root: its label). */
        for (int i = 0; i < n->child_count; i++) {
            entries += entries_for(n->children[i]);
        }
        bytes = (uint32_t)entries * 32;
    } else {
        bytes = n->file->size;
    }
    n->clusters = bytes ? (bytes + cluster_bytes - 1) / cluster_bytes : (n->dir ? 1 : 0);
    n->cluster = n->clusters ? next_cluster : 0;
    for (uint32_t i = 0; i < n->clusters; i++) {
        fat[next_cluster + i] = i + 1 < n->clusters ? next_cluster + i + 1 : 0x0fffffff;
    }
    next_cluster += n->clusters;
    for (int i = 0; i < n->child_count; i++) {
        int number = 1;
        struct node *c = n->children[i];
        make_short_name(c, number);
        for (int j = 0; j < i; j++) { /* (Two long names, one short form: ~2, ~3...) */
            if (!memcmp(n->children[j]->short_name, c->short_name, 11)) {
                make_short_name(c, ++number);
                j = -1;
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        allocate(n->children[i]);
    }
}

static void write_node(struct node *n, const char *label) {
    if (!n->dir) {
        write_run(n->cluster, n->file->data, n->file->size);
        return;
    }
    uint8_t *entries = calloc(n->clusters, cluster_bytes);
    if (!entries) {
        fail("out of memory");
    }
    uint8_t *e = entries;
    if (n->parent) {
        char dot[11], dotdot[11];
        memset(dot, ' ', 11);
        memset(dotdot, ' ', 11);
        dot[0] = '.';
        dotdot[0] = dotdot[1] = '.';
        put_short(e, dot, 0x10, n->cluster, 0);
        put_short(e + 32, dotdot, 0x10, n->parent->parent ? n->parent->cluster : 0, 0);
        e += 64;
    } else {
        char name[11];
        memset(name, ' ', 11);
        memcpy(name, label, strlen(label) < 11 ? strlen(label) : 11);
        put_short(e, name, 0x08, 0, 0); /* The volume label. */
        e += 32;
    }
    for (int i = 0; i < n->child_count; i++) {
        e = put_node(e, n->children[i]);
    }
    for (uint32_t c = 0; c < n->clusters; c++) {
        write_cluster(n->cluster + c, entries + (size_t)c * cluster_bytes, cluster_bytes);
    }
    free(entries);
    for (int i = 0; i < n->child_count; i++) {
        write_node(n->children[i], label);
    }
}

void make_fat32(int partition, uint64_t bytes, uint32_t hidden, const char *label,
                const struct fat_file *files, int count) {
    fd = partition;
    sector_size = 512;
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    fat_date = (uint16_t)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
    fat_time = (uint16_t)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);

    uint32_t total = (uint32_t)(bytes / sector_size);
    uint32_t per_cluster = 1;
    while ((uint64_t)total / per_cluster > 1000000) {
        per_cluster *= 2; /* (Big partitions: bigger clusters.) */
    }
    cluster_bytes = per_cluster * sector_size;
    uint32_t reserved = 32;
    uint32_t fat_sectors = (uint32_t)(((uint64_t)(total - reserved) / per_cluster + 2) * 4 +
                                      sector_size - 1) / sector_size;
    data_start = reserved + 2 * fat_sectors;
    uint32_t clusters = (total - data_start) / per_cluster;
    if (clusters < 65525) {
        fail("the boot partition is too small for FAT32");
    }
    fat = calloc(clusters + 2, 4);
    if (!fat) {
        fail("out of memory");
    }
    fat[0] = 0x0ffffff8;
    fat[1] = 0x0fffffff;

    /* The tree of files. */
    node_count = 0;
    struct node *root = &nodes[node_count++];
    memset(root, 0, sizeof(*root));
    root->dir = true;
    for (int i = 0; i < count; i++) {
        struct node *dir = root;
        const char *p = files[i].path + 1;
        const char *slash;
        while ((slash = strchr(p, '/'))) {
            dir = child(dir, p, (size_t)(slash - p), true);
            p = slash + 1;
        }
        child(dir, p, strlen(p), false)->file = &files[i];
    }
    next_cluster = 2;
    allocate(root);
    if (next_cluster > clusters + 2) {
        fail("the files don't fit on the boot partition");
    }
    write_node(root, label);

    /* The boot sector (and its copy), the FS information sector, the FATs. */
    uint8_t sector[512];
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xeb;
    sector[1] = 0x58;
    sector[2] = 0x90;
    memcpy(sector + 3, "VEXA    ", 8);
    put16(sector + 11, (uint16_t)sector_size);
    sector[13] = (uint8_t)per_cluster;
    put16(sector + 14, (uint16_t)reserved);
    sector[16] = 2;           /* FATs */
    sector[21] = 0xf8;        /* A fixed disk. */
    put16(sector + 24, 63);   /* Sectors per track, heads (for old firmware). */
    put16(sector + 26, 255);
    put32(sector + 28, hidden);
    put32(sector + 32, total);
    put32(sector + 36, fat_sectors);
    put32(sector + 44, root->cluster);
    put16(sector + 48, 1);    /* FS information sector. */
    put16(sector + 50, 6);    /* Backup boot sector. */
    sector[64] = 0x80;
    sector[66] = 0x29;
    uint32_t serial;
    random_bytes(&serial, 4);
    put32(sector + 67, serial);
    memset(sector + 71, ' ', 11);
    memcpy(sector + 71, label, strlen(label) < 11 ? strlen(label) : 11);
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    uint8_t info[512];
    memset(info, 0, sizeof(info));
    put32(info + 0, 0x41615252);
    put32(info + 484, 0x61417272);
    put32(info + 488, clusters - (next_cluster - 2));
    put32(info + 492, next_cluster);
    put32(info + 508, 0xaa550000);
    for (uint32_t copy = 0; copy <= 6; copy += 6) {
        if (pwrite(fd, sector, 512, (off_t)copy * 512) != 512 ||
            pwrite(fd, info, 512, (off_t)(copy + 1) * 512) != 512) {
            fail("writing the boot partition failed");
        }
    }
    for (int f = 0; f < 2; f++) {
        off_t at = (off_t)(reserved + (uint32_t)f * fat_sectors) * sector_size;
        size_t size = (size_t)fat_sectors * sector_size;
        uint8_t *table = calloc(1, size);
        if (!table) {
            fail("out of memory");
        }
        memcpy(table, fat, (size_t)(clusters + 2) * 4 < size ? (size_t)(clusters + 2) * 4 : size);
        if (pwrite(fd, table, size, at) != (ssize_t)size) {
            fail("writing the boot partition failed");
        }
        free(table);
    }
    free(fat);
    fsync(fd);
}
