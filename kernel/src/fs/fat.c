#include <vexa/block.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * FAT (12, 16 and 32, with long names) and exFAT, read and write: what USB
 * sticks, memory cards and the EFI system partition use.
 *
 * Both keep files as chains of clusters: the FAT (file allocation table)
 * says which cluster comes after which. exFAT adds a bitmap of the clusters
 * in use, and lets a file that's in one piece skip the FAT ("NoFatChain").
 * Directories are files of 32-byte entries. FAT gives a file an 8.3 short
 * entry, with its long name in entries before it; exFAT a set of entries: a
 * file entry, a stream entry (where it is, how long) and its name.
 *
 * Neither has "." and ".." that can be trusted (exFAT has none), so a node
 * holds a reference to its parent directory, which answers "..". A node is
 * known by its place: its parent, and the index of its first entry there.
 * Names are matched without regard to case, as Windows does. Times are
 * kept as the local time, which Vexa takes to be UTC. There are no owners,
 * links or symbolic links; the read-only attribute is the write permission.
 */

#define ATTR_READ_ONLY 0x01
#define ATTR_VOLUME 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LONG_NAME 0x0f

#define MAX_UNITS 255 /* UTF-16 units in a name. */

/* exFAT entry types. */
#define EX_BITMAP 0x81
#define EX_UPCASE 0x82
#define EX_FILE 0x85
#define EX_STREAM 0xc0
#define EX_NAME 0xc1
#define EX_IN_USE 0x80

enum kind { FAT12, FAT16, FAT32, EXFAT };

/* Where a file's (or directory's) data is. */
struct chain {
    uint32_t first;      /* Its first cluster; 0 if it has none. */
    bool contiguous;     /* exFAT's NoFatChain: first, first + 1, ... */
    bool fixed_root;     /* FAT12/16's root directory: a fixed area, no clusters. */
    bool counted;        /* `clusters` is known. */
    uint32_t clusters;
    uint32_t cache_index, cache_cluster; /* The last one found by walking (0: none). */
};

struct fat {
    struct block_device *device;
    struct mount *mount;
    enum kind kind;
    uint32_t cluster_size;
    uint64_t fat_start, fat_bytes; /* In bytes, from the start of the device. */
    uint32_t fat_count;
    uint64_t root_start;           /* FAT12/16: the root directory's area... */
    uint32_t root_entries;         /* ...and how many entries it holds. */
    uint64_t data_start;           /* Cluster 2. */
    uint32_t cluster_count;        /* Clusters 2 to cluster_count + 1. */
    uint32_t root_cluster;         /* FAT32 and exFAT. */
    uint32_t next_free;            /* Where looking for a free cluster starts. */
    int64_t free_clusters;         /* -1 until counted. */
    uint8_t *bitmap;               /* exFAT: the clusters in use, a bit each... */
    uint64_t bitmap_start;         /* ...and where it is on the disk. */
    uint16_t *upcase;              /* exFAT: upper case of each UTF-16 unit. */
    struct fat_node *open_nodes;
};

struct fat_node {
    struct vnode vnode;
    struct chain data;
    uint64_t valid;          /* exFAT: bytes written (the rest reads as zeros). */
    uint8_t attr;
    int64_t created;
    struct fat_node *parent; /* With a reference; NULL for the root. */
    uint32_t index, count;   /* Its entries in the parent: the first, how many. */
    bool removed;            /* Its name is gone: its clusters go when it does. */
    struct fat_node *next;
};

/* A directory entry (or entry set), read or to be written. */
struct info {
    uint16_t name[MAX_UNITS + 1];
    uint32_t units;
    uint8_t attr;
    struct chain data;
    uint64_t size, valid;
    int64_t modified, created;
    uint32_t index, count;
};

static const struct vnode_ops fat_ops;
static const uint8_t zeros[4096];

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v), put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32));
}

static struct fat *fs_of(struct vnode *vnode) {
    return vnode->data;
}

static struct fat_node *node_of(struct vnode *vnode) {
    return (struct fat_node *)vnode;
}

/* ---- Times (FAT's: 2-second steps from 1980, local time) ---- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static int64_t from_fat_time(uint16_t time, uint16_t date) {
    unsigned m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1) {
        return 0;
    }
    return days_from_civil(1980 + (date >> 9), m, d) * 86400 + (time >> 11) * 3600 +
           ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

/* Both in one: the date in the high 16 bits (exFAT's timestamps). */
static uint32_t to_fat_time(int64_t t) {
    if (t < 315532800) { /* 1980 */
        t = 315532800;
    }
    int64_t y;
    unsigned m, d;
    civil_from_days(t / 86400, &y, &m, &d);
    if (y > 2107) {
        y = 2107;
    }
    long s = (long)(t % 86400);
    uint32_t date = (uint32_t)(y - 1980) << 9 | m << 5 | d;
    uint32_t time = (uint32_t)(s / 3600) << 11 | (uint32_t)(s / 60 % 60) << 5 | (uint32_t)(s % 60 / 2);
    return date << 16 | time;
}

/* ---- Names: UTF-8 (Vexa's) and UTF-16 (FAT's) ---- */

/* Returns how many units, or -1 if it's too long or not UTF-8. */
static int utf8_to_16(const char *s, size_t length, uint16_t *out) {
    int units = 0;
    for (size_t i = 0; i < length;) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2
                                 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0 || i + (size_t)extra >= length + (extra ? 0 : 1)) {
            return -1;
        }
        cp = extra == 0 ? c : extra == 1 ? c & 0x1f : extra == 2 ? c & 0x0f : c & 0x07;
        for (int k = 1; k <= extra; k++) {
            if (((uint8_t)s[i + k] & 0xc0) != 0x80) {
                return -1;
            }
            cp = cp << 6 | ((uint8_t)s[i + k] & 0x3f);
        }
        i += (size_t)extra + 1;
        if (cp >= 0x10000) {
            if (units + 2 > MAX_UNITS) {
                return -1;
            }
            cp -= 0x10000;
            out[units++] = (uint16_t)(0xd800 | cp >> 10);
            out[units++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
        } else {
            if (units + 1 > MAX_UNITS) {
                return -1;
            }
            out[units++] = (uint16_t)cp;
        }
    }
    return units;
}

/* Into out (VX_NAME_MAX + 1 bytes); returns its length. */
static size_t utf16_to_8(const uint16_t *name, uint32_t units, char *out) {
    size_t n = 0;
    for (uint32_t i = 0; i < units; i++) {
        uint32_t cp = name[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < units && name[i + 1] >= 0xdc00 &&
            name[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (name[i + 1] - 0xdc00);
            i++;
        }
        char bytes[4];
        size_t k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (k == 1) {
            bytes[0] = (char)cp;
        } else if (k == 2) {
            bytes[0] = (char)(0xc0 | cp >> 6), bytes[1] = (char)(0x80 | (cp & 0x3f));
        } else if (k == 3) {
            bytes[0] = (char)(0xe0 | cp >> 12), bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
            bytes[2] = (char)(0x80 | (cp & 0x3f));
        } else {
            bytes[0] = (char)(0xf0 | cp >> 18), bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
            bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3f)), bytes[3] = (char)(0x80 | (cp & 0x3f));
        }
        if (n + k > VX_NAME_MAX) {
            break;
        }
        memcpy(out + n, bytes, k);
        n += k;
    }
    out[n] = '\0';
    return n;
}

static uint16_t upper(struct fat *fs, uint16_t c) {
    if (fs->upcase) {
        return fs->upcase[c];
    }
    return c >= 'a' && c <= 'z' ? (uint16_t)(c - 32) : c;
}

static bool same_name(struct fat *fs, const uint16_t *a, uint32_t a_units, const uint16_t *b,
                      uint32_t b_units) {
    if (a_units != b_units) {
        return false;
    }
    for (uint32_t i = 0; i < a_units; i++) {
        if (upper(fs, a[i]) != upper(fs, b[i])) {
            return false;
        }
    }
    return true;
}

/* ---- The allocation table ---- */

static bool valid_cluster(struct fat *fs, uint32_t c) {
    return c >= 2 && c < fs->cluster_count + 2;
}

static uint32_t end_mark(struct fat *fs) {
    return fs->kind == FAT12 ? 0xfff : fs->kind == FAT16 ? 0xffff
           : fs->kind == FAT32 ? 0x0fffffff : 0xffffffff;
}

static uint64_t cluster_offset(struct fat *fs, uint32_t c) {
    return fs->data_start + (uint64_t)(c - 2) * fs->cluster_size;
}

static int fat_get(struct fat *fs, uint32_t c, uint32_t *out) {
    uint8_t b[4] = {0};
    int error;
    switch (fs->kind) {
    case FAT12:
        error = block_read_bytes(fs->device, fs->fat_start + c + c / 2, b, 2);
        *out = c & 1 ? (uint32_t)le16(b) >> 4 : le16(b) & 0xfffu;
        return error;
    case FAT16:
        error = block_read_bytes(fs->device, fs->fat_start + (uint64_t)c * 2, b, 2);
        *out = le16(b);
        return error;
    default:
        error = block_read_bytes(fs->device, fs->fat_start + (uint64_t)c * 4, b, 4);
        *out = fs->kind == FAT32 ? le32(b) & 0x0fffffff : le32(b);
        return error;
    }
}

static int fat_set(struct fat *fs, uint32_t c, uint32_t value) {
    /* Every copy of the FAT (exFAT: the first, the one in use). */
    uint32_t copies = fs->kind == EXFAT ? 1 : fs->fat_count;
    for (uint32_t i = 0; i < copies; i++) {
        uint64_t base = fs->fat_start + i * fs->fat_bytes;
        uint8_t b[4];
        int error;
        if (fs->kind == FAT12) {
            uint64_t at = base + c + c / 2;
            if ((error = block_read_bytes(fs->device, at, b, 2)) != 0) {
                return error;
            }
            uint16_t v = le16(b);
            v = c & 1 ? (uint16_t)((v & 0x000f) | value << 4) : (uint16_t)((v & 0xf000) | (value & 0xfff));
            put16(b, v);
            error = block_write_bytes(fs->device, at, b, 2);
        } else if (fs->kind == FAT16) {
            put16(b, value);
            error = block_write_bytes(fs->device, base + (uint64_t)c * 2, b, 2);
        } else {
            uint64_t at = base + (uint64_t)c * 4;
            if (fs->kind == FAT32) { /* (The top four bits are reserved: kept.) */
                if ((error = block_read_bytes(fs->device, at, b, 4)) != 0) {
                    return error;
                }
                value = (le32(b) & 0xf0000000) | (value & 0x0fffffff);
            }
            put32(b, value);
            error = block_write_bytes(fs->device, at, b, 4);
        }
        if (error) {
            return error;
        }
    }
    return 0;
}

static bool chain_end(struct fat *fs, uint32_t value) {
    return !valid_cluster(fs, value); /* (The end mark, or a bad or free one.) */
}

static bool cluster_free(struct fat *fs, uint32_t c) {
    if (fs->kind == EXFAT) {
        return !(fs->bitmap[(c - 2) / 8] >> ((c - 2) % 8) & 1);
    }
    uint32_t v;
    return fat_get(fs, c, &v) == 0 && v == 0;
}

static int set_bitmap(struct fat *fs, uint32_t c, bool used) {
    uint32_t byte = (c - 2) / 8;
    uint8_t bit = (uint8_t)(1u << ((c - 2) % 8));
    fs->bitmap[byte] = used ? fs->bitmap[byte] | bit : fs->bitmap[byte] & ~bit;
    return block_write_bytes(fs->device, fs->bitmap_start + byte, &fs->bitmap[byte], 1);
}

/* Marks a cluster used: the end of a chain (in the FAT, unless it's part of
 * an exFAT file in one piece). */
static int claim(struct fat *fs, uint32_t c, bool in_fat) {
    int error = fs->kind == EXFAT ? set_bitmap(fs, c, true) : 0;
    if (!error && (in_fat || fs->kind != EXFAT)) {
        error = fat_set(fs, c, end_mark(fs));
    }
    if (!error && fs->free_clusters > 0) {
        fs->free_clusters--;
    }
    return error;
}

static int release_cluster(struct fat *fs, uint32_t c) {
    int error = fs->kind == EXFAT ? set_bitmap(fs, c, false) : fat_set(fs, c, 0);
    if (!error && fs->free_clusters >= 0) {
        fs->free_clusters++;
    }
    return error;
}

static int find_free(struct fat *fs, uint32_t near, uint32_t *out) {
    uint32_t start = valid_cluster(fs, near + 1) ? near + 1 : fs->next_free;
    if (!valid_cluster(fs, start)) {
        start = 2;
    }
    for (uint32_t i = 0; i < fs->cluster_count; i++) {
        uint32_t c = 2 + (start - 2 + i) % fs->cluster_count;
        if (cluster_free(fs, c)) {
            fs->next_free = c + 1;
            *out = c;
            return 0;
        }
    }
    return -VX_ENOSPC;
}

static int zero_bytes(struct fat *fs, uint64_t at, uint64_t size) {
    while (size) {
        size_t n = size < sizeof(zeros) ? (size_t)size : sizeof(zeros);
        int error = block_write_bytes(fs->device, at, zeros, n);
        if (error) {
            return error;
        }
        at += n, size -= n;
    }
    return 0;
}

static void count_free(struct fat *fs) {
    if (fs->free_clusters >= 0) {
        return;
    }
    int64_t n = 0;
    if (fs->kind == EXFAT) {
        for (uint32_t c = 0; c < fs->cluster_count; c++) {
            n += !(fs->bitmap[c / 8] >> (c % 8) & 1);
        }
    } else {
        /* The FAT, in pieces (FAT12's 1.5-byte entries: one at a time). */
        uint8_t *buffer = kmalloc(4096);
        uint32_t per = fs->kind == FAT16 ? 2 : 4;
        for (uint32_t c = 2; c < fs->cluster_count + 2;) {
            if (fs->kind == FAT12 || !buffer) {
                n += cluster_free(fs, c);
                c++;
                continue;
            }
            uint32_t count = 4096 / per;
            if (count > fs->cluster_count + 2 - c) {
                count = fs->cluster_count + 2 - c;
            }
            if (block_read_bytes(fs->device, fs->fat_start + (uint64_t)c * per, buffer,
                                 count * per)) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v = per == 2 ? le16(buffer + i * 2) : le32(buffer + i * 4) & 0x0fffffff;
                n += v == 0;
            }
            c += count;
        }
        kfree(buffer);
    }
    fs->free_clusters = n;
}

/* ---- Chains ---- */

static int chain_count(struct fat *fs, struct chain *ch) {
    if (ch->counted || ch->fixed_root) {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t c = ch->first; valid_cluster(fs, c) && n <= fs->cluster_count; n++) {
        int error = fat_get(fs, c, &c);
        if (error) {
            return error;
        }
    }
    ch->clusters = n;
    ch->counted = true;
    return 0;
}

/* The chain's cluster number n (from 0): -VX_ENOENT past its end. */
static int chain_at(struct fat *fs, struct chain *ch, uint32_t n, uint32_t *out) {
    if (!valid_cluster(fs, ch->first)) {
        return -VX_ENOENT;
    }
    if (ch->contiguous) {
        if (n >= ch->clusters) {
            return -VX_ENOENT;
        }
        *out = ch->first + n;
        return 0;
    }
    uint32_t i = 0, c = ch->first;
    if (ch->cache_cluster && ch->cache_index <= n) {
        i = ch->cache_index, c = ch->cache_cluster;
    }
    while (i < n) {
        uint32_t next;
        int error = fat_get(fs, c, &next);
        if (error) {
            return error;
        }
        if (chain_end(fs, next)) {
            return -VX_ENOENT;
        }
        c = next, i++;
    }
    ch->cache_index = n, ch->cache_cluster = c;
    *out = c;
    return 0;
}

/* Makes the chain `want` clusters long (new ones zeroed if `zero`). */
static int chain_grow(struct fat *fs, struct chain *ch, uint32_t want, bool zero) {
    int error = chain_count(fs, ch);
    if (error) {
        return error;
    }
    while (ch->clusters < want) {
        uint32_t n = ch->clusters, last = 0, c;
        if (n && (error = chain_at(fs, ch, n - 1, &last)) != 0) {
            return error;
        }
        if (ch->contiguous) {
            /* In one piece still, if the next cluster is free; if not, it
             * gets a chain in the FAT from now on. */
            c = ch->first + n;
            if (valid_cluster(fs, c) && cluster_free(fs, c)) {
                if ((error = claim(fs, c, false)) != 0) {
                    return error;
                }
                goto claimed;
            }
            for (uint32_t i = 0; i < n; i++) {
                if ((error = fat_set(fs, ch->first + i, i + 1 < n ? ch->first + i + 1
                                                                  : end_mark(fs))) != 0) {
                    return error;
                }
            }
            ch->contiguous = false;
        }
        if ((error = find_free(fs, last, &c)) != 0 || (error = claim(fs, c, true)) != 0) {
            return error;
        }
        if (n == 0) {
            ch->first = c;
        } else if ((error = fat_set(fs, last, c)) != 0) {
            return error;
        }
    claimed:
        if (zero && (error = zero_bytes(fs, cluster_offset(fs, c), fs->cluster_size)) != 0) {
            return error;
        }
        ch->clusters = n + 1;
        ch->cache_index = n, ch->cache_cluster = c;
    }
    return 0;
}

/* Keeps the chain's first `keep` clusters, and frees the rest. */
static int chain_shrink(struct fat *fs, struct chain *ch, uint32_t keep) {
    int error = chain_count(fs, ch);
    if (error || keep >= ch->clusters) {
        return error;
    }
    uint32_t c;
    if (ch->contiguous) {
        for (uint32_t i = keep; i < ch->clusters; i++) {
            if ((error = release_cluster(fs, ch->first + i)) != 0) {
                return error;
            }
        }
    } else {
        if (keep == 0) {
            c = ch->first;
        } else {
            uint32_t last;
            if ((error = chain_at(fs, ch, keep - 1, &last)) != 0 ||
                (error = fat_get(fs, last, &c)) != 0 ||
                (error = fat_set(fs, last, end_mark(fs))) != 0) {
                return error;
            }
        }
        for (uint32_t i = 0; valid_cluster(fs, c) && i <= fs->cluster_count; i++) {
            uint32_t next;
            if ((error = fat_get(fs, c, &next)) != 0 || (error = release_cluster(fs, c)) != 0) {
                return error;
            }
            c = next;
        }
    }
    ch->clusters = keep;
    ch->cache_index = ch->cache_cluster = 0;
    if (keep == 0) {
        ch->first = 0;
        ch->contiguous = false;
    }
    return 0;
}

/* Reads or writes bytes of a chain (which must be long enough). Returns
 * how many, or an error; short at the chain's end. */
static int64_t chain_io(struct fat *fs, struct chain *ch, uint64_t offset, void *buffer,
                        size_t size, bool write) {
    if (ch->fixed_root) {
        uint64_t length = (uint64_t)fs->root_entries * 32;
        if (offset >= length) {
            return 0;
        }
        size = size > length - offset ? (size_t)(length - offset) : size;
        int error = write ? block_write_bytes(fs->device, fs->root_start + offset, buffer, size)
                          : block_read_bytes(fs->device, fs->root_start + offset, buffer, size);
        return error ? error : (int64_t)size;
    }
    size_t done = 0;
    while (done < size) {
        uint32_t c, within = (uint32_t)(offset % fs->cluster_size);
        int error = chain_at(fs, ch, (uint32_t)(offset / fs->cluster_size), &c);
        if (error == -VX_ENOENT) {
            break;
        }
        if (error) {
            return error;
        }
        size_t n = fs->cluster_size - within;
        n = n > size - done ? size - done : n;
        uint64_t at = cluster_offset(fs, c) + within;
        uint8_t *p = (uint8_t *)buffer + done;
        error = write ? block_write_bytes(fs->device, at, p, n) : block_read_bytes(fs->device, at, p, n);
        if (error) {
            return error;
        }
        done += n, offset += n;
    }
    return (int64_t)done;
}

/* ---- Directory entries ---- */

static int read_entry(struct fat *fs, struct fat_node *dir, uint32_t index, uint8_t e[32]) {
    int64_t n = chain_io(fs, &dir->data, (uint64_t)index * 32, e, 32, false);
    return n < 0 ? (int)n : n < 32 ? -VX_ENOENT : 0;
}

static int write_entry(struct fat *fs, struct fat_node *dir, uint32_t index, const uint8_t e[32]) {
    int64_t n = chain_io(fs, &dir->data, (uint64_t)index * 32, (void *)e, 32, true);
    return n < 0 ? (int)n : n < 32 ? -VX_EIO : 0;
}

static uint8_t short_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    }
    return sum;
}

static uint16_t set_checksum(const uint8_t *entries, uint32_t count) {
    uint16_t sum = 0;
    for (uint32_t i = 0; i < count * 32; i++) {
        if (i != 2 && i != 3) {
            sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entries[i]);
        }
    }
    return sum;
}

static uint16_t name_hash(struct fat *fs, const uint16_t *name, uint32_t units) {
    uint16_t hash = 0;
    for (uint32_t i = 0; i < units; i++) {
        uint16_t c = upper(fs, name[i]);
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c & 0xff));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c >> 8));
    }
    return hash;
}

/* The name a short entry gives (with the case flags Windows NT sets). */
static uint32_t short_name(const uint8_t *e, uint16_t *out) {
    uint32_t n = 0;
    bool lower_base = e[12] & 0x08, lower_ext = e[12] & 0x10;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        uint8_t c = i == 0 && e[0] == 0x05 ? 0xe5 : e[i];
        out[n++] = lower_base && c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            out[n++] = lower_ext && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i];
        }
    }
    return n;
}

/* The next entry from *cursor (an entry index): 1 and *out, or 0 at the end. */
static int fat_next(struct fat *fs, struct fat_node *dir, uint32_t *cursor, struct info *out) {
    uint8_t e[32];
    int expect = -1; /* The long name's next sequence number, -1: none going. */
    uint8_t sum = 0;
    uint32_t start = 0, units = 0;
    for (;;) {
        uint32_t i = (*cursor)++;
        int error = read_entry(fs, dir, i, e);
        if (error == -VX_ENOENT || (!error && e[0] == 0x00)) {
            return 0;
        }
        if (error) {
            return error;
        }
        if (e[0] == 0xe5) {
            expect = -1;
            continue;
        }
        if ((e[11] & 0x3f) == ATTR_LONG_NAME) {
            int seq = e[0] & 0x1f;
            if (e[0] & 0x40) {
                expect = seq;
                sum = e[13];
                start = i;
                units = 0;
                for (int k = 0; k < MAX_UNITS; k++) {
                    out->name[k] = 0xffff;
                }
            }
            if (expect < 1 || seq != expect || e[13] != sum || seq > 20) {
                expect = -1;
                continue;
            }
            static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; k++) {
                uint32_t pos = (uint32_t)(seq - 1) * 13 + (uint32_t)k;
                if (pos < MAX_UNITS) {
                    out->name[pos] = le16(e + at[k]);
                }
            }
            expect--;
            continue;
        }
        if (e[11] & ATTR_VOLUME || e[0] == '.') {
            expect = -1;
            continue;
        }
        if (expect == 0 && short_checksum(e) == sum) {
            while (units < MAX_UNITS && out->name[units] != 0x0000 && out->name[units] != 0xffff) {
                units++;
            }
            out->index = start;
            out->count = i - start + 1;
        } else {
            units = short_name(e, out->name);
            out->index = i;
            out->count = 1;
        }
        out->units = units;
        out->attr = e[11];
        memset(&out->data, 0, sizeof(out->data));
        out->data.first = le16(e + 26) | (fs->kind == FAT32 ? (uint32_t)le16(e + 20) << 16 : 0);
        out->size = out->valid = e[11] & ATTR_DIRECTORY ? 0 : le32(e + 28);
        out->modified = from_fat_time(le16(e + 22), le16(e + 24));
        out->created = from_fat_time(le16(e + 14), le16(e + 16));
        return 1;
    }
}

static int exfat_next(struct fat *fs, struct fat_node *dir, uint32_t *cursor, struct info *out) {
    uint8_t e[32], s[32];
    for (;;) {
        uint32_t i = (*cursor)++;
        int error = read_entry(fs, dir, i, e);
        if (error == -VX_ENOENT || (!error && e[0] == 0x00)) {
            return 0;
        }
        if (error) {
            return error;
        }
        if (e[0] != EX_FILE || e[1] < 2 || e[1] > 18) {
            continue;
        }
        uint32_t secondaries = e[1];
        if ((error = read_entry(fs, dir, i + 1, s)) != 0) {
            return error == -VX_ENOENT ? 0 : error;
        }
        if (s[0] != EX_STREAM) {
            continue;
        }
        uint32_t units = s[3], got = 0;
        for (uint32_t k = 2; k <= secondaries && got < units; k++) {
            uint8_t n[32];
            if ((error = read_entry(fs, dir, i + k, n)) != 0) {
                return error == -VX_ENOENT ? 0 : error;
            }
            if (n[0] != EX_NAME) {
                break;
            }
            for (int j = 0; j < 15 && got < units; j++) {
                out->name[got++] = le16(n + 2 + j * 2);
            }
        }
        *cursor = i + 1 + secondaries;
        out->units = got;
        out->attr = (uint8_t)le16(e + 4);
        out->modified = from_fat_time((uint16_t)le32(e + 12), (uint16_t)(le32(e + 12) >> 16));
        out->created = from_fat_time((uint16_t)le32(e + 8), (uint16_t)(le32(e + 8) >> 16));
        memset(&out->data, 0, sizeof(out->data));
        out->data.first = le32(s + 20);
        out->size = le64(s + 24);
        out->valid = le64(s + 8);
        out->data.contiguous = s[1] & 2;
        if (out->data.contiguous) {
            out->data.clusters = (uint32_t)((out->size + fs->cluster_size - 1) / fs->cluster_size);
            out->data.counted = true;
        }
        out->index = i;
        out->count = 1 + secondaries;
        return 1;
    }
}

static int next_entry(struct fat *fs, struct fat_node *dir, uint32_t *cursor, struct info *out) {
    return fs->kind == EXFAT ? exfat_next(fs, dir, cursor, out) : fat_next(fs, dir, cursor, out);
}

static int find_entry(struct fat *fs, struct fat_node *dir, const uint16_t *name, uint32_t units,
                      struct info *out) {
    uint32_t cursor = 0;
    int result;
    while ((result = next_entry(fs, dir, &cursor, out)) == 1) {
        if (same_name(fs, name, units, out->name, out->units)) {
            return 0;
        }
    }
    return result < 0 ? result : -VX_ENOENT;
}

/* Finds room for `count` entries in a row in a directory (making it bigger
 * if it's full): the first one's index. */
static void update_entry(struct fat *fs, struct fat_node *node);

static int find_room(struct fat *fs, struct fat_node *dir, uint32_t count, uint32_t *out) {
    uint32_t run = 0, start = 0;
    for (uint32_t i = 0;; i++) {
        uint8_t e[32];
        int error = read_entry(fs, dir, i, e);
        if (error == -VX_ENOENT) {
            if (dir->data.fixed_root || i >= 65536 * (fs->kind == EXFAT ? 4u : 1u)) {
                return -VX_ENOSPC;
            }
            uint32_t clusters = dir->data.clusters;
            if ((error = chain_grow(fs, &dir->data, clusters + 1, true)) != 0) {
                return error;
            }
            if (fs->kind == EXFAT) { /* (Its length is in its entry.) */
                dir->vnode.size = dir->valid = (uint64_t)dir->data.clusters * fs->cluster_size;
                update_entry(fs, dir);
            }
            i--;
            continue;
        }
        if (error) {
            return error;
        }
        bool unused = fs->kind == EXFAT ? !(e[0] & EX_IN_USE) : e[0] == 0x00 || e[0] == 0xe5;
        if (!unused) {
            run = 0;
            continue;
        }
        if (run++ == 0) {
            start = i;
        }
        if (run == count) {
            *out = start;
            return 0;
        }
    }
}

static bool short_char(uint16_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c < 0x80 && c > ' ' && strchr("!#$%&'()-@^_`{}~", (char)c));
}

static bool short_taken(struct fat *fs, struct fat_node *dir, const uint8_t name[11]) {
    uint8_t e[32];
    for (uint32_t i = 0; read_entry(fs, dir, i, e) == 0 && e[0] != 0x00; i++) {
        if (e[0] != 0xe5 && (e[11] & 0x3f) != ATTR_LONG_NAME && memcmp(e, name, 11) == 0) {
            return true;
        }
    }
    return false;
}

/* The 8.3 name for a long one: itself if it is one (then no long name is
 * needed: false), else BASE~N.EXT. */
static bool make_short(struct fat *fs, struct fat_node *dir, const uint16_t *name, uint32_t units,
                       uint8_t out[11]) {
    memset(out, ' ', 11);
    int dot = -1;
    for (uint32_t i = 0; i < units; i++) {
        if (name[i] == '.') {
            dot = (int)i;
        }
    }
    /* Already 8.3, in capitals? */
    uint32_t base_len = dot < 0 ? units : (uint32_t)dot;
    uint32_t ext_len = dot < 0 ? 0 : units - (uint32_t)dot - 1;
    bool exact = base_len >= 1 && base_len <= 8 && ext_len <= 3 && (dot < 0 || ext_len > 0);
    for (uint32_t i = 0; i < units && exact; i++) {
        exact = (int)i == dot || short_char(name[i]);
    }
    if (exact) {
        for (uint32_t i = 0; i < base_len; i++) {
            out[i] = (uint8_t)name[i];
        }
        for (uint32_t i = 0; i < ext_len; i++) {
            out[8 + i] = (uint8_t)name[dot + 1 + (int)i];
        }
        if (out[0] == 0xe5) {
            out[0] = 0x05;
        }
        return false;
    }
    /* BASE~N: up to six of its characters (in capitals; others as _). */
    uint8_t base[8];
    int n = 0;
    for (uint32_t i = 0; i < base_len && n < 6; i++) {
        uint16_t c = upper(fs, name[i]);
        if (c == ' ' || c == '.') {
            continue;
        }
        base[n++] = short_char(c) ? (uint8_t)c : '_';
    }
    if (n == 0) {
        base[n++] = '_';
    }
    for (uint32_t i = 0, k = 0; dot >= 0 && i < ext_len && k < 3; i++) {
        uint16_t c = upper(fs, name[dot + 1 + (int)i]);
        if (c != ' ') {
            out[8 + k++] = short_char(c) ? (uint8_t)c : '_';
        }
    }
    for (uint32_t number = 1; number < 1000000; number++) {
        char digits[8];
        int d = 0;
        for (uint32_t v = number; v; v /= 10) {
            digits[d++] = (char)('0' + v % 10);
        }
        int keep = n < 7 - d ? n : 7 - d;
        memset(out, ' ', 8);
        memcpy(out, base, (size_t)keep);
        out[keep] = '~';
        for (int k = 0; k < d; k++) {
            out[keep + 1 + k] = (uint8_t)digits[d - 1 - k];
        }
        if (!short_taken(fs, dir, out)) {
            break;
        }
    }
    return true;
}

/* Writes the entries for `in` (its name, attributes, data and times) into
 * a directory: in->index and in->count say where. */
static int add_entries(struct fat *fs, struct fat_node *dir, struct info *in) {
    uint8_t *entries;
    uint32_t count;
    if (fs->kind == EXFAT) {
        count = 2 + (in->units + 14) / 15;
        entries = kzalloc(count * 32);
        if (!entries) {
            return -VX_ENOMEM;
        }
        uint8_t *f = entries, *s = entries + 32;
        f[0] = EX_FILE;
        f[1] = (uint8_t)(count - 1);
        put16(f + 4, in->attr);
        put32(f + 8, to_fat_time(in->created ? in->created : in->modified));
        put32(f + 12, to_fat_time(in->modified));
        put32(f + 16, to_fat_time(in->modified));
        s[0] = EX_STREAM;
        s[1] = (uint8_t)(1 | (in->data.contiguous ? 2 : 0));
        s[3] = (uint8_t)in->units;
        put16(s + 4, name_hash(fs, in->name, in->units));
        put64(s + 8, in->valid);
        put32(s + 20, in->data.first);
        put64(s + 24, in->size);
        for (uint32_t k = 0; k < in->units; k++) {
            uint8_t *n = entries + 64 + (k / 15) * 32;
            n[0] = EX_NAME;
            put16(n + 2 + (k % 15) * 2, in->name[k]);
        }
        put16(f + 2, set_checksum(entries, count));
    } else {
        uint8_t short_entry[11];
        bool long_name = make_short(fs, dir, in->name, in->units, short_entry);
        uint32_t parts = long_name ? (in->units + 12) / 13 : 0;
        count = parts + 1;
        entries = kzalloc(count * 32);
        if (!entries) {
            return -VX_ENOMEM;
        }
        uint8_t sum = short_checksum(short_entry);
        static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (uint32_t j = 0; j < parts; j++) {
            uint8_t *e = entries + j * 32;
            uint32_t seq = parts - j;
            e[0] = (uint8_t)(seq | (j == 0 ? 0x40 : 0));
            e[11] = ATTR_LONG_NAME;
            e[13] = sum;
            for (int k = 0; k < 13; k++) {
                uint32_t pos = (seq - 1) * 13 + (uint32_t)k;
                uint16_t c = pos < in->units ? in->name[pos] : pos == in->units ? 0x0000 : 0xffff;
                put16(e + at[k], c);
            }
        }
        uint8_t *e = entries + parts * 32;
        memcpy(e, short_entry, 11);
        e[11] = in->attr;
        uint32_t created = to_fat_time(in->created ? in->created : in->modified);
        uint32_t modified = to_fat_time(in->modified);
        put16(e + 14, created), put16(e + 16, created >> 16);
        put16(e + 18, modified >> 16);
        put16(e + 20, fs->kind == FAT32 ? in->data.first >> 16 : 0);
        put16(e + 22, modified), put16(e + 24, modified >> 16);
        put16(e + 26, in->data.first);
        put32(e + 28, in->attr & ATTR_DIRECTORY ? 0 : (uint32_t)in->size);
    }
    uint32_t index;
    int error = find_room(fs, dir, count, &index);
    for (uint32_t k = 0; k < count && !error; k++) {
        error = write_entry(fs, dir, index + k, entries + k * 32);
    }
    kfree(entries);
    if (!error) {
        in->index = index;
        in->count = count;
    }
    return error;
}

static int remove_entries(struct fat *fs, struct fat_node *dir, uint32_t index, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        uint8_t e[32];
        int error = read_entry(fs, dir, index + k, e);
        if (!error) {
            e[0] = fs->kind == EXFAT ? e[0] & ~EX_IN_USE : 0xe5;
            error = write_entry(fs, dir, index + k, e);
        }
        if (error) {
            return error;
        }
    }
    return 0;
}

/* Writes a node's size, place and time back into its entry. */
static void update_entry(struct fat *fs, struct fat_node *node) {
    struct fat_node *dir = node->parent;
    if (!dir || node->removed) {
        return; /* (The root has no entry.) */
    }
    uint32_t modified = to_fat_time(node->vnode.modified);
    if (fs->kind != EXFAT) {
        uint8_t e[32];
        uint32_t at = node->index + node->count - 1;
        if (read_entry(fs, dir, at, e)) {
            return;
        }
        e[11] = node->attr;
        put16(e + 20, fs->kind == FAT32 ? node->data.first >> 16 : 0);
        put16(e + 26, node->data.first);
        put32(e + 28, node->vnode.type == VX_TYPE_DIRECTORY ? 0 : (uint32_t)node->vnode.size);
        put16(e + 22, modified), put16(e + 24, modified >> 16);
        put16(e + 18, modified >> 16);
        write_entry(fs, dir, at, e);
        return;
    }
    uint8_t *entries = kmalloc(node->count * 32);
    if (!entries) {
        return;
    }
    for (uint32_t k = 0; k < node->count; k++) {
        if (read_entry(fs, dir, node->index + k, entries + k * 32)) {
            kfree(entries);
            return;
        }
    }
    uint8_t *f = entries, *s = entries + 32;
    put16(f + 4, node->attr);
    put32(f + 12, modified);
    put32(f + 16, modified);
    s[1] = (uint8_t)(1 | (node->data.contiguous ? 2 : 0));
    put64(s + 8, node->valid);
    put32(s + 20, node->data.first);
    put64(s + 24, node->vnode.size);
    put16(f + 2, set_checksum(entries, node->count));
    write_entry(fs, dir, node->index, f);
    write_entry(fs, dir, node->index + 1, s);
    kfree(entries);
}

/* ---- Nodes ---- */

static uint64_t inode_number(struct fat_node *parent, uint32_t index) {
    if (!parent) {
        return 1;
    }
    return ((uint64_t)parent->data.first << 24 | index) + 2;
}

static int get_node(struct fat *fs, struct fat_node *dir, const struct info *in,
                    struct vnode **out) {
    for (struct fat_node *node = fs->open_nodes; node; node = node->next) {
        if (node->parent == dir && node->index == in->index && !node->removed) {
            vnode_ref(&node->vnode);
            *out = &node->vnode;
            return 0;
        }
    }
    struct fat_node *node = kzalloc(sizeof(*node));
    if (!node) {
        return -VX_ENOMEM;
    }
    bool is_dir = in->attr & ATTR_DIRECTORY;
    vnode_init(&node->vnode, fs->mount, is_dir ? VX_TYPE_DIRECTORY : VX_TYPE_FILE, &fat_ops);
    node->data = in->data;
    node->attr = in->attr;
    node->valid = in->valid;
    node->created = in->created;
    node->index = in->index;
    node->count = in->count;
    node->parent = dir;
    if (dir) {
        vnode_ref(&dir->vnode);
    }
    node->vnode.inode = inode_number(dir, in->index);
    node->vnode.size = in->size;
    node->vnode.links = is_dir ? 2 : 1;
    node->vnode.mode = (is_dir ? 0755 : 0644) & (in->attr & ATTR_READ_ONLY ? ~0222u : ~0u);
    node->vnode.modified = in->modified;
    node->vnode.data = fs;
    if (is_dir && fs->kind != EXFAT) {
        chain_count(fs, &node->data);
        node->vnode.size = (uint64_t)node->data.clusters * fs->cluster_size;
    }
    node->next = fs->open_nodes;
    fs->open_nodes = node;
    *out = &node->vnode;
    return 0;
}

static void fat_release(struct vnode *vnode) {
    struct fat *fs = fs_of(vnode);
    struct fat_node *node = node_of(vnode);
    for (struct fat_node **link = &fs->open_nodes; *link; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            break;
        }
    }
    if (node->removed) { /* (Its name went while it was open.) */
        chain_shrink(fs, &node->data, 0);
    }
    struct fat_node *parent = node->parent;
    kfree(node);
    if (parent) {
        vnode_put(&parent->vnode);
    }
}

/* ---- Directories ---- */

static int fat_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct fat *fs = fs_of(dir);
    struct fat_node *d = node_of(dir);
    if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.' &&
                                            !d->parent)) {
        vnode_ref(dir);
        *out = dir;
        return 0;
    }
    if (length == 2 && name[0] == '.' && name[1] == '.') {
        vnode_ref(&d->parent->vnode);
        *out = &d->parent->vnode;
        return 0;
    }
    struct info *in = kmalloc(sizeof(*in));
    uint16_t *wanted = kmalloc(sizeof(uint16_t) * (MAX_UNITS + 1));
    int error = in && wanted ? 0 : -VX_ENOMEM;
    int units = error ? 0 : utf8_to_16(name, length, wanted);
    if (!error && units <= 0) {
        error = -VX_ENOENT;
    }
    if (!error) {
        error = find_entry(fs, d, wanted, (uint32_t)units, in);
    }
    if (!error) {
        error = get_node(fs, d, in, out);
    }
    kfree(in);
    kfree(wanted);
    return error;
}

static int fat_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *out) {
    struct fat *fs = fs_of(dir);
    struct info *in = kmalloc(sizeof(*in));
    if (!in) {
        return -VX_ENOMEM;
    }
    uint32_t cursor = (uint32_t)*cookie;
    int result = next_entry(fs, node_of(dir), &cursor, in);
    *cookie = cursor;
    if (result == 1) {
        out->inode = inode_number(node_of(dir), in->index);
        out->type = in->attr & ATTR_DIRECTORY ? VX_TYPE_DIRECTORY : VX_TYPE_FILE;
        out->name_length = (uint32_t)utf16_to_8(in->name, in->units, out->name);
    }
    kfree(in);
    return result;
}

static bool invalid_name(const uint16_t *name, uint32_t units) {
    for (uint32_t i = 0; i < units; i++) {
        if (name[i] < 0x20 || (name[i] < 0x80 && strchr("\"*/:<>?\\|", (char)name[i]))) {
            return true;
        }
    }
    return units == 0;
}

/* The info for a name in a directory (UTF-16), for creating or renaming. */
static int name_info(const char *name, size_t length, struct info *in) {
    int units = utf8_to_16(name, length, in->name);
    if (units < 0) {
        return -VX_ENAMETOOLONG;
    }
    in->units = (uint32_t)units;
    /* Names don't end in dots or spaces on FAT (Windows drops them). */
    while (in->units && (in->name[in->units - 1] == '.' || in->name[in->units - 1] == ' ')) {
        in->units--;
    }
    return invalid_name(in->name, in->units) ? -VX_EINVAL : 0;
}

static int fat_create(struct vnode *dir, const char *name, size_t length, uint32_t type,
                      struct vnode **out) {
    struct fat *fs = fs_of(dir);
    struct fat_node *d = node_of(dir);
    if (type != VX_TYPE_FILE && type != VX_TYPE_DIRECTORY) {
        return -VX_EINVAL; /* (No links, sockets or devices on FAT.) */
    }
    struct info *in = kzalloc(sizeof(*in));
    if (!in) {
        return -VX_ENOMEM;
    }
    int error = name_info(name, length, in);
    in->modified = in->created = time_now();
    in->attr = type == VX_TYPE_DIRECTORY ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    if (!error && type == VX_TYPE_DIRECTORY) {
        /* A cluster for it, zeroed; FAT's also get "." and "..". */
        error = chain_grow(fs, &in->data, 1, true);
        if (!error && fs->kind == EXFAT) {
            in->size = in->valid = fs->cluster_size;
        }
        if (!error && fs->kind != EXFAT) {
            uint8_t e[64] = {0};
            uint32_t now = to_fat_time(in->modified);
            for (int k = 0; k < 2; k++) {
                uint8_t *p = e + k * 32;
                uint32_t cluster = k == 0 ? in->data.first : d->parent ? d->data.first : 0;
                memset(p, ' ', 11);
                p[0] = '.';
                p[1] = k ? '.' : ' ';
                p[11] = ATTR_DIRECTORY;
                put16(p + 14, now), put16(p + 16, now >> 16), put16(p + 18, now >> 16);
                put16(p + 22, now), put16(p + 24, now >> 16);
                put16(p + 20, fs->kind == FAT32 ? cluster >> 16 : 0);
                put16(p + 26, cluster);
            }
            error = block_write_bytes(fs->device, cluster_offset(fs, in->data.first), e, 64);
        }
    }
    if (!error) {
        error = add_entries(fs, d, in);
        if (error) {
            chain_shrink(fs, &in->data, 0);
        }
    }
    if (!error) {
        dir->modified = in->modified;
        update_entry(fs, d);
        error = get_node(fs, d, in, out);
    }
    kfree(in);
    return error;
}

static struct fat_node *open_node(struct fat *fs, struct fat_node *dir, uint32_t index) {
    for (struct fat_node *node = fs->open_nodes; node; node = node->next) {
        if (node->parent == dir && node->index == index && !node->removed) {
            return node;
        }
    }
    return NULL;
}

/* True if a directory (given by its entry) holds nothing. */
static int is_empty(struct fat *fs, struct fat_node *parent, const struct info *in) {
    struct vnode *v;
    int error = get_node(fs, parent, in, &v);
    if (error) {
        return error;
    }
    struct info *other = kmalloc(sizeof(*other));
    uint32_t cursor = 0;
    int result = other ? next_entry(fs, node_of(v), &cursor, other) : -VX_ENOMEM;
    kfree(other);
    vnode_put(v);
    return result < 0 ? result : result == 0;
}

/* Takes an entry out of a directory; its clusters go now, or when the
 * file is closed if it's open. */
static int drop_entry(struct fat *fs, struct fat_node *dir, struct info *in) {
    int error = remove_entries(fs, dir, in->index, in->count);
    if (error) {
        return error;
    }
    struct fat_node *open = open_node(fs, dir, in->index);
    if (open) {
        open->removed = true;
        return 0;
    }
    return chain_shrink(fs, &in->data, 0);
}

static int fat_remove(struct vnode *dir, const char *name, size_t length) {
    struct fat *fs = fs_of(dir);
    struct fat_node *d = node_of(dir);
    struct info *in = kmalloc(sizeof(*in));
    uint16_t *wanted = kmalloc(sizeof(uint16_t) * (MAX_UNITS + 1));
    int error = in && wanted ? 0 : -VX_ENOMEM;
    int units = error ? 0 : utf8_to_16(name, length, wanted);
    if (!error) {
        error = units <= 0 ? -VX_ENOENT : find_entry(fs, d, wanted, (uint32_t)units, in);
    }
    if (!error && in->attr & ATTR_DIRECTORY) {
        int empty = is_empty(fs, d, in);
        error = empty < 0 ? empty : empty ? 0 : -VX_ENOTEMPTY;
    }
    if (!error) {
        error = drop_entry(fs, d, in);
    }
    if (!error) {
        dir->modified = time_now();
        update_entry(fs, d);
    }
    kfree(in);
    kfree(wanted);
    return error;
}

static int fat_rename(struct vnode *old_dir, const char *old_name, size_t old_length,
                      struct vnode *new_dir, const char *new_name, size_t new_length) {
    struct fat *fs = fs_of(old_dir);
    struct fat_node *from = node_of(old_dir), *to = node_of(new_dir);
    struct info *moving = kmalloc(sizeof(*moving));
    struct info *target = kmalloc(sizeof(*target));
    struct info *fresh = kzalloc(sizeof(*fresh));
    int error = moving && target && fresh ? 0 : -VX_ENOMEM;
    if (!error) {
        int units = utf8_to_16(old_name, old_length, target->name);
        error = units <= 0 ? -VX_ENOENT : find_entry(fs, from, target->name, (uint32_t)units, moving);
    }
    if (!error) {
        error = name_info(new_name, new_length, fresh);
    }
    /* Into one of its own directories: no. */
    for (struct fat_node *up = to; !error && up && moving->attr & ATTR_DIRECTORY; up = up->parent) {
        if (up->parent == from && up->index == moving->index) {
            error = -VX_EINVAL;
        }
    }
    /* Something there already (not itself, in other capitals) goes. */
    if (!error && find_entry(fs, to, fresh->name, fresh->units, target) == 0 &&
        !(to == from && target->index == moving->index)) {
        bool target_dir = target->attr & ATTR_DIRECTORY, moving_dir = moving->attr & ATTR_DIRECTORY;
        if (target_dir != moving_dir) {
            error = target_dir ? -VX_EISDIR : -VX_ENOTDIR;
        } else if (target_dir) {
            int empty = is_empty(fs, to, target);
            error = empty < 0 ? empty : empty ? 0 : -VX_ENOTEMPTY;
        }
        if (!error) {
            error = drop_entry(fs, to, target);
        }
    }
    if (!error) {
        fresh->attr = moving->attr;
        fresh->data = moving->data;
        fresh->size = moving->size;
        fresh->valid = moving->valid;
        fresh->modified = moving->modified;
        fresh->created = moving->created;
        struct fat_node *open = open_node(fs, from, moving->index);
        if (open) { /* (What it has may be newer than its entry.) */
            fresh->data = open->data;
            fresh->size = open->vnode.size;
            fresh->valid = open->valid;
            fresh->modified = open->vnode.modified;
            fresh->attr = open->attr;
        }
        error = add_entries(fs, to, fresh);
        if (!error) {
            error = remove_entries(fs, from, moving->index, moving->count);
        }
        if (!error && to != from && fresh->attr & ATTR_DIRECTORY && fs->kind != EXFAT) {
            /* Its ".." names its new parent. */
            uint8_t e[32];
            uint64_t at = cluster_offset(fs, fresh->data.first) + 32;
            uint32_t parent = to->parent ? to->data.first : 0;
            if (valid_cluster(fs, fresh->data.first) &&
                block_read_bytes(fs->device, at, e, 32) == 0 && e[0] == '.' && e[1] == '.') {
                put16(e + 20, fs->kind == FAT32 ? parent >> 16 : 0);
                put16(e + 26, parent);
                block_write_bytes(fs->device, at, e, 32);
            }
        }
        if (!error && open) {
            if (to != from) {
                vnode_ref(&to->vnode);
                open->parent = to;
                vnode_put(&from->vnode);
            }
            open->index = fresh->index;
            open->count = fresh->count;
        }
    }
    if (!error) {
        old_dir->modified = new_dir->modified = time_now();
        update_entry(fs, from);
        if (to != from) {
            update_entry(fs, to);
        }
    }
    kfree(moving);
    kfree(target);
    kfree(fresh);
    return error;
}

/* ---- Files ---- */

static int64_t fat_read(struct vnode *vnode, void *buffer, size_t size, uint64_t offset) {
    struct fat_node *node = node_of(vnode);
    if (offset >= vnode->size) {
        return 0;
    }
    if (size > vnode->size - offset) {
        size = (size_t)(vnode->size - offset);
    }
    /* exFAT: past what was written reads as zeros. */
    size_t stored = offset >= node->valid ? 0 : size > node->valid - offset
                                                     ? (size_t)(node->valid - offset) : size;
    int64_t n = stored ? chain_io(fs_of(vnode), &node->data, offset, buffer, stored, false) : 0;
    if (n < 0) {
        return n;
    }
    memset((uint8_t *)buffer + n, 0, size - (size_t)n);
    return (int64_t)size;
}

static int grow_to(struct fat *fs, struct fat_node *node, uint64_t size) {
    if (size > 0xffffffffull && fs->kind != EXFAT) {
        return -VX_ENOSPC; /* (FAT's files stop at 4 GiB.) */
    }
    uint64_t clusters = (size + fs->cluster_size - 1) / fs->cluster_size;
    if (clusters > fs->cluster_count) {
        return -VX_ENOSPC;
    }
    return chain_grow(fs, &node->data, (uint32_t)clusters, false);
}

static int64_t fat_write(struct vnode *vnode, const void *buffer, size_t size, uint64_t offset) {
    struct fat *fs = fs_of(vnode);
    struct fat_node *node = node_of(vnode);
    if (vnode->type != VX_TYPE_FILE) {
        return -VX_EISDIR;
    }
    if (size == 0) {
        return 0;
    }
    int error = grow_to(fs, node, offset + size);
    /* A gap from the end to where this starts: zeros. */
    for (uint64_t at = node->valid; !error && at < offset;) {
        size_t n = offset - at < sizeof(zeros) ? (size_t)(offset - at) : sizeof(zeros);
        int64_t written = chain_io(fs, &node->data, at, (void *)zeros, n, true);
        error = written < 0 ? (int)written : 0;
        at += n;
    }
    int64_t n = error ? error : chain_io(fs, &node->data, offset, (void *)buffer, size, true);
    if (n > 0) {
        if (offset + (uint64_t)n > vnode->size) {
            vnode->size = offset + (uint64_t)n;
        }
        if (offset + (uint64_t)n > node->valid) {
            node->valid = offset + (uint64_t)n;
        }
        vnode->modified = time_now();
        node->attr |= ATTR_ARCHIVE;
        update_entry(fs, node);
    }
    return n;
}

static int fat_truncate(struct vnode *vnode, uint64_t size) {
    struct fat *fs = fs_of(vnode);
    struct fat_node *node = node_of(vnode);
    if (vnode->type != VX_TYPE_FILE) {
        return -VX_EISDIR;
    }
    int error;
    if (size < vnode->size) {
        uint64_t keep = (size + fs->cluster_size - 1) / fs->cluster_size;
        error = chain_shrink(fs, &node->data, (uint32_t)keep);
        if (node->valid > size) {
            node->valid = size;
        }
    } else {
        error = grow_to(fs, node, size);
        /* What's new reads as zeros: written out (exFAT: past `valid` anyway). */
        for (uint64_t at = node->valid; !error && fs->kind != EXFAT && at < size;) {
            size_t n = size - at < sizeof(zeros) ? (size_t)(size - at) : sizeof(zeros);
            int64_t written = chain_io(fs, &node->data, at, (void *)zeros, n, true);
            error = written < 0 ? (int)written : 0;
            at += n;
        }
        if (!error && fs->kind != EXFAT) {
            node->valid = size;
        }
    }
    if (!error) {
        vnode->size = size;
        vnode->modified = time_now();
        update_entry(fs, node);
    }
    return error;
}

static int fat_set_mode(struct vnode *vnode) {
    struct fat_node *node = node_of(vnode);
    node->attr = vnode->mode & 0200 ? node->attr & ~ATTR_READ_ONLY : node->attr | ATTR_READ_ONLY;
    update_entry(fs_of(vnode), node);
    return 0;
}

static void fat_statfs(struct mount *mount, uint64_t *total, uint64_t *free) {
    struct fat *fs = mount->data;
    count_free(fs);
    *total = (uint64_t)fs->cluster_count * fs->cluster_size;
    *free = (uint64_t)(fs->free_clusters > 0 ? fs->free_clusters : 0) * fs->cluster_size;
}

static const struct vnode_ops fat_ops = {
    .lookup = fat_lookup,
    .create = fat_create,
    .remove = fat_remove,
    .rename = fat_rename,
    .read_dir = fat_read_dir,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .set_mode = fat_set_mode,
    .statfs = fat_statfs,
    .release = fat_release,
};

/* ---- Mounting ---- */

/* exFAT: the root directory's bitmap and up-case table entries. */
static int exfat_tables(struct fat *fs, struct fat_node *root) {
    uint8_t e[32];
    uint32_t bitmap_cluster = 0, upcase_cluster = 0;
    uint64_t bitmap_length = 0, upcase_length = 0;
    for (uint32_t i = 0; read_entry(fs, root, i, e) == 0 && e[0] != 0x00 && i < 4096; i++) {
        if (e[0] == EX_BITMAP && !(e[1] & 1) && !bitmap_cluster) {
            bitmap_cluster = le32(e + 20);
            bitmap_length = le64(e + 24);
        } else if (e[0] == EX_UPCASE) {
            upcase_cluster = le32(e + 20);
            upcase_length = le64(e + 24);
        }
    }
    if (!valid_cluster(fs, bitmap_cluster) || bitmap_length < (fs->cluster_count + 7) / 8) {
        return -VX_EINVAL;
    }
    fs->bitmap_start = cluster_offset(fs, bitmap_cluster);
    fs->bitmap = kmalloc((size_t)bitmap_length);
    if (!fs->bitmap) {
        return -VX_ENOMEM;
    }
    int error = block_read_bytes(fs->device, fs->bitmap_start, fs->bitmap, (size_t)bitmap_length);
    if (error) {
        return error;
    }
    /* The up-case table (it may be compressed: 0xffff, n means the next n
     * characters are their own capitals). Without it: ASCII's. */
    if (valid_cluster(fs, upcase_cluster) && upcase_length && upcase_length <= 0x40000) {
        uint16_t *table = kmalloc(65536 * 2);
        uint8_t *raw = kmalloc((size_t)upcase_length);
        if (table && raw &&
            block_read_bytes(fs->device, cluster_offset(fs, upcase_cluster), raw,
                             (size_t)upcase_length) == 0) {
            for (uint32_t c = 0; c < 65536; c++) {
                table[c] = (uint16_t)c;
            }
            uint32_t c = 0;
            for (uint64_t i = 0; i + 1 < upcase_length && c < 65536; i += 2) {
                uint16_t v = le16(raw + i);
                if (v == 0xffff && i + 3 < upcase_length) {
                    c += le16(raw + i + 2);
                    i += 2;
                } else {
                    table[c++] = v;
                }
            }
            fs->upcase = table;
            table = NULL;
        }
        kfree(table);
        kfree(raw);
    }
    return 0;
}

static int fat_mount(struct mount *mount, struct block_device *device) {
    if (!device) {
        return -VX_EINVAL;
    }
    uint8_t *b = kmalloc(512);
    struct fat *fs = kzalloc(sizeof(*fs));
    if (!b || !fs) {
        kfree(b);
        kfree(fs);
        return -VX_ENOMEM;
    }
    int error = block_read_bytes(device, 0, b, 512);
    fs->device = device;
    fs->mount = mount;
    fs->free_clusters = -1;
    if (!error && memcmp(b + 3, "EXFAT   ", 8) == 0) {
        uint32_t sector = 1u << b[108], spc = 1u << b[109];
        fs->kind = EXFAT;
        fs->cluster_size = sector * spc;
        fs->fat_start = (uint64_t)le32(b + 80) * sector;
        fs->fat_bytes = (uint64_t)le32(b + 84) * sector;
        fs->fat_count = b[110];
        fs->data_start = (uint64_t)le32(b + 88) * sector;
        fs->cluster_count = le32(b + 92);
        fs->root_cluster = le32(b + 96);
        if (b[108] < 9 || b[108] > 12 || b[108] + b[109] > 25) {
            error = -VX_EINVAL;
        }
    } else if (!error) {
        uint32_t sector = le16(b + 11), spc = b[13], reserved = le16(b + 14);
        uint32_t fat_size = le16(b + 22) ? le16(b + 22) : le32(b + 36);
        uint32_t total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
        fs->fat_count = b[16];
        fs->root_entries = le16(b + 17);
        if (sector < 512 || sector > 4096 || (sector & (sector - 1)) || !spc || (spc & (spc - 1)) ||
            !reserved || !fs->fat_count || !fat_size) {
            error = -VX_EINVAL;
        } else {
            uint32_t root_sectors = (fs->root_entries * 32 + sector - 1) / sector;
            uint32_t data_sector = reserved + fs->fat_count * fat_size + root_sectors;
            fs->cluster_size = sector * spc;
            fs->fat_start = (uint64_t)reserved * sector;
            fs->fat_bytes = (uint64_t)fat_size * sector;
            fs->root_start = (uint64_t)(reserved + fs->fat_count * fat_size) * sector;
            fs->data_start = (uint64_t)data_sector * sector;
            fs->cluster_count = total > data_sector ? (total - data_sector) / spc : 0;
            fs->kind = fs->cluster_count < 4085 ? FAT12 : fs->cluster_count < 65525 ? FAT16 : FAT32;
            fs->root_cluster = fs->kind == FAT32 ? le32(b + 44) : 0;
            if (fs->cluster_count == 0) {
                error = -VX_EINVAL;
            }
        }
    }
    kfree(b);
    /* The FAT must fit the clusters it describes. */
    uint64_t needed = fs->kind == FAT12 ? (fs->cluster_count + 2) * 3 / 2
                      : fs->kind == FAT16 ? (fs->cluster_count + 2) * 2ull
                                          : (fs->cluster_count + 2) * 4ull;
    if (!error && fs->fat_bytes < needed) {
        error = -VX_EINVAL;
    }
    struct info *root = error ? NULL : kzalloc(sizeof(*root));
    if (!error && !root) {
        error = -VX_ENOMEM;
    }
    if (error) {
        kfree(fs);
        return error;
    }
    fs->next_free = 2;
    mount->data = fs;
    mount->read_only = !device->write;
    root->attr = ATTR_DIRECTORY;
    root->data.first = fs->root_cluster;
    root->data.fixed_root = fs->kind == FAT12 || fs->kind == FAT16;
    error = get_node(fs, NULL, root, &mount->root);
    kfree(root);
    if (!error && fs->kind == EXFAT) {
        struct fat_node *r = node_of(mount->root);
        chain_count(fs, &r->data);
        r->vnode.size = r->valid = (uint64_t)r->data.clusters * fs->cluster_size;
        error = exfat_tables(fs, r);
    }
    if (error) {
        if (mount->root) {
            vnode_put(mount->root);
            mount->root = NULL;
        }
        kfree(fs->bitmap);
        kfree(fs->upcase);
        kfree(fs);
        return error;
    }
    if (fs->kind == FAT32 && !mount->read_only) {
        /* FSInfo's free count would go stale: "not known". */
        uint8_t info[4];
        uint32_t sector = 0;
        uint8_t boot[64];
        if (block_read_bytes(device, 0, boot, 64) == 0 && (sector = le16(boot + 48)) != 0 &&
            block_read_bytes(device, (uint64_t)sector * le16(boot + 11), info, 4) == 0 &&
            memcmp(info, "RRaA", 4) == 0) {
            uint8_t unknown[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            block_write_bytes(device, (uint64_t)sector * le16(boot + 11) + 488, unknown, 8);
        }
    }
    mount->fs_name = fs->kind == EXFAT ? "exfat" : "vfat";
    static const char *const names[] = {"FAT12", "FAT16", "FAT32", "exFAT"};
    kprintf("[fat] %s: %s, %u clusters of %u bytes%s\n", device->name, names[fs->kind],
            fs->cluster_count, fs->cluster_size, mount->read_only ? " (read-only)" : "");
    return 0;
}

const struct filesystem_type fat_type = {
    .name = "fat",
    .mount = fat_mount,
};

/* True if the device holds a FAT or exFAT file system. */
bool fat_probe(struct block_device *device) {
    uint8_t b[512];
    if (block_read_bytes(device, 0, b, sizeof(b)) != 0) {
        return false;
    }
    if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
        return true;
    }
    uint32_t sector = le16(b + 11), spc = b[13];
    return b[510] == 0x55 && b[511] == 0xaa && (b[0] == 0xeb || b[0] == 0xe9) &&
           sector >= 512 && sector <= 4096 && !(sector & (sector - 1)) && spc &&
           !(spc & (spc - 1)) && le16(b + 14) && (b[16] == 1 || b[16] == 2) &&
           (memcmp(b + 54, "FAT", 3) == 0 || memcmp(b + 82, "FAT32", 5) == 0);
}
