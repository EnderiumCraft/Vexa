#include <vexa/block.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/pty.h>
#include <vexa/tty.h>
#include <vexa/vfs.h>

/* devfs: /dev, the device files. null, zero and console are always there;
 * drivers add the rest as they find devices (disks and partitions, input
 * devices under /dev/input, the display...). */

#define MAX_DEVICES 64

struct device_node {
    struct vnode vnode;
    char name[16];
    struct device_node *parent; /* Its directory; NULL for /dev itself. */
    struct block_device *block; /* For block devices. */
    const char *link;           /* For symbolic links: the target. */
    void *data;                 /* The driver's (devfs_add). */
};

struct pending_device {
    char path[32];
    uint32_t type;
    const struct vnode_ops *ops;
    struct block_device *block;
    void *data;
};

static struct mount *devfs_mount_point;
static struct vnode devfs_root;
static struct device_node *devices[MAX_DEVICES];
static int device_count;
static struct pending_device pending[MAX_DEVICES]; /* Found before /dev was mounted. */
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

static int64_t console_read(struct file *file, void *b, size_t s) {
    return tty_read(console_tty, b, s, file->object.flags & OBJECT_NONBLOCK);
}

static int64_t console_write(struct file *file, const void *b, size_t s) {
    (void)file;
    return tty_write(console_tty, b, s);
}

static uint32_t console_poll(struct file *file) {
    (void)file;
    return tty_poll(console_tty);
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
static const struct vnode_ops console_ops = {
    .file_read = console_read,
    .file_write = console_write,
    .file_poll = console_poll,
};

struct tty *vfs_terminal(struct file *file) {
    if (file->vnode->ops == &console_ops) {
        return console_tty;
    }
    return pty_terminal(file);
}

bool vfs_is_terminal(struct file *file) {
    return vfs_terminal(file) != NULL;
}
static const struct vnode_ops block_ops = {.read = block_node_read, .write = block_node_write};

/* /dev/fd, /dev/stdin...: symbolic links into /proc/self/fd, as on Linux. */
static const char *const link_targets[][2] = {
    {"fd", "/proc/self/fd"},
    {"stdin", "/proc/self/fd/0"},
    {"stdout", "/proc/self/fd/1"},
    {"stderr", "/proc/self/fd/2"},
    {"shm", "/run/shm"}, /* POSIX shared memory and semaphores: files in a tmpfs. */
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

static const struct vnode_ops devfs_dir_ops;

static struct device_node *add_node(struct device_node *parent, const char *name, uint32_t type,
                                    const struct vnode_ops *ops, struct block_device *block) {
    struct device_node *node = kzalloc(sizeof(*node));
    if (!node || device_count == MAX_DEVICES) {
        kfree(node);
        kprintf("[devfs] no room for /dev/%s\n", name);
        return NULL;
    }
    vnode_init(&node->vnode, devfs_mount_point, type, ops);
    node->parent = parent;
    node->vnode.inode = device_count + 2;
    if (type == VX_TYPE_DIRECTORY) {
        node->vnode.links = 2;
        node->vnode.mode = 0755;
    }
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

/* The node behind a directory vnode (NULL for /dev itself). */
static struct device_node *dir_node(struct vnode *dir) {
    return dir == &devfs_root ? NULL : (struct device_node *)dir;
}

static int devfs_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct device_node *here = dir_node(dir);
    if (length == 2 && name[0] == '.' && name[1] == '.') {
        struct vnode *up = here && here->parent ? &here->parent->vnode : &devfs_root;
        vnode_ref(up);
        *out = up;
        return 0;
    }
    for (int i = 0; i < device_count; i++) {
        if (devices[i]->parent == here && strlen(devices[i]->name) == length &&
            memcmp(devices[i]->name, name, length) == 0) {
            vnode_ref(&devices[i]->vnode);
            *out = &devices[i]->vnode;
            return 0;
        }
    }
    return -VX_ENOENT;
}

static int devfs_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry) {
    struct device_node *here = dir_node(dir);
    while (*cookie < (uint64_t)device_count && devices[*cookie]->parent != here) {
        (*cookie)++;
    }
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

static struct device_node *add_path(const char *path, uint32_t type, const struct vnode_ops *ops,
                                    struct block_device *block, void *data);

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
    add_node(NULL, "null", VX_TYPE_CHAR_DEVICE, &null_ops, NULL);
    add_node(NULL, "zero", VX_TYPE_CHAR_DEVICE, &zero_ops, NULL);
    add_node(NULL, "console", VX_TYPE_CHAR_DEVICE, &console_ops, NULL);
    add_node(NULL, "tty", VX_TYPE_CHAR_DEVICE, &console_ops, NULL);
    for (size_t i = 0; i < sizeof(link_targets) / sizeof(link_targets[0]); i++) {
        struct device_node *link =
            add_node(NULL, link_targets[i][0], VX_TYPE_SYMLINK, &link_ops, NULL);
        if (link) {
            link->link = link_targets[i][1];
            link->vnode.size = strlen(link->link);
        }
    }
    for (int i = 0; i < pending_count; i++) {
        add_path(pending[i].path, pending[i].type, pending[i].ops, pending[i].block,
                 pending[i].data);
    }
    pending_count = 0;
    return 0;
}

/* Adds a node at `path` (relative to /dev), making its directories. */
static struct device_node *add_path(const char *path, uint32_t type, const struct vnode_ops *ops,
                                    struct block_device *block, void *data) {
    struct device_node *parent = NULL;
    for (;;) {
        const char *slash = strchr(path, '/');
        if (!slash) {
            break;
        }
        char name[16] = {0};
        size_t n = (size_t)(slash - path) < sizeof(name) - 1 ? (size_t)(slash - path)
                                                             : sizeof(name) - 1;
        memcpy(name, path, n);
        struct device_node *dir = NULL;
        for (int i = 0; i < device_count && !dir; i++) {
            if (devices[i]->parent == parent && strcmp(devices[i]->name, name) == 0) {
                dir = devices[i];
            }
        }
        if (!dir) {
            dir = add_node(parent, name, VX_TYPE_DIRECTORY, &devfs_dir_ops, NULL);
            if (!dir) {
                return NULL;
            }
        }
        parent = dir;
        path = slash + 1;
    }
    struct device_node *node = add_node(parent, path, type, ops, block);
    if (node) {
        node->data = data;
    }
    return node;
}

static void add_or_defer(const char *path, uint32_t type, const struct vnode_ops *ops,
                         struct block_device *block, void *data) {
    vfs_lock();
    if (devfs_mount_point) {
        add_path(path, type, ops, block, data);
    } else if (pending_count < MAX_DEVICES) {
        struct pending_device *p = &pending[pending_count++];
        memset(p->path, 0, sizeof(p->path));
        memcpy(p->path, path, strlen(path) < sizeof(p->path) - 1 ? strlen(path)
                                                                 : sizeof(p->path) - 1);
        p->type = type;
        p->ops = ops;
        p->block = block;
        p->data = data;
    }
    vfs_unlock();
}

void devfs_add_block_device(struct block_device *device) {
    add_or_defer(device->name, VX_TYPE_BLOCK_DEVICE, &block_ops, device, NULL);
}

void devfs_add(const char *path, const struct vnode_ops *ops, void *data) {
    add_or_defer(path, VX_TYPE_CHAR_DEVICE, ops, NULL, data);
}

void devfs_add_directory(const char *path, const struct vnode_ops *ops) {
    add_or_defer(path, VX_TYPE_DIRECTORY, ops, NULL, NULL);
}

int devfs_parent(struct vnode *dir, struct vnode **out) {
    struct device_node *node = dir_node(dir);
    struct vnode *up = node && node->parent ? &node->parent->vnode : &devfs_root;
    vnode_ref(up);
    *out = up;
    return 0;
}

void *devfs_data(struct vnode *vnode) {
    return ((struct device_node *)vnode)->data;
}

const struct filesystem_type devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
};
