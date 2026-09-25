#include <stdbool.h>
#include <vexa/block.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * ext2, read and write.
 *
 * The classic Linux file system: disks made by `mke2fs -t ext2` on Linux work,
 * and Linux's e2fsck is happy with what Vexa writes. Supported: block sizes of
 * 1, 2 and 4 KiB, direct and (double, triple) indirect blocks, files over
 * 4 GiB, typed directory entries. Unknown required features refuse the mount;
 * unknown "read-only compatible" ones make it read-only.
 *
 * Everything runs under the VFS lock and writes go straight to the disk, so
 * the file system on disk is always complete (though not crash-proof: that
 * needs a journal, which ext2 doesn't have).
 */

#define SUPERBLOCK_OFFSET 1024
#define EXT2_MAGIC 0xef53
#define ROOT_INODE 2

#define INCOMPAT_FILETYPE 0x0002
#define RO_COMPAT_SPARSE_SUPER 0x0001
#define RO_COMPAT_LARGE_FILE 0x0002
#define SUPPORTED_INCOMPAT INCOMPAT_FILETYPE
#define SUPPORTED_RO_COMPAT (RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE)

#define MODE_TYPE_MASK 0xf000
#define MODE_FIFO 0x1000
#define MODE_CHAR 0x2000
#define MODE_DIR 0x4000
#define MODE_BLOCK 0x6000
#define MODE_FILE 0x8000
#define MODE_SYMLINK 0xa000
#define MODE_SOCKET 0xc000

#define FLAG_INDEX 0x1000 /* Hashed directory index: we don't maintain it. */

#define DIRECT_BLOCKS 12

struct __attribute__((packed)) superblock {
    uint32_t inodes_count, blocks_count, reserved_blocks_count, free_blocks_count;
    uint32_t free_inodes_count, first_data_block, log_block_size, log_frag_size;
    uint32_t blocks_per_group, frags_per_group, inodes_per_group, mount_time, write_time;
    uint16_t mount_count, max_mount_count, magic, state, errors, minor_rev_level;
    uint32_t last_check, check_interval, creator_os, rev_level;
    uint16_t default_resuid, default_resgid;
    uint32_t first_inode;
    uint16_t inode_size, block_group_number;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t rest[1024 - 104];
};

struct __attribute__((packed)) group_descriptor {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks_count, free_inodes_count, used_dirs_count, padding;
    uint8_t reserved[12];
};

struct inode { /* Naturally aligned; the size check below guards the layout. */
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links_count;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, fragment_address;
    uint8_t osd2[12];
};

struct __attribute__((packed)) dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len, file_type;
    char name[];
};

_Static_assert(sizeof(struct superblock) == 1024, "superblock layout");
_Static_assert(sizeof(struct group_descriptor) == 32, "group descriptor layout");
_Static_assert(sizeof(struct inode) == 128, "inode layout");

struct ext2 {
    struct block_device *device;
    struct mount *mount;
    struct superblock sb;
    struct group_descriptor *groups;
    uint32_t group_count;
    uint32_t block_size;
    uint32_t inode_size;
    bool file_types; /* Directory entries carry the file type. */
    struct ext2_node *open_nodes;
};

struct ext2_node {
    struct vnode vnode;
    uint32_t number;
    struct inode inode;
    bool free_on_release; /* Unlinked while open: free it when closed. */
    struct ext2_node *next;
};

static const struct vnode_ops ext2_ops;

static struct ext2 *fs_of(struct vnode *vnode) {
    return vnode->mount->data;
}

static struct ext2_node *node_of(struct vnode *vnode) {
    return (struct ext2_node *)vnode;
}

/* ---- Raw access ---- */

static int read_block(struct ext2 *fs, uint32_t block, void *buffer) {
    return block_read_bytes(fs->device, (uint64_t)block * fs->block_size, buffer, fs->block_size);
}

static int write_block(struct ext2 *fs, uint32_t block, const void *buffer) {
    return block_write_bytes(fs->device, (uint64_t)block * fs->block_size, buffer, fs->block_size);
}

static int write_superblock(struct ext2 *fs) {
    fs->sb.write_time = (uint32_t)time_now();
    return block_write_bytes(fs->device, SUPERBLOCK_OFFSET, &fs->sb, sizeof(fs->sb));
}

static int write_group(struct ext2 *fs, uint32_t group) {
    uint64_t table = (uint64_t)(fs->sb.first_data_block + 1) * fs->block_size;
    return block_write_bytes(fs->device, table + group * sizeof(struct group_descriptor),
                             &fs->groups[group], sizeof(struct group_descriptor));
}

static uint64_t inode_offset(struct ext2 *fs, uint32_t number) {
    uint32_t group = (number - 1) / fs->sb.inodes_per_group;
    uint32_t index = (number - 1) % fs->sb.inodes_per_group;
    return (uint64_t)fs->groups[group].inode_table * fs->block_size + (uint64_t)index * fs->inode_size;
}

static int read_inode(struct ext2 *fs, uint32_t number, struct inode *inode) {
    if (number == 0 || number > fs->sb.inodes_count) {
        return -VX_EIO;
    }
    return block_read_bytes(fs->device, inode_offset(fs, number), inode, sizeof(*inode));
}

static int write_inode(struct ext2 *fs, struct ext2_node *node) {
    return block_write_bytes(fs->device, inode_offset(fs, node->number), &node->inode,
                             sizeof(node->inode));
}

static uint64_t inode_size(const struct inode *inode) {
    uint64_t size = inode->size;
    if ((inode->mode & MODE_TYPE_MASK) == MODE_FILE) {
        size |= (uint64_t)inode->size_high << 32;
    }
    return size;
}

static void set_size(struct ext2_node *node, uint64_t size) {
    node->inode.size = (uint32_t)size;
    if ((node->inode.mode & MODE_TYPE_MASK) == MODE_FILE) {
        node->inode.size_high = (uint32_t)(size >> 32);
    }
    node->vnode.size = size;
}

/* ---- Allocation bitmaps ---- */

/* Finds and sets a clear bit in a bitmap block. Returns the bit or -1. */
static int take_bit(struct ext2 *fs, uint32_t bitmap_block, uint32_t limit, uint32_t first) {
    uint8_t *bitmap = kmalloc(fs->block_size);
    if (!bitmap || read_block(fs, bitmap_block, bitmap)) {
        kfree(bitmap);
        return -1;
    }
    int found = -1;
    for (uint32_t bit = first; bit < limit; bit++) {
        if (!(bitmap[bit / 8] & (1 << (bit % 8)))) {
            bitmap[bit / 8] |= 1 << (bit % 8);
            found = write_block(fs, bitmap_block, bitmap) ? -1 : (int)bit;
            break;
        }
    }
    kfree(bitmap);
    return found;
}

static int clear_bit(struct ext2 *fs, uint32_t bitmap_block, uint32_t bit) {
    uint8_t *bitmap = kmalloc(fs->block_size);
    int error = !bitmap || read_block(fs, bitmap_block, bitmap) ? -VX_EIO : 0;
    if (!error) {
        bitmap[bit / 8] &= ~(1 << (bit % 8));
        error = write_block(fs, bitmap_block, bitmap);
    }
    kfree(bitmap);
    return error;
}

static uint32_t blocks_in_group(struct ext2 *fs, uint32_t group) {
    uint32_t total = fs->sb.blocks_count - fs->sb.first_data_block;
    uint32_t start = group * fs->sb.blocks_per_group;
    return total - start < fs->sb.blocks_per_group ? total - start : fs->sb.blocks_per_group;
}

/* Allocates a zeroed block, preferring `near`'s group. Returns 0 if full. */
static uint32_t alloc_block(struct ext2 *fs, uint32_t near) {
    uint32_t start = near > fs->sb.first_data_block
                         ? (near - fs->sb.first_data_block) / fs->sb.blocks_per_group
                         : 0;
    for (uint32_t i = 0; i < fs->group_count; i++) {
        uint32_t group = (start + i) % fs->group_count;
        if (fs->groups[group].free_blocks_count == 0) {
            continue;
        }
        int bit = take_bit(fs, fs->groups[group].block_bitmap, blocks_in_group(fs, group), 0);
        if (bit < 0) {
            continue;
        }
        fs->groups[group].free_blocks_count--;
        fs->sb.free_blocks_count--;
        write_group(fs, group);
        write_superblock(fs);
        uint32_t block = fs->sb.first_data_block + group * fs->sb.blocks_per_group + bit;
        uint8_t *zeros = kzalloc(fs->block_size);
        if (zeros) {
            write_block(fs, block, zeros);
            kfree(zeros);
        }
        return block;
    }
    return 0;
}

static void free_block(struct ext2 *fs, uint32_t block) {
    if (block < fs->sb.first_data_block || block >= fs->sb.blocks_count) {
        kprintf("[ext2] refusing to free out-of-range block %u\n", block);
        return;
    }
    uint32_t group = (block - fs->sb.first_data_block) / fs->sb.blocks_per_group;
    uint32_t bit = (block - fs->sb.first_data_block) % fs->sb.blocks_per_group;
    if (clear_bit(fs, fs->groups[group].block_bitmap, bit) == 0) {
        fs->groups[group].free_blocks_count++;
        fs->sb.free_blocks_count++;
        write_group(fs, group);
        write_superblock(fs);
    }
}

static uint32_t alloc_inode(struct ext2 *fs, uint32_t near, bool directory) {
    uint32_t start = near ? (near - 1) / fs->sb.inodes_per_group : 0;
    for (uint32_t i = 0; i < fs->group_count; i++) {
        uint32_t group = (start + i) % fs->group_count;
        if (fs->groups[group].free_inodes_count == 0) {
            continue;
        }
        /* Inodes below first_inode are reserved (they live in group 0). */
        uint32_t first = group == 0 ? fs->sb.first_inode - 1 : 0;
        int bit = take_bit(fs, fs->groups[group].inode_bitmap, fs->sb.inodes_per_group, first);
        if (bit < 0) {
            continue;
        }
        fs->groups[group].free_inodes_count--;
        fs->sb.free_inodes_count--;
        if (directory) {
            fs->groups[group].used_dirs_count++;
        }
        write_group(fs, group);
        write_superblock(fs);
        return group * fs->sb.inodes_per_group + bit + 1;
    }
    return 0;
}

static void free_inode(struct ext2 *fs, uint32_t number, bool directory) {
    uint32_t group = (number - 1) / fs->sb.inodes_per_group;
    if (clear_bit(fs, fs->groups[group].inode_bitmap, (number - 1) % fs->sb.inodes_per_group) == 0) {
        fs->groups[group].free_inodes_count++;
        fs->sb.free_inodes_count++;
        if (directory) {
            fs->groups[group].used_dirs_count--;
        }
        write_group(fs, group);
        write_superblock(fs);
    }
}

/* ---- Block maps: file block number -> disk block ---- */

static uint32_t read_pointer(struct ext2 *fs, uint32_t block, uint32_t index) {
    uint32_t value = 0;
    block_read_bytes(fs->device, (uint64_t)block * fs->block_size + index * 4, &value, 4);
    return value;
}

static int write_pointer(struct ext2 *fs, uint32_t block, uint32_t index, uint32_t value) {
    return block_write_bytes(fs->device, (uint64_t)block * fs->block_size + index * 4, &value, 4);
}

/* Returns the disk block holding file block `logical`, or 0 for a hole. With
 * `create`, allocates missing blocks (and indirect blocks) on the way. */
static uint32_t map_block(struct ext2 *fs, struct ext2_node *node, uint64_t logical, bool create) {
    uint32_t per_block = fs->block_size / 4;
    uint32_t *slot;
    int levels;
    if (logical < DIRECT_BLOCKS) {
        slot = &node->inode.block[logical];
        levels = 0;
    } else if ((logical -= DIRECT_BLOCKS) < per_block) {
        slot = &node->inode.block[12];
        levels = 1;
    } else if ((logical -= per_block) < (uint64_t)per_block * per_block) {
        slot = &node->inode.block[13];
        levels = 2;
    } else if ((logical -= (uint64_t)per_block * per_block) <
               (uint64_t)per_block * per_block * per_block) {
        slot = &node->inode.block[14];
        levels = 3;
    } else {
        return 0; /* Beyond what ext2 can address. */
    }

    bool inode_changed = false;
    if (!*slot) {
        if (!create || !(*slot = alloc_block(fs, node->inode.block[0]))) {
            return 0;
        }
        node->inode.blocks += fs->block_size / 512;
        inode_changed = true;
    }
    uint32_t block = *slot;
    for (int level = levels; level > 0; level--) {
        uint64_t span = 1;
        for (int i = 1; i < level; i++) {
            span *= per_block;
        }
        uint32_t index = (uint32_t)(logical / span);
        logical %= span;
        uint32_t next = read_pointer(fs, block, index);
        if (!next) {
            if (!create || !(next = alloc_block(fs, block))) {
                block = 0;
                break;
            }
            write_pointer(fs, block, index, next);
            node->inode.blocks += fs->block_size / 512;
            inode_changed = true;
        }
        block = next;
    }
    if (inode_changed) {
        write_inode(fs, node);
    }
    return block;
}

/* Frees the blocks under an indirect block that hold file blocks >= keep
 * (relative to the start of this subtree). Returns true if it freed them all
 * (so the indirect block itself can go too). */
static bool free_tree(struct ext2 *fs, struct ext2_node *node, uint32_t block, int level,
                      uint64_t keep) {
    uint32_t per_block = fs->block_size / 4;
    uint64_t span = 1;
    for (int i = 1; i < level; i++) {
        span *= per_block;
    }
    bool all = true;
    for (uint32_t i = 0; i < per_block; i++) {
        uint32_t child = read_pointer(fs, block, i);
        uint64_t first = (uint64_t)i * span;
        if (!child) {
            continue;
        }
        if (first + span <= keep) {
            all = false; /* Entirely kept. */
            continue;
        }
        bool freed = level == 1 || free_tree(fs, node, child, level - 1, first >= keep ? 0 : keep - first);
        if (freed) {
            free_block(fs, child);
            node->inode.blocks -= fs->block_size / 512;
            write_pointer(fs, block, i, 0);
        } else {
            all = false;
        }
    }
    return all;
}

/* Frees every block from file block `keep` on. */
/* A "fast" symbolic link keeps its target in the block pointers, not in blocks. */
static bool is_fast_symlink(const struct ext2_node *node) {
    return (node->inode.mode & MODE_TYPE_MASK) == MODE_SYMLINK && node->inode.blocks == 0;
}

static void free_blocks_from(struct ext2 *fs, struct ext2_node *node, uint64_t keep) {
    if (is_fast_symlink(node)) {
        if (keep == 0) {
            memset(node->inode.block, 0, sizeof(node->inode.block));
            write_inode(fs, node);
        }
        return;
    }
    for (uint32_t i = 0; i < DIRECT_BLOCKS; i++) {
        if (i >= keep && node->inode.block[i]) {
            free_block(fs, node->inode.block[i]);
            node->inode.blocks -= fs->block_size / 512;
            node->inode.block[i] = 0;
        }
    }
    uint32_t per_block = fs->block_size / 4;
    uint64_t start = DIRECT_BLOCKS, span = per_block;
    for (int level = 1; level <= 3; level++) {
        uint32_t *slot = &node->inode.block[11 + level];
        if (*slot && start + span > keep &&
            free_tree(fs, node, *slot, level, keep > start ? keep - start : 0)) {
            free_block(fs, *slot);
            node->inode.blocks -= fs->block_size / 512;
            *slot = 0;
        }
        start += span;
        span *= per_block;
    }
    write_inode(fs, node);
}

/* ---- Vnodes ---- */

static uint32_t vnode_type(uint16_t mode) {
    switch (mode & MODE_TYPE_MASK) {
    case MODE_DIR: return VX_TYPE_DIRECTORY;
    case MODE_SYMLINK: return VX_TYPE_SYMLINK;
    case MODE_SOCKET: return VX_TYPE_SOCKET;
    case MODE_CHAR: return VX_TYPE_CHAR_DEVICE;
    case MODE_BLOCK: return VX_TYPE_BLOCK_DEVICE;
    default: return VX_TYPE_FILE;
    }
}

/* Returns the vnode for an inode, sharing it if it's already open. */
static int get_node(struct ext2 *fs, uint32_t number, struct vnode **out) {
    for (struct ext2_node *node = fs->open_nodes; node; node = node->next) {
        if (node->number == number) {
            vnode_ref(&node->vnode);
            *out = &node->vnode;
            return 0;
        }
    }
    struct ext2_node *node = kzalloc(sizeof(*node));
    if (!node) {
        return -VX_ENOMEM;
    }
    if (read_inode(fs, number, &node->inode)) {
        kfree(node);
        return -VX_EIO;
    }
    vnode_init(&node->vnode, fs->mount, vnode_type(node->inode.mode), &ext2_ops);
    node->number = number;
    node->vnode.inode = number;
    node->vnode.size = inode_size(&node->inode);
    node->vnode.links = node->inode.links_count;
    node->vnode.mode = node->inode.mode & 07777;
    node->vnode.modified = node->inode.mtime;
    node->vnode.data = fs;
    node->next = fs->open_nodes;
    fs->open_nodes = node;
    *out = &node->vnode;
    return 0;
}

static void destroy_inode(struct ext2 *fs, struct ext2_node *node) {
    bool directory = (node->inode.mode & MODE_TYPE_MASK) == MODE_DIR;
    free_blocks_from(fs, node, 0);
    node->inode.dtime = (uint32_t)time_now();
    node->inode.links_count = 0;
    set_size(node, 0);
    write_inode(fs, node);
    free_inode(fs, node->number, directory);
}

static void ext2_release(struct vnode *vnode) {
    struct ext2 *fs = fs_of(vnode);
    struct ext2_node *node = node_of(vnode);
    for (struct ext2_node **link = &fs->open_nodes; *link; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            break;
        }
    }
    if (node->free_on_release) {
        destroy_inode(fs, node);
    }
    kfree(node);
}

/* ---- Directories ---- */

static uint32_t entry_size(size_t name_length) {
    return (8 + name_length + 3) & ~3U;
}

/* Calls `visit` for each entry; stops early when it returns true. Passes the
 * block buffer so `visit` can modify it and set *dirty. */
typedef bool (*entry_visitor)(struct dir_entry *entry, struct dir_entry *previous,
                              uint8_t *block, bool *dirty, void *arg);

static int walk_directory(struct ext2 *fs, struct ext2_node *dir, entry_visitor visit, void *arg,
                          bool *stopped) {
    uint8_t *buffer = kmalloc(fs->block_size);
    if (!buffer) {
        return -VX_ENOMEM;
    }
    *stopped = false;
    uint64_t blocks = dir->vnode.size / fs->block_size;
    int error = 0;
    for (uint64_t b = 0; b < blocks && !*stopped && !error; b++) {
        uint32_t disk_block = map_block(fs, dir, b, false);
        if (!disk_block || read_block(fs, disk_block, buffer)) {
            continue;
        }
        bool dirty = false;
        struct dir_entry *previous = NULL;
        for (uint32_t offset = 0; offset + 8 <= fs->block_size;) {
            struct dir_entry *entry = (void *)(buffer + offset);
            if (entry->rec_len < 8 || entry->rec_len % 4 || offset + entry->rec_len > fs->block_size) {
                kprintf("[ext2] corrupt directory entry in inode %u\n", dir->number);
                break;
            }
            if (visit(entry, previous, buffer, &dirty, arg)) {
                *stopped = true;
                break;
            }
            previous = entry;
            offset += entry->rec_len;
        }
        if (dirty) {
            error = write_block(fs, disk_block, buffer);
        }
    }
    kfree(buffer);
    return error;
}

struct find_args {
    const char *name;
    size_t length;
    uint32_t found;
    uint8_t file_type;
    bool remove;
};

static bool find_visitor(struct dir_entry *entry, struct dir_entry *previous, uint8_t *block,
                         bool *dirty, void *arg) {
    (void)block;
    struct find_args *find = arg;
    if (!entry->inode || entry->name_len != find->length ||
        memcmp(entry->name, find->name, find->length) != 0) {
        return false;
    }
    find->found = entry->inode;
    find->file_type = entry->file_type;
    if (find->remove) {
        if (previous) {
            previous->rec_len += entry->rec_len; /* Fold it into the one before. */
        } else {
            entry->inode = 0; /* First in its block: just mark it unused. */
        }
        *dirty = true;
    }
    return true;
}

static int ext2_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct ext2 *fs = fs_of(dir);
    struct find_args find = {.name = name, .length = length};
    bool stopped;
    int error = walk_directory(fs, node_of(dir), find_visitor, &find, &stopped);
    if (error) {
        return error;
    }
    if (!find.found) {
        return -VX_ENOENT;
    }
    return get_node(fs, find.found, out);
}

struct add_args {
    const char *name;
    size_t length;
    uint32_t inode;
    uint8_t file_type;
    bool done;
};

static bool add_visitor(struct dir_entry *entry, struct dir_entry *previous, uint8_t *block,
                        bool *dirty, void *arg) {
    (void)previous;
    (void)block;
    struct add_args *add = arg;
    uint32_t needed = entry_size(add->length);
    uint32_t used = entry->inode ? entry_size(entry->name_len) : 0;
    if (entry->rec_len - used < needed) {
        return false;
    }
    struct dir_entry *target = entry;
    if (used) { /* Split the free space off the end of this entry. */
        target = (void *)((uint8_t *)entry + used);
        target->rec_len = entry->rec_len - used;
        entry->rec_len = used;
    }
    target->inode = add->inode;
    target->name_len = (uint8_t)add->length;
    target->file_type = add->file_type;
    memcpy(target->name, add->name, add->length);
    *dirty = true;
    add->done = true;
    return true;
}

static uint8_t entry_file_type(struct ext2 *fs, uint32_t vx_type) {
    if (!fs->file_types) {
        return 0;
    }
    return vx_type == VX_TYPE_DIRECTORY ? 2 : vx_type == VX_TYPE_SYMLINK ? 7
                                             : vx_type == VX_TYPE_SOCKET  ? 6 : 1;
}

static int add_entry(struct ext2 *fs, struct ext2_node *dir, const char *name, size_t length,
                     uint32_t inode, uint32_t vx_type) {
    struct add_args add = {.name = name, .length = length, .inode = inode,
                           .file_type = entry_file_type(fs, vx_type)};
    bool stopped;
    int error = walk_directory(fs, dir, add_visitor, &add, &stopped);
    if (!error && !add.done) {
        /* No room: give the directory another block holding just this entry. */
        uint64_t logical = dir->vnode.size / fs->block_size;
        uint32_t block = map_block(fs, dir, logical, true);
        uint8_t *buffer = block ? kzalloc(fs->block_size) : NULL;
        if (!buffer) {
            return -VX_ENOSPC;
        }
        struct dir_entry *entry = (void *)buffer;
        entry->inode = inode;
        entry->rec_len = (uint16_t)fs->block_size;
        entry->name_len = (uint8_t)length;
        entry->file_type = add.file_type;
        memcpy(entry->name, name, length);
        error = write_block(fs, block, buffer);
        kfree(buffer);
        set_size(dir, dir->vnode.size + fs->block_size);
    }
    if (!error) {
        dir->inode.flags &= ~FLAG_INDEX; /* Linux rebuilds the index if it wants one. */
        dir->inode.mtime = dir->inode.ctime = (uint32_t)time_now();
        dir->vnode.modified = dir->inode.mtime;
        error = write_inode(fs, dir);
    }
    return error;
}

struct read_dir_args {
    uint64_t skip; /* Entries before the cookie. */
    uint64_t index;
    struct dir_entry *found;
    struct vx_dir_entry *out;
};

static bool read_dir_visitor(struct dir_entry *entry, struct dir_entry *previous, uint8_t *block,
                             bool *dirty, void *arg) {
    (void)previous;
    (void)block;
    (void)dirty;
    struct read_dir_args *args = arg;
    if (!entry->inode) {
        return false;
    }
    if (args->index++ < args->skip) {
        return false;
    }
    args->out->inode = entry->inode;
    args->out->type = entry->file_type == 2 ? VX_TYPE_DIRECTORY
                      : entry->file_type == 7 ? VX_TYPE_SYMLINK
                      : entry->file_type == 3 ? VX_TYPE_CHAR_DEVICE
                      : entry->file_type == 4 ? VX_TYPE_BLOCK_DEVICE
                      : entry->file_type == 6 ? VX_TYPE_SOCKET
                                              : VX_TYPE_FILE;
    args->out->name_length = entry->name_len;
    memcpy(args->out->name, entry->name, entry->name_len);
    args->out->name[entry->name_len] = '\0';
    args->found = entry;
    return true;
}

static int ext2_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *out) {
    struct ext2 *fs = fs_of(dir);
    struct read_dir_args args = {.skip = *cookie, .out = out};
    bool stopped;
    int error = walk_directory(fs, node_of(dir), read_dir_visitor, &args, &stopped);
    if (error) {
        return error;
    }
    if (!args.found) {
        return 0;
    }
    if (!fs->file_types) {
        struct inode inode;
        if (read_inode(fs, (uint32_t)out->inode, &inode) == 0) {
            out->type = vnode_type(inode.mode);
        }
    }
    (*cookie)++;
    return 1;
}

static bool empty_visitor(struct dir_entry *entry, struct dir_entry *previous, uint8_t *block,
                          bool *dirty, void *arg) {
    (void)previous;
    (void)block;
    (void)dirty;
    bool dot = entry->name_len == 1 && entry->name[0] == '.';
    bool dotdot = entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.';
    if (entry->inode && !dot && !dotdot) {
        *(bool *)arg = false;
        return true;
    }
    return false;
}

static int ext2_create(struct vnode *dir_vnode, const char *name, size_t length, uint32_t type,
                       struct vnode **out) {
    struct ext2 *fs = fs_of(dir_vnode);
    struct ext2_node *dir = node_of(dir_vnode);
    bool directory = type == VX_TYPE_DIRECTORY;
    uint32_t number = alloc_inode(fs, dir->number, directory);
    if (!number) {
        return -VX_ENOSPC;
    }
    uint32_t now = (uint32_t)time_now();
    struct ext2_node *node = kzalloc(sizeof(*node));
    if (!node) {
        free_inode(fs, number, directory);
        return -VX_ENOMEM;
    }
    node->number = number;
    node->inode.mode = directory                   ? (MODE_DIR | 0755)
                       : type == VX_TYPE_SYMLINK ? (MODE_SYMLINK | 0777)
                       : type == VX_TYPE_SOCKET  ? (MODE_SOCKET | 0755)
                                                 : (MODE_FILE | 0644);
    node->inode.atime = node->inode.ctime = node->inode.mtime = now;
    node->inode.links_count = directory ? 2 : 1;
    vnode_init(&node->vnode, fs->mount, type, &ext2_ops);
    node->vnode.inode = number;
    node->vnode.data = fs;
    node->vnode.modified = now;

    int error = 0;
    if (directory) {
        uint32_t block = map_block(fs, node, 0, true);
        uint8_t *buffer = block ? kzalloc(fs->block_size) : NULL;
        if (!buffer) {
            error = -VX_ENOSPC;
        } else {
            struct dir_entry *dot = (void *)buffer;
            dot->inode = number;
            dot->rec_len = 12;
            dot->name_len = 1;
            dot->file_type = entry_file_type(fs, VX_TYPE_DIRECTORY);
            dot->name[0] = '.';
            struct dir_entry *dotdot = (void *)(buffer + 12);
            dotdot->inode = dir->number;
            dotdot->rec_len = (uint16_t)(fs->block_size - 12);
            dotdot->name_len = 2;
            dotdot->file_type = dot->file_type;
            dotdot->name[0] = dotdot->name[1] = '.';
            error = write_block(fs, block, buffer);
            kfree(buffer);
            set_size(node, fs->block_size);
        }
    }
    if (!error) {
        node->vnode.links = node->inode.links_count;
        error = write_inode(fs, node);
    }
    if (!error) {
        error = add_entry(fs, dir, name, length, number, type);
    }
    if (error) {
        destroy_inode(fs, node);
        kfree(node);
        return error;
    }
    if (directory) {
        dir->inode.links_count++; /* The new directory's "..". */
        dir->vnode.links = dir->inode.links_count;
        write_inode(fs, dir);
    }
    node->next = fs->open_nodes;
    fs->open_nodes = node;
    *out = &node->vnode;
    return 0;
}

static int ext2_link(struct vnode *dir_vnode, const char *name, size_t length,
                     struct vnode *target) {
    struct ext2 *fs = fs_of(dir_vnode);
    struct ext2_node *node = node_of(target);
    int error = add_entry(fs, node_of(dir_vnode), name, length, node->number, target->type);
    if (!error) {
        node->inode.links_count++;
        node->inode.ctime = (uint32_t)time_now();
        target->links = node->inode.links_count;
        error = write_inode(fs, node);
    }
    return error;
}

static int ext2_remove(struct vnode *dir_vnode, const char *name, size_t length) {
    struct ext2 *fs = fs_of(dir_vnode);
    struct ext2_node *dir = node_of(dir_vnode);
    struct vnode *vnode;
    int error = ext2_lookup(dir_vnode, name, length, &vnode);
    if (error) {
        return error;
    }
    struct ext2_node *node = node_of(vnode);
    bool directory = vnode->type == VX_TYPE_DIRECTORY;
    if (directory) {
        bool empty = true, stopped;
        walk_directory(fs, node, empty_visitor, &empty, &stopped);
        if (!empty) {
            vnode_put(vnode);
            return -VX_ENOTEMPTY;
        }
    }
    struct find_args find = {.name = name, .length = length, .remove = true};
    bool stopped;
    error = walk_directory(fs, dir, find_visitor, &find, &stopped);
    if (error) {
        vnode_put(vnode);
        return error;
    }
    uint32_t now = (uint32_t)time_now();
    dir->inode.mtime = dir->inode.ctime = now;
    dir->vnode.modified = now;
    if (directory) {
        dir->inode.links_count--; /* Its ".." pointed at us. */
        dir->vnode.links = dir->inode.links_count;
        node->inode.links_count = 0;
    } else if (node->inode.links_count > 0) {
        node->inode.links_count--;
    }
    write_inode(fs, dir);
    node->vnode.links = node->inode.links_count;
    node->inode.ctime = now;
    write_inode(fs, node);
    if (node->inode.links_count == 0) {
        node->free_on_release = true; /* Freed once nobody has it open. */
    }
    vnode_put(vnode);
    return 0;
}

struct dotdot_args {
    uint32_t parent;
};

static bool dotdot_visitor(struct dir_entry *entry, struct dir_entry *previous, uint8_t *block,
                           bool *dirty, void *arg) {
    (void)previous;
    (void)block;
    if (entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.') {
        entry->inode = ((struct dotdot_args *)arg)->parent;
        *dirty = true;
        return true;
    }
    return false;
}

static int ext2_rename(struct vnode *old_dir_vnode, const char *old_name, size_t old_length,
                       struct vnode *new_dir_vnode, const char *new_name, size_t new_length) {
    struct ext2 *fs = fs_of(old_dir_vnode);
    struct ext2_node *old_dir = node_of(old_dir_vnode), *new_dir = node_of(new_dir_vnode);
    struct vnode *moving_vnode;
    int error = ext2_lookup(old_dir_vnode, old_name, old_length, &moving_vnode);
    if (error) {
        return error;
    }
    struct ext2_node *moving = node_of(moving_vnode);
    bool directory = moving_vnode->type == VX_TYPE_DIRECTORY;

    struct vnode *existing;
    if (ext2_lookup(new_dir_vnode, new_name, new_length, &existing) == 0) {
        bool same = existing == moving_vnode;
        bool existing_dir = existing->type == VX_TYPE_DIRECTORY;
        vnode_put(existing);
        if (same) {
            vnode_put(moving_vnode);
            return 0;
        }
        error = existing_dir && !directory ? -VX_EISDIR
                : !existing_dir && directory ? -VX_ENOTDIR
                                             : ext2_remove(new_dir_vnode, new_name, new_length);
        if (error) {
            vnode_put(moving_vnode);
            return error;
        }
    }
    /* Link under the new name first, then drop the old one: an interruption
     * leaves an extra name rather than a lost file. */
    error = add_entry(fs, new_dir, new_name, new_length, moving->number, moving_vnode->type);
    if (!error) {
        struct find_args find = {.name = old_name, .length = old_length, .remove = true};
        bool stopped;
        error = walk_directory(fs, old_dir, find_visitor, &find, &stopped);
    }
    if (!error && directory && old_dir != new_dir) {
        struct dotdot_args args = {new_dir->number};
        bool stopped;
        walk_directory(fs, moving, dotdot_visitor, &args, &stopped);
        old_dir->inode.links_count--;
        old_dir->vnode.links = old_dir->inode.links_count;
        new_dir->inode.links_count++;
        new_dir->vnode.links = new_dir->inode.links_count;
        write_inode(fs, new_dir);
    }
    if (!error) {
        uint32_t now = (uint32_t)time_now();
        old_dir->inode.mtime = old_dir->inode.ctime = now;
        old_dir->vnode.modified = now;
        write_inode(fs, old_dir);
        moving->inode.ctime = now;
        write_inode(fs, moving);
    }
    vnode_put(moving_vnode);
    return error;
}

/* ---- Files ---- */

static int64_t ext2_read(struct vnode *vnode, void *buffer, size_t size, uint64_t offset) {
    struct ext2 *fs = fs_of(vnode);
    struct ext2_node *node = node_of(vnode);
    if (offset >= vnode->size) {
        return 0;
    }
    if (size > vnode->size - offset) {
        size = vnode->size - offset;
    }
    if (is_fast_symlink(node)) {
        memcpy(buffer, (uint8_t *)node->inode.block + offset, size); /* A "fast" symlink. */
        return (int64_t)size;
    }
    for (size_t done = 0; done < size;) {
        uint64_t logical = (offset + done) / fs->block_size;
        uint32_t within = (offset + done) % fs->block_size;
        size_t n = fs->block_size - within < size - done ? fs->block_size - within : size - done;
        uint32_t block = map_block(fs, node, logical, false);
        if (!block) {
            memset((uint8_t *)buffer + done, 0, n); /* A hole. */
        } else if (block_read_bytes(fs->device, (uint64_t)block * fs->block_size + within,
                                    (uint8_t *)buffer + done, n)) {
            return done ? (int64_t)done : -VX_EIO;
        }
        done += n;
    }
    return (int64_t)size;
}

static int64_t ext2_write(struct vnode *vnode, const void *buffer, size_t size, uint64_t offset) {
    struct ext2 *fs = fs_of(vnode);
    struct ext2_node *node = node_of(vnode);
    if (vnode->type == VX_TYPE_SYMLINK && offset == 0 && vnode->size == 0 &&
        size < sizeof(node->inode.block)) {
        /* A short link target fits in the inode itself (a "fast" symlink). */
        memcpy(node->inode.block, buffer, size);
        set_size(node, size);
        node->inode.mtime = node->inode.ctime = (uint32_t)time_now();
        vnode->modified = node->inode.mtime;
        write_inode(fs, node);
        return (int64_t)size;
    }
    size_t done = 0;
    int error = 0;
    while (done < size) {
        uint64_t logical = (offset + done) / fs->block_size;
        uint32_t within = (offset + done) % fs->block_size;
        size_t n = fs->block_size - within < size - done ? fs->block_size - within : size - done;
        uint32_t block = map_block(fs, node, logical, true);
        if (!block) {
            error = -VX_ENOSPC;
            break;
        }
        if (block_write_bytes(fs->device, (uint64_t)block * fs->block_size + within,
                              (const uint8_t *)buffer + done, n)) {
            error = -VX_EIO;
            break;
        }
        done += n;
    }
    if (offset + done > vnode->size) {
        set_size(node, offset + done);
    }
    node->inode.mtime = node->inode.ctime = (uint32_t)time_now();
    vnode->modified = node->inode.mtime;
    write_inode(fs, node);
    return done ? (int64_t)done : error;
}

static int ext2_truncate(struct vnode *vnode, uint64_t size) {
    struct ext2 *fs = fs_of(vnode);
    struct ext2_node *node = node_of(vnode);
    if (size < vnode->size) {
        free_blocks_from(fs, node, (size + fs->block_size - 1) / fs->block_size);
        /* Zero the rest of a partly kept last block, so growing later reads zeros. */
        uint32_t within = size % fs->block_size;
        uint32_t block = within ? map_block(fs, node, size / fs->block_size, false) : 0;
        if (block) {
            uint8_t *zeros = kzalloc(fs->block_size - within);
            if (zeros) {
                block_write_bytes(fs->device, (uint64_t)block * fs->block_size + within, zeros,
                                  fs->block_size - within);
                kfree(zeros);
            }
        }
    }
    set_size(node, size);
    node->inode.mtime = node->inode.ctime = (uint32_t)time_now();
    vnode->modified = node->inode.mtime;
    return write_inode(fs, node);
}

static int ext2_set_mode(struct vnode *vnode) {
    struct ext2_node *node = node_of(vnode);
    node->inode.mode = (uint16_t)((node->inode.mode & MODE_TYPE_MASK) | (vnode->mode & 07777));
    node->inode.ctime = (uint32_t)time_now();
    return write_inode(fs_of(vnode), node);
}

static void ext2_statfs(struct mount *mount, uint64_t *total, uint64_t *free) {
    struct ext2 *fs = mount->data;
    *total = (uint64_t)fs->sb.blocks_count * fs->block_size;
    *free = (uint64_t)fs->sb.free_blocks_count * fs->block_size;
}

static const struct vnode_ops ext2_ops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .remove = ext2_remove,
    .link = ext2_link,
    .rename = ext2_rename,
    .read_dir = ext2_read_dir,
    .read = ext2_read,
    .write = ext2_write,
    .truncate = ext2_truncate,
    .set_mode = ext2_set_mode,
    .statfs = ext2_statfs,
    .release = ext2_release,
};

/* ---- Mounting ---- */

static int ext2_mount(struct mount *mount, struct block_device *device) {
    if (!device) {
        return -VX_EINVAL;
    }
    struct ext2 *fs = kzalloc(sizeof(*fs));
    if (!fs) {
        return -VX_ENOMEM;
    }
    fs->device = device;
    fs->mount = mount;
    int error = block_read_bytes(device, SUPERBLOCK_OFFSET, &fs->sb, sizeof(fs->sb));
    if (error || fs->sb.magic != EXT2_MAGIC || fs->sb.log_block_size > 2 ||
        fs->sb.blocks_per_group == 0 || fs->sb.inodes_per_group == 0) {
        kfree(fs);
        return error ? error : -VX_EINVAL;
    }
    if (fs->sb.feature_incompat & ~SUPPORTED_INCOMPAT) {
        kprintf("[ext2] %s: needs features Vexa doesn't support (incompat %x); an ext4 disk?\n",
                device->name, fs->sb.feature_incompat & ~SUPPORTED_INCOMPAT);
        kfree(fs);
        return -VX_EINVAL;
    }
    fs->block_size = 1024U << fs->sb.log_block_size;
    fs->inode_size = fs->sb.rev_level >= 1 ? fs->sb.inode_size : 128;
    if (fs->sb.rev_level == 0) {
        fs->sb.first_inode = 11;
    }
    fs->file_types = fs->sb.feature_incompat & INCOMPAT_FILETYPE;
    fs->group_count = (fs->sb.blocks_count - fs->sb.first_data_block + fs->sb.blocks_per_group - 1) /
                      fs->sb.blocks_per_group;
    fs->groups = kmalloc(fs->group_count * sizeof(struct group_descriptor));
    if (!fs->groups ||
        block_read_bytes(device, (uint64_t)(fs->sb.first_data_block + 1) * fs->block_size,
                         fs->groups, fs->group_count * sizeof(struct group_descriptor))) {
        kfree(fs->groups);
        kfree(fs);
        return -VX_EIO;
    }
    mount->read_only = !device->write || (fs->sb.feature_ro_compat & ~SUPPORTED_RO_COMPAT);
    if (mount->read_only && device->write) {
        kprintf("[ext2] %s: mounting read-only (unsupported features %x)\n", device->name,
                fs->sb.feature_ro_compat & ~SUPPORTED_RO_COMPAT);
    }
    mount->data = fs;
    error = get_node(fs, ROOT_INODE, &mount->root);
    if (error) {
        kfree(fs->groups);
        kfree(fs);
        return error;
    }
    return 0;
}

const struct filesystem_type ext2_type = {
    .name = "ext2",
    .mount = ext2_mount,
};

/* True if the device holds an ext2 file system (checks the magic number). */
bool ext2_probe(struct block_device *device) {
    uint16_t magic = 0;
    return block_read_bytes(device, SUPERBLOCK_OFFSET + 56, &magic, 2) == 0 && magic == EXT2_MAGIC;
}
