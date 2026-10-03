/*
 * ext3's journal (JBD2, Linux's format; see journal.h).
 *
 * Writing, Vexa keeps it simple: each transaction is written from the
 * journal's start, committed, written to its place (checkpointed) and the
 * journal marked empty, before the next one; so a crash leaves at most one
 * committed transaction to replay, and no revoke records are needed.
 *
 * Replaying is for any journal Linux may have left too: several
 * transactions, wrapping around, revoke records (blocks freed later in it,
 * not to be replayed), and the tag formats of 64-bit and checksummed journals
 * (whose checksums aren't checked).
 */
#include <vexa/abi.h>
#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>

#include "journal.h"

#define JBD2_MAGIC 0xc03b3998U
#define TYPE_DESCRIPTOR 1
#define TYPE_COMMIT 2
#define TYPE_SUPERBLOCK_V1 3
#define TYPE_SUPERBLOCK_V2 4
#define TYPE_REVOKE 5

#define TAG_ESCAPE 1
#define TAG_SAME_UUID 2
#define TAG_LAST 8

#define COMPAT_CHECKSUM 0x1
#define INCOMPAT_REVOKE 0x1
#define INCOMPAT_64BIT 0x2
#define INCOMPAT_ASYNC_COMMIT 0x4
#define INCOMPAT_CSUM_V2 0x8
#define INCOMPAT_CSUM_V3 0x10

/* The journal superblock's fields (big-endian), by offset. */
#define SB_BLOCKSIZE 12
#define SB_MAXLEN 16
#define SB_FIRST 20
#define SB_SEQUENCE 24
#define SB_START 28
#define SB_COMPAT 36
#define SB_INCOMPAT 40
#define SB_UUID 48

struct journal {
    struct block_device *device;
    uint32_t block_size;
    uint32_t *blocks; /* Journal block -> disk block. */
    uint32_t first, end; /* Its log: blocks first to end - 1. */
    uint32_t sequence;   /* The next transaction's. */
    uint32_t compat, incompat;
    uint8_t uuid[16];
    uint8_t *super;      /* Its superblock, as on the disk. */
    uint8_t *scratch;
    bool writable;
};

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int read_journal(struct journal *j, uint32_t index, void *buffer) {
    return block_read_bytes(j->device, (uint64_t)j->blocks[index] * j->block_size, buffer,
                            j->block_size);
}

static int write_journal(struct journal *j, uint32_t index, const void *buffer) {
    return block_write_bytes(j->device, (uint64_t)j->blocks[index] * j->block_size, buffer,
                             j->block_size);
}

static int write_super(struct journal *j) {
    return write_journal(j, 0, j->super);
}

/* The log wraps around: index after `index`, `n` blocks on. */
static uint32_t advance(struct journal *j, uint32_t index, uint32_t n) {
    uint32_t length = j->end - j->first;
    return j->first + (index - j->first + n) % length;
}

/* ---- Replaying ---- */

static uint32_t tag_size(struct journal *j) {
    if (j->incompat & INCOMPAT_CSUM_V3) {
        return 16;
    }
    return 8 + ((j->incompat & INCOMPAT_64BIT) ? 4 : 0);
}

static uint32_t tag_flags(struct journal *j, const uint8_t *tag) {
    return (j->incompat & INCOMPAT_CSUM_V3) ? get32(tag + 4) : (uint32_t)(tag[6] << 8 | tag[7]);
}

static uint64_t tag_block(struct journal *j, const uint8_t *tag) {
    uint64_t block = get32(tag);
    if (j->incompat & INCOMPAT_64BIT) {
        block |= (uint64_t)get32(tag + ((j->incompat & INCOMPAT_CSUM_V3) ? 8 : 8)) << 32;
    }
    return block;
}

/* Calls `visit` for each tag of a descriptor block, with its data block's
 * place in the log; returns how many tags there were. */
typedef void (*tag_visitor)(struct journal *j, uint64_t target, uint32_t flags, uint32_t data,
                            uint32_t sequence, void *arg);

static uint32_t walk_tags(struct journal *j, const uint8_t *block, uint32_t at, uint32_t sequence,
                          tag_visitor visit, void *arg) {
    uint32_t limit = j->block_size -
                     ((j->incompat & (INCOMPAT_CSUM_V2 | INCOMPAT_CSUM_V3)) ? 4 : 0);
    uint32_t size = tag_size(j), offset = 12, count = 0;
    while (offset + size <= limit) {
        const uint8_t *tag = block + offset;
        uint32_t flags = tag_flags(j, tag);
        count++;
        if (visit) {
            visit(j, tag_block(j, tag), flags, advance(j, at, count), sequence, arg);
        }
        offset += size + ((flags & TAG_SAME_UUID) ? 0 : 16);
        if (flags & TAG_LAST) {
            break;
        }
    }
    return count;
}

struct revoked {
    uint64_t block;
    uint32_t sequence;
};

struct revokes {
    struct revoked *list;
    uint32_t count, capacity;
};

static void add_revoke(struct revokes *r, uint64_t block, uint32_t sequence) {
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->list[i].block == block) {
            if ((int32_t)(sequence - r->list[i].sequence) > 0) {
                r->list[i].sequence = sequence;
            }
            return;
        }
    }
    if (r->count == r->capacity) {
        uint32_t capacity = r->capacity ? r->capacity * 2 : 64;
        struct revoked *list = kmalloc(capacity * sizeof(*list));
        if (!list) {
            return;
        }
        if (r->list) {
            memcpy(list, r->list, r->count * sizeof(*list));
            kfree(r->list);
        }
        r->list = list;
        r->capacity = capacity;
    }
    r->list[r->count++] = (struct revoked){block, sequence};
}

static bool is_revoked(struct revokes *r, uint64_t block, uint32_t sequence) {
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->list[i].block == block && (int32_t)(r->list[i].sequence - sequence) >= 0) {
            return true;
        }
    }
    return false;
}

struct replay {
    struct revokes *revokes;
    uint8_t *buffer;
    uint32_t written;
    int error;
};

static void replay_tag(struct journal *j, uint64_t target, uint32_t flags, uint32_t data,
                       uint32_t sequence, void *arg) {
    struct replay *r = arg;
    if (is_revoked(r->revokes, target, sequence) || target >> 32) {
        return;
    }
    if (read_journal(j, data, r->buffer)) {
        r->error = -VX_EIO;
        return;
    }
    if (flags & TAG_ESCAPE) {
        put32(r->buffer, JBD2_MAGIC);
    }
    if (block_write_bytes(j->device, target * j->block_size, r->buffer, j->block_size)) {
        r->error = -VX_EIO;
        return;
    }
    r->written++;
}

enum pass { PASS_SCAN, PASS_REVOKE, PASS_REPLAY };

/* One pass over the log from its start: returns the sequence number after
 * the last committed transaction. */
static uint32_t pass(struct journal *j, enum pass which, uint32_t end_sequence,
                     struct revokes *revokes, struct replay *replay) {
    uint8_t *block = j->scratch;
    uint32_t at = get32(j->super + SB_START), sequence = get32(j->super + SB_SEQUENCE);
    uint32_t committed = sequence;
    for (uint32_t guard = 0; guard < j->end - j->first; guard++) {
        if (which != PASS_SCAN && (int32_t)(sequence - end_sequence) >= 0) {
            break;
        }
        if (read_journal(j, at, block) || get32(block) != JBD2_MAGIC ||
            get32(block + 8) != sequence) {
            break;
        }
        uint32_t type = get32(block + 4);
        if (type == TYPE_DESCRIPTOR) {
            uint32_t n = walk_tags(j, block, at, sequence,
                                   which == PASS_REPLAY ? replay_tag : NULL, replay);
            if (which == PASS_REPLAY) {
                /* (replay_tag read its blocks into the replay buffer.) */
            }
            at = advance(j, at, n + 1);
        } else if (type == TYPE_COMMIT) {
            sequence++;
            committed = sequence;
            at = advance(j, at, 1);
        } else if (type == TYPE_REVOKE) {
            if (which == PASS_REVOKE) {
                uint32_t used = get32(block + 12), size = (j->incompat & INCOMPAT_64BIT) ? 8 : 4;
                for (uint32_t offset = 16; offset + size <= used && offset + size <= j->block_size;
                     offset += size) {
                    uint64_t target = size == 8 ? (uint64_t)get32(block + offset) << 32 |
                                                      get32(block + offset + 4)
                                                : get32(block + offset);
                    add_revoke(revokes, target, sequence);
                }
            }
            at = advance(j, at, 1);
        } else {
            break;
        }
    }
    return committed;
}

static int replay(struct journal *j, bool *replayed) {
    struct revokes revokes = {0};
    uint32_t start_sequence = get32(j->super + SB_SEQUENCE);
    uint32_t end = pass(j, PASS_SCAN, 0, NULL, NULL);
    struct replay r = {&revokes, kmalloc(j->block_size), 0, 0};
    if (!r.buffer) {
        return -VX_ENOMEM;
    }
    if (end != start_sequence) {
        /* Of the scratch buffer's two uses, the pass's is the block it reads. */
        pass(j, PASS_REVOKE, end, &revokes, NULL);
        pass(j, PASS_REPLAY, end, &revokes, &r);
        *replayed = true;
        kprintf("[ext2] journal: replayed %u transaction%s (%u blocks)\n", end - start_sequence,
                end - start_sequence == 1 ? "" : "s", r.written);
    }
    kfree(r.buffer);
    kfree(revokes.list);
    if (r.error) {
        return r.error;
    }
    /* Empty now. */
    put32(j->super + SB_SEQUENCE, end);
    put32(j->super + SB_START, 0);
    j->sequence = end;
    return write_super(j);
}

/* ---- Opening ---- */

struct journal *journal_open(struct block_device *device, uint32_t block_size, uint32_t *blocks,
                             uint32_t count, bool *replayed, int *error) {
    *replayed = false;
    struct journal *j = kzalloc(sizeof(*j));
    if (!j) {
        *error = -VX_ENOMEM;
        return NULL;
    }
    j->device = device;
    j->block_size = block_size;
    j->blocks = blocks;
    j->super = kmalloc(block_size);
    j->scratch = kmalloc(block_size);
    if (!j->super || !j->scratch || read_journal(j, 0, j->super)) {
        *error = -VX_EIO;
        goto fail;
    }
    uint32_t type = get32(j->super + 4);
    if (get32(j->super) != JBD2_MAGIC || (type != TYPE_SUPERBLOCK_V1 && type != TYPE_SUPERBLOCK_V2) ||
        get32(j->super + SB_BLOCKSIZE) != block_size) {
        kprintf("[ext2] journal: not a journal Vexa knows\n");
        *error = -VX_EINVAL;
        goto fail;
    }
    j->first = get32(j->super + SB_FIRST);
    j->end = get32(j->super + SB_MAXLEN);
    if (j->end > count) {
        j->end = count;
    }
    if (j->first < 1 || j->first + 4 > j->end) {
        kprintf("[ext2] journal: its size doesn't add up\n");
        *error = -VX_EINVAL;
        goto fail;
    }
    if (type == TYPE_SUPERBLOCK_V2) {
        j->compat = get32(j->super + SB_COMPAT);
        j->incompat = get32(j->super + SB_INCOMPAT);
        memcpy(j->uuid, j->super + SB_UUID, 16);
    }
    j->sequence = get32(j->super + SB_SEQUENCE);
    uint32_t known = INCOMPAT_REVOKE | INCOMPAT_64BIT | INCOMPAT_CSUM_V2 | INCOMPAT_CSUM_V3 |
                     INCOMPAT_ASYNC_COMMIT;
    if (j->incompat & ~known) {
        kprintf("[ext2] journal: features Vexa doesn't know (%x)\n", j->incompat & ~known);
        *error = -VX_EINVAL;
        goto fail;
    }
    if (get32(j->super + SB_START)) {
        *error = replay(j, replayed);
        if (*error) {
            goto fail;
        }
    }
    /* What Vexa writes: plain transactions, without checksums. */
    j->writable = !(j->incompat & ~INCOMPAT_REVOKE) && !(j->compat & COMPAT_CHECKSUM);
    if (!j->writable) {
        kprintf("[ext2] journal: written with features Vexa doesn't write (%x, %x)\n", j->compat,
                j->incompat);
        *error = -VX_EINVAL;
        goto fail;
    }
    *error = 0;
    return j;

fail:
    kfree(j->super);
    kfree(j->scratch);
    kfree(j);
    return NULL;
}

/* ---- Writing ---- */

uint32_t journal_capacity(struct journal *j) {
    uint32_t tags = (j->block_size - 12 - 16) / 8;
    uint32_t room = j->end - j->first - 2; /* (A descriptor and a commit block.) */
    return tags < room ? tags : room;
}

int journal_commit(struct journal *j, const uint32_t *numbers, uint8_t *const *data,
                   uint32_t count) {
    if (!count) {
        return 0;
    }
    if (count > journal_capacity(j)) {
        return -VX_EINVAL;
    }
    uint8_t *block = j->scratch;
    uint32_t sequence = j->sequence;
    /* The descriptor: which disk block each of the next ones is. */
    memset(block, 0, j->block_size);
    put32(block, JBD2_MAGIC);
    put32(block + 4, TYPE_DESCRIPTOR);
    put32(block + 8, sequence);
    uint32_t offset = 12;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t flags = (i ? TAG_SAME_UUID : 0) | (i == count - 1 ? TAG_LAST : 0) |
                         (get32(data[i]) == JBD2_MAGIC ? TAG_ESCAPE : 0);
        put32(block + offset, numbers[i]);
        block[offset + 6] = (uint8_t)(flags >> 8);
        block[offset + 7] = (uint8_t)flags;
        offset += 8;
        if (i == 0) {
            memcpy(block + offset, j->uuid, 16);
            offset += 16;
        }
    }
    int error = write_journal(j, j->first, block);
    for (uint32_t i = 0; i < count && !error; i++) {
        if (get32(data[i]) == JBD2_MAGIC) {
            /* (Escaped: a block that looks like a journal block, the magic zeroed.) */
            memcpy(block, data[i], j->block_size);
            put32(block, 0);
            error = write_journal(j, j->first + 1 + i, block);
        } else {
            error = write_journal(j, j->first + 1 + i, data[i]);
        }
    }
    if (!error) {
        memset(block, 0, j->block_size);
        put32(block, JBD2_MAGIC);
        put32(block + 4, TYPE_COMMIT);
        put32(block + 8, sequence);
        error = write_journal(j, j->first + 1 + count, block);
    }
    if (!error) {
        /* Committed: from here on, a crash replays it. */
        put32(j->super + SB_SEQUENCE, sequence);
        put32(j->super + SB_START, j->first);
        error = write_super(j);
    }
    if (error) {
        return error;
    }
    for (uint32_t i = 0; i < count; i++) {
        int e = block_write_bytes(j->device, (uint64_t)numbers[i] * j->block_size, data[i],
                                  j->block_size);
        error = error ? error : e;
    }
    if (error) {
        return error; /* (Left in the journal: the next mount replays it.) */
    }
    put32(j->super + SB_SEQUENCE, sequence + 1);
    put32(j->super + SB_START, 0);
    j->sequence = sequence + 1;
    return write_super(j);
}
