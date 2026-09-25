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

struct tmpfs_node {
    struct vnode vnode;
    char *name;
    size_t name_length;
    struct tmpfs_node *parent;
    struct tmpfs_node *children; /* Directories. */
    struct tmpfs_node *next_sibling;
    uint8_t **pages; /* Files: page_count entries, NULL for holes. */
    size_t page_count;
    bool removed;
};

static uint64_t next_inode = 1;
static const struct vnode_ops tmpfs_ops;

static struct tmpfs_node *node_of(struct vnode *vnode) {
    return (struct tmpfs_node *)vnode;
}

static void free_pages_from(struct tmpfs_node *node, size_t first) {
    for (size_t i = first; i < node->page_count; i++) {
        if (node->pages[i]) {
            pmm_free(virt_to_phys(node->pages[i]), 0);
            node->pages[i] = NULL;
        }
    }
}

static struct tmpfs_node *new_node(struct mount *mount, uint32_t type, const char *name,
                                   size_t length) {
    struct tmpfs_node *node = kzalloc(sizeof(*node));
    char *copy = kmalloc(length + 1);
    if (!node || !copy) {
        kfree(node);
        kfree(copy);
        return NULL;
    }
    vnode_init(&node->vnode, mount, type, &tmpfs_ops);
    node->vnode.inode = next_inode++;
    node->vnode.modified = time_now();
    node->vnode.links = type == VX_TYPE_DIRECTORY ? 2 : 1;
    memcpy(copy, name, length);
    copy[length] = '\0';
    node->name = copy;
    node->name_length = length;
    return node;
}

static struct tmpfs_node *find_child(struct tmpfs_node *dir, const char *name, size_t length) {
    for (struct tmpfs_node *child = dir->children; child; child = child->next_sibling) {
        if (child->name_length == length && memcmp(child->name, name, length) == 0) {
            return child;
        }
    }
    return NULL;
}

static int tmpfs_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    struct tmpfs_node *node = node_of(dir);
    struct tmpfs_node *found = length == 2 && name[0] == '.' && name[1] == '.'
                                   ? (node->parent ? node->parent : node)
                                   : find_child(node, name, length);
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
    struct tmpfs_node *node = new_node(dir->mount, type, name, length);
    if (!node) {
        return -VX_ENOMEM;
    }
    node->parent = parent;
    node->next_sibling = parent->children;
    parent->children = node;
    if (type == VX_TYPE_DIRECTORY) {
        dir->links++;
    }
    dir->modified = time_now();
    vnode_ref(&node->vnode); /* One for the directory tree, one for the caller. */
    *out = &node->vnode;
    return 0;
}

static int tmpfs_remove(struct vnode *dir, const char *name, size_t length) {
    struct tmpfs_node *parent = node_of(dir);
    struct tmpfs_node **link = &parent->children;
    while (*link && !((*link)->name_length == length && memcmp((*link)->name, name, length) == 0)) {
        link = &(*link)->next_sibling;
    }
    struct tmpfs_node *node = *link;
    if (!node) {
        return -VX_ENOENT;
    }
    if (node->children) {
        return -VX_ENOTEMPTY;
    }
    *link = node->next_sibling;
    if (node->vnode.type == VX_TYPE_DIRECTORY) {
        dir->links--;
    }
    dir->modified = time_now();
    node->removed = true;
    node->vnode.links = 0;
    vnode_put(&node->vnode); /* The tree's reference; open handles keep it alive. */
    return 0;
}

static int tmpfs_rename(struct vnode *old_dir, const char *old_name, size_t old_length,
                        struct vnode *new_dir, const char *new_name, size_t new_length) {
    struct tmpfs_node *from = node_of(old_dir), *to = node_of(new_dir);
    struct tmpfs_node *node = find_child(from, old_name, old_length);
    if (!node) {
        return -VX_ENOENT;
    }
    struct tmpfs_node *existing = find_child(to, new_name, new_length);
    if (existing == node) {
        return 0;
    }
    if (existing) {
        bool node_dir = node->vnode.type == VX_TYPE_DIRECTORY;
        bool existing_dir = existing->vnode.type == VX_TYPE_DIRECTORY;
        if (existing_dir && !node_dir) {
            return -VX_EISDIR;
        }
        if (!existing_dir && node_dir) {
            return -VX_ENOTDIR;
        }
        int error = tmpfs_remove(new_dir, new_name, new_length);
        if (error) {
            return error;
        }
    }
    char *name = kmalloc(new_length + 1);
    if (!name) {
        return -VX_ENOMEM;
    }
    memcpy(name, new_name, new_length);
    name[new_length] = '\0';
    for (struct tmpfs_node **link = &from->children; *link; link = &(*link)->next_sibling) {
        if (*link == node) {
            *link = node->next_sibling;
            break;
        }
    }
    kfree(node->name);
    node->name = name;
    node->name_length = new_length;
    node->parent = to;
    node->next_sibling = to->children;
    to->children = node;
    if (node->vnode.type == VX_TYPE_DIRECTORY && from != to) {
        old_dir->links--;
        new_dir->links++;
    }
    old_dir->modified = new_dir->modified = time_now();
    return 0;
}

static int tmpfs_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry) {
    struct tmpfs_node *child = node_of(dir)->children;
    for (uint64_t i = 0; child && i < *cookie; i++) {
        child = child->next_sibling;
    }
    if (!child) {
        return 0;
    }
    entry->inode = child->vnode.inode;
    entry->type = child->vnode.type;
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
    if (!ensure_page_slots(node, (offset + size + PAGE_SIZE - 1) / PAGE_SIZE)) {
        return -VX_ENOMEM;
    }
    size_t done = 0;
    while (done < size) {
        uint64_t page = (offset + done) / PAGE_SIZE, within = (offset + done) % PAGE_SIZE;
        size_t n = PAGE_SIZE - within < size - done ? PAGE_SIZE - within : size - done;
        if (!node->pages[page]) {
            uint64_t phys = pmm_alloc(0);
            if (!phys) {
                break;
            }
            node->pages[page] = phys_to_virt(phys);
            memset(node->pages[page], 0, PAGE_SIZE);
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
    struct tmpfs_node *node = node_of(vnode);
    if (!node->removed) {
        return; /* Still in the tree, which holds a reference; can't happen. */
    }
    free_pages_from(node, 0);
    kfree(node->pages);
    kfree(node->name);
    kfree(node);
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
    .rename = tmpfs_rename,
    .read_dir = tmpfs_read_dir,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .truncate = tmpfs_truncate,
    .release = tmpfs_release,
    .statfs = tmpfs_statfs,
};

static int tmpfs_mount(struct mount *mount, struct block_device *device) {
    (void)device;
    struct tmpfs_node *root = new_node(mount, VX_TYPE_DIRECTORY, "", 0);
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
