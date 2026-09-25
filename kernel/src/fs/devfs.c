#include <vexa/block.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/tty.h>
#include <vexa/vfs.h>

/* devfs: /dev, a flat directory of device files. null, zero and console are
 * always there; disks and partitions are added as drivers find them. */

#define MAX_DEVICES 64

struct device_node {
    struct vnode vnode;
    char name[16];
    struct block_device *block; /* For block devices. */
    const char *link;           /* For symbolic links: the target. */
};

static struct mount *devfs_mount_point;
static struct vnode devfs_root;
static struct device_node *devices[MAX_DEVICES];
static int device_count;
static struct block_device *pending_blocks[MAX_DEVICES]; /* Found before /dev was mounted. */
static int pending_count;

static int64_t null_read(struct vnode *v, void *b, size_t s, uint64_t o) {
    (void)v, (void)b, (void)s, (void)o;
    return 0;
}

static int64_t null_write(struct vnode *v, const void *b, size_t s, uint64_t o) {
    (void)v, (void)b, (void)o;
    return (int64_t)s;
}

static int64_t zero_read(struct vnode *v, void *b, size_t s, uint64_t o) {
    (void)v, (void)o;
    memset(b, 0, s);
    return (int64_t)s;
}

static int64_t console_read(struct vnode *v, void *b, size_t s, uint64_t o) {
    (void)v, (void)o;
    return tty_read(b, s);
}

static int64_t console_write(struct vnode *v, const void *b, size_t s, uint64_t o) {
    (void)v, (void)o;
    return tty_write(b, s);
}

static int64_t block_node_read(struct vnode *vnode, void *buffer, size_t size, uint64_t offset) {
    struct block_device *device = ((struct device_node *)vnode)->block;
    uint64_t total = block_size_bytes(device);
    if (offset >= total) {
        return 0;
    }
    if (size > total - offset) {
        size = total - offset;
    }
    int error = block_read_bytes(device, offset, buffer, size);
    return error ? error : (int64_t)size;
}

static int64_t block_node_write(struct vnode *vnode, const void *buffer, size_t size,
                                uint64_t offset) {
    struct block_device *device = ((struct device_node *)vnode)->block;
    uint64_t total = block_size_bytes(device);
    if (offset >= total) {
        return -VX_ENOSPC;
    }
    if (size > total - offset) {
        size = total - offset;
    }
    int error = block_write_bytes(device, offset, buffer, size);
    return error ? error : (int64_t)size;
}

static const struct vnode_ops null_ops = {.read = null_read, .write = null_write};
static const struct vnode_ops zero_ops = {.read = zero_read, .write = null_write};
static const struct vnode_ops console_ops = {.read = console_read, .write = console_write};

bool vfs_is_terminal(struct file *file) {
    return file->vnode->ops == &console_ops;
}
static const struct vnode_ops block_ops = {.read = block_node_read, .write = block_node_write};

/* /dev/fd, /dev/stdin...: symbolic links into /proc/self/fd, as on Linux. */
static const char *const link_targets[][2] = {
    {"fd", "/proc/self/fd"},
    {"stdin", "/proc/self/fd/0"},
    {"stdout", "/proc/self/fd/1"},
    {"stderr", "/proc/self/fd/2"},
};

static int64_t link_read(struct vnode *v, void *b, size_t s, uint64_t o) {
    const char *target = ((struct device_node *)v)->link;
    size_t length = strlen(target);
    if (o >= length) {
        return 0;
    }
    size_t n = length - o < s ? length - o : s;
    memcpy(b, target + o, n);
    return (int64_t)n;
}

static const struct vnode_ops link_ops = {.read = link_read};

static struct device_node *add_node(const char *name, uint32_t type,
                                    const struct vnode_ops *ops, struct block_device *block) {
    struct device_node *node = kzalloc(sizeof(*node));
    if (!node || device_count == MAX_DEVICES) {
        kfree(node);
        kprintf("[devfs] no room for /dev/%s\n", name);
        return NULL;
    }
    vnode_init(&node->vnode, devfs_mount_point, type, ops);
    node->vnode.inode = device_count + 2;
    node->vnode.modified = time_now();
    node->block = block;
    if (block) {
        node->vnode.size = block_size_bytes(block);
    }
    for (size_t i = 0; name[i] && i < sizeof(node->name) - 1; i++) {
        node->name[i] = name[i];
    }
    devices[device_count++] = node;
    return node;
}

static int devfs_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    if (length == 2 && name[0] == '.' && name[1] == '.') {
        vnode_ref(dir);
        *out = dir;
        return 0;
    }
    for (int i = 0; i < device_count; i++) {
        if (strlen(devices[i]->name) == length && memcmp(devices[i]->name, name, length) == 0) {
            vnode_ref(&devices[i]->vnode);
            *out = &devices[i]->vnode;
            return 0;
        }
    }
    return -VX_ENOENT;
}

static int devfs_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry) {
    (void)dir;
    if (*cookie >= (uint64_t)device_count) {
        return 0;
    }
    struct device_node *node = devices[(*cookie)++];
    entry->inode = node->vnode.inode;
    entry->type = node->vnode.type;
    entry->name_length = strlen(node->name);
    memcpy(entry->name, node->name, entry->name_length + 1);
    return 1;
}

static const struct vnode_ops devfs_dir_ops = {
    .lookup = devfs_lookup,
    .read_dir = devfs_read_dir,
};

static int devfs_mount(struct mount *mount, struct block_device *device) {
    (void)device;
    if (devfs_mount_point) {
        return -VX_EBUSY; /* One /dev is enough. */
    }
    devfs_mount_point = mount;
    vnode_init(&devfs_root, mount, VX_TYPE_DIRECTORY, &devfs_dir_ops);
    devfs_root.inode = 1;
    devfs_root.links = 2;
    devfs_root.modified = time_now();
    mount->root = &devfs_root;
    add_node("null", VX_TYPE_CHAR_DEVICE, &null_ops, NULL);
    add_node("zero", VX_TYPE_CHAR_DEVICE, &zero_ops, NULL);
    add_node("console", VX_TYPE_CHAR_DEVICE, &console_ops, NULL);
    add_node("tty", VX_TYPE_CHAR_DEVICE, &console_ops, NULL);
    for (size_t i = 0; i < sizeof(link_targets) / sizeof(link_targets[0]); i++) {
        struct device_node *link = add_node(link_targets[i][0], VX_TYPE_SYMLINK, &link_ops, NULL);
        if (link) {
            link->link = link_targets[i][1];
            link->vnode.size = strlen(link->link);
        }
    }
    for (int i = 0; i < pending_count; i++) {
        add_node(pending_blocks[i]->name, VX_TYPE_BLOCK_DEVICE, &block_ops, pending_blocks[i]);
    }
    pending_count = 0;
    return 0;
}

void devfs_add_block_device(struct block_device *device) {
    vfs_lock();
    if (devfs_mount_point) {
        add_node(device->name, VX_TYPE_BLOCK_DEVICE, &block_ops, device);
    } else if (pending_count < MAX_DEVICES) {
        pending_blocks[pending_count++] = device;
    }
    vfs_unlock();
}

const struct filesystem_type devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
};
