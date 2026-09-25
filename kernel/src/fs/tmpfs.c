#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * tmpfs: files and directories kept in RAM. File contents live in whole pages
 * (a growable array of page pointers), so large files don't need one big
 * contiguous allocation.
 */

/* A name in a directory. A node can have several (hard links); each holds a
 * reference to it. */
struct tmpfs_dirent {
    char *name;
    size_t name_length;
    struct tmpfs_node *node;
    struct tmpfs_dirent *next;
};

struct tmpfs_node {
    struct vnode vnode;
    struct tmpfs_node *parent;     /* Directories: for "..". */
    struct tmpfs_dirent *entries;  /* Directories. */
    uint8_t **pages; /* Files: page_count entries, NULL for holes. */
    size_t page_count;
};

static uint64_t next_inode = 1;
static const struct vnode_ops tmpfs_ops;

static struct tmpfs_node *node_of(struct vnode *vnode) {
    return (struct tmpfs_node *)vnode;
}

static void free_pages_from(struct tmpfs_node *node, size_t first) {
    for (size_t i = first; i < node->page_count; i++) {
        if (node->pages[i]) {
            /* Reference counted: a process may still have it mapped (MAP_SHARED). */
            page_ref_put(virt_to_phys(node->pages[i]));
            node->pages[i] = NULL;
        }
    }
}

static struct tmpfs_node *new_node(struct mount *mount, uint32_t type) {
    struct tmpfs_node *node = kzalloc(sizeof(*node));
    if (!node) {
        return NULL;
    }
    vnode_init(&node->vnode, mount, type, &tmpfs_ops);
    node->vnode.inode = next_inode++;
    node->vnode.modified = time_now();
    node->vnode.links = type == VX_TYPE_DIRECTORY ? 2 : 0; /* Files: one per name. */
    return node;
}

static struct tmpfs_dirent **find_entry(struct tmpfs_node *dir, const char *name, size_t length) {
    struct tmpfs_dirent **link = &dir->entries;
    while (*link && !((*link)->name_length == length && memcmp((*link)->name, name, length) == 0)) {
        link = &(*link)->next;
    }
    return link;
}

/* Gives `node` a name in `dir`; the entry takes a reference. */
static int add_entry(struct tmpfs_node *dir, const char *name, size_t length,
                     struct tmpfs_node *node) {
    struct tmpfs_dirent *entry = kmalloc(sizeof(*entry));
    char *copy = kmalloc(length + 1);
    if (!entry || !copy) {
        kfree(entry);
        kfree(copy);
        return -VX_ENOMEM;
    }
    memcpy(copy, name, length);
    copy[length] = '\0';
    entry->name = copy;
    entry->name_length = length;
    entry->node = node;
    entry->next = dir->entries;
    dir->entries = entry;
    vnode_ref(&node->vnode);
    if (node->vnode.type != VX_TYPE_DIRECTORY) {
        node->vnode.links++;
    }
    dir->vnode.modified = time_now();
    return 0;
}

static int tmpfs_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct tmpfs_node *node = node_of(dir);
    struct tmpfs_node *found;
    if (length == 2 && name[0] == '.' && name[1] == '.') {
        found = node->parent ? node->parent : node;
    } else {
        struct tmpfs_dirent *entry = *find_entry(node, name, length);
        found = entry ? entry->node : NULL;
    }
    if (!found) {
        return -VX_ENOENT;
    }
    vnode_ref(&found->vnode);
    *out = &found->vnode;
    return 0;
}

static int tmpfs_create(struct vnode *dir, const char *name, size_t length, uint32_t type,
                        struct vnode **out) {
    struct tmpfs_node *parent = node_of(dir);
    struct tmpfs_node *node = new_node(dir->mount, type);
    if (!node) {
        return -VX_ENOMEM;
    }
    int error = add_entry(parent, name, length, node);
    if (error) {
        kfree(node);
        return error;
    }
    if (type == VX_TYPE_DIRECTORY) {
        node->parent = parent;
        dir->links++;
    }
    *out = &node->vnode; /* new_node's reference goes to the caller. */
    return 0;
}

static int tmpfs_link(struct vnode *dir, const char *name, size_t length, struct vnode *target) {
    return add_entry(node_of(dir), name, length, node_of(target));
}

/* Takes an entry out of `dir` and drops its reference. */
static void drop_entry(struct tmpfs_node *dir, struct tmpfs_dirent **link) {
    struct tmpfs_dirent *entry = *link;
    struct tmpfs_node *node = entry->node;
    *link = entry->next;
    if (node->vnode.type == VX_TYPE_DIRECTORY) {
        dir->vnode.links--;
        node->vnode.links = 0;
    } else {
        node->vnode.links--;
    }
    dir->vnode.modified = time_now();
    kfree(entry->name);
    kfree(entry);
    vnode_put(&node->vnode); /* Open handles and other names keep it alive. */
}

static int tmpfs_remove(struct vnode *dir, const char *name, size_t length) {
    struct tmpfs_node *parent = node_of(dir);
    struct tmpfs_dirent **link = find_entry(parent, name, length);
    if (!*link) {
        return -VX_ENOENT;
    }
    if ((*link)->node->entries) {
        return -VX_ENOTEMPTY;
    }
    drop_entry(parent, link);
    return 0;
}

static int tmpfs_rename(struct vnode *old_dir, const char *old_name, size_t old_length,
                        struct vnode *new_dir, const char *new_name, size_t new_length) {
    struct tmpfs_node *from = node_of(old_dir), *to = node_of(new_dir);
    struct tmpfs_dirent **old_link = find_entry(from, old_name, old_length);
    if (!*old_link) {
        return -VX_ENOENT;
    }
    struct tmpfs_node *node = (*old_link)->node;
    struct tmpfs_dirent **existing = find_entry(to, new_name, new_length);
    if (*existing) {
        if ((*existing)->node == node) {
            return 0; /* Two names for the same file: nothing to do. */
        }
        bool node_dir = node->vnode.type == VX_TYPE_DIRECTORY;
        bool existing_dir = (*existing)->node->vnode.type == VX_TYPE_DIRECTORY;
        if (existing_dir && !node_dir) {
            return -VX_EISDIR;
        }
        if (!existing_dir && node_dir) {
            return -VX_ENOTDIR;
        }
        if ((*existing)->node->entries) {
            return -VX_ENOTEMPTY;
        }
        drop_entry(to, existing);
    }
    /* Name it in the new place first, then drop the old name. */
    vnode_ref(&node->vnode);
    int error = add_entry(to, new_name, new_length, node);
    if (!error) {
        drop_entry(from, find_entry(from, old_name, old_length));
        if (node->vnode.type == VX_TYPE_DIRECTORY) {
            node->parent = to;
            to->vnode.links++;
            node->vnode.links = 2;
        }
    }
    vnode_put(&node->vnode);
    return error;
}

static int tmpfs_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry) {
    struct tmpfs_dirent *child = node_of(dir)->entries;
    for (uint64_t i = 0; child && i < *cookie; i++) {
        child = child->next;
    }
    if (!child) {
        return 0;
    }
    entry->inode = child->node->vnode.inode;
    entry->type = child->node->vnode.type;
    entry->name_length = child->name_length;
    memcpy(entry->name, child->name, child->name_length + 1);
    (*cookie)++;
    return 1;
}

static int64_t tmpfs_read(struct vnode *vnode, void *buffer, size_t size, uint64_t offset) {
    struct tmpfs_node *node = node_of(vnode);
    if (offset >= vnode->size) {
        return 0;
    }
    if (size > vnode->size - offset) {
        size = vnode->size - offset;
    }
    for (size_t done = 0; done < size;) {
        uint64_t page = (offset + done) / PAGE_SIZE, within = (offset + done) % PAGE_SIZE;
        size_t n = PAGE_SIZE - within < size - done ? PAGE_SIZE - within : size - done;
        if (page < node->page_count && node->pages[page]) {
            memcpy((uint8_t *)buffer + done, node->pages[page] + within, n);
        } else {
            memset((uint8_t *)buffer + done, 0, n); /* A hole reads as zeros. */
        }
        done += n;
    }
    return (int64_t)size;
}

static bool ensure_page_slots(struct tmpfs_node *node, size_t count) {
    if (count <= node->page_count) {
        return true;
    }
    size_t new_count = node->page_count ? node->page_count : 4;
    while (new_count < count) {
        new_count *= 2;
    }
    uint8_t **pages = kzalloc(new_count * sizeof(*pages));
    if (!pages) {
        return false;
    }
    if (node->pages) {
        memcpy(pages, node->pages, node->page_count * sizeof(*pages));
        kfree(node->pages);
    }
    node->pages = pages;
    node->page_count = new_count;
    return true;
}

static int64_t tmpfs_write(struct vnode *vnode, const void *buffer, size_t size, uint64_t offset) {
    struct tmpfs_node *node = node_of(vnode);
    if (offset + size < offset) {
        return -VX_EINVAL;
    }
    if (size == 0) {
        return 0;
    }
    if (!ensure_page_slots(node, (offset + size + PAGE_SIZE - 1) / PAGE_SIZE)) {
        return -VX_ENOMEM;
    }
    size_t done = 0;
    while (done < size) {
        uint64_t page = (offset + done) / PAGE_SIZE, within = (offset + done) % PAGE_SIZE;
        size_t n = PAGE_SIZE - within < size - done ? PAGE_SIZE - within : size - done;
        if (!node->pages[page]) {
            uint64_t phys = page_ref_new(); /* Zeroed. */
            if (!phys) {
                break;
            }
            node->pages[page] = phys_to_virt(phys);
        }
        memcpy(node->pages[page] + within, (const uint8_t *)buffer + done, n);
        done += n;
    }
    if (offset + done > vnode->size) {
        vnode->size = offset + done;
    }
    vnode->modified = time_now();
    return done ? (int64_t)done : -VX_ENOSPC;
}

static int tmpfs_truncate(struct vnode *vnode, uint64_t size) {
    struct tmpfs_node *node = node_of(vnode);
    if (size < vnode->size) {
        size_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        free_pages_from(node, keep);
        if (size % PAGE_SIZE && keep > 0 && keep - 1 < node->page_count && node->pages[keep - 1]) {
            memset(node->pages[keep - 1] + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        }
    }
    vnode->size = size;
    vnode->modified = time_now();
    return 0;
}

static void tmpfs_release(struct vnode *vnode) {
    struct tmpfs_node *node = node_of(vnode); /* No names or handles are left. */
    free_pages_from(node, 0);
    kfree(node->pages);
    kfree(node);
}

/* A page of the file for mapping into a process (MAP_SHARED), with a
 * reference for the mapping; 0 past the end of the file. */
static uint64_t tmpfs_share_page(struct vnode *vnode, uint64_t index) {
    struct tmpfs_node *node = node_of(vnode);
    if (index * PAGE_SIZE >= vnode->size || !ensure_page_slots(node, index + 1)) {
        return 0;
    }
    if (!node->pages[index]) {
        uint64_t phys = page_ref_new();
        if (!phys) {
            return 0;
        }
        node->pages[index] = phys_to_virt(phys);
    }
    uint64_t phys = virt_to_phys(node->pages[index]);
    page_ref_get(phys);
    return phys;
}

static void tmpfs_statfs(struct mount *mount, uint64_t *total, uint64_t *free) {
    (void)mount;
    *total = pmm_total_pages() * PAGE_SIZE; /* tmpfs shares the machine's memory. */
    *free = pmm_free_pages() * PAGE_SIZE;
}

static const struct vnode_ops tmpfs_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .remove = tmpfs_remove,
    .link = tmpfs_link,
    .rename = tmpfs_rename,
    .read_dir = tmpfs_read_dir,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .truncate = tmpfs_truncate,
    .release = tmpfs_release,
    .statfs = tmpfs_statfs,
    .share_page = tmpfs_share_page,
};

static int tmpfs_mount(struct mount *mount, struct block_device *device) {
    (void)device;
    struct tmpfs_node *root = new_node(mount, VX_TYPE_DIRECTORY);
    if (!root) {
        return -VX_ENOMEM;
    }
    mount->root = &root->vnode;
    return 0;
}

const struct filesystem_type tmpfs_type = {
    .name = "tmpfs",
    .mount = tmpfs_mount,
};
