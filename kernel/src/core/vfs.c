#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

#define MAX_FILESYSTEMS 8

static struct mutex lock = MUTEX_INIT;
static struct mount *mounts;
static struct mount *root_mount;
static const struct filesystem_type *filesystems[MAX_FILESYSTEMS];

void vfs_lock(void) {
    mutex_lock(&lock);
}

void vfs_unlock(void) {
    mutex_unlock(&lock);
}

void vnode_init(struct vnode *vnode, struct mount *mount, uint32_t type,
                const struct vnode_ops *ops) {
    memset(vnode, 0, sizeof(*vnode));
    vnode->type = type;
    vnode->refs = 1;
    vnode->links = 1;
    vnode->ops = ops;
    vnode->mount = mount;
}

void vnode_ref(struct vnode *vnode) {
    __atomic_add_fetch(&vnode->refs, 1, __ATOMIC_RELAXED);
}

void vnode_put(struct vnode *vnode) {
    if (__atomic_sub_fetch(&vnode->refs, 1, __ATOMIC_ACQ_REL) == 0 && vnode->ops->release) {
        vnode->ops->release(vnode);
    }
}

void vfs_register_filesystem(const struct filesystem_type *type) {
    for (int i = 0; i < MAX_FILESYSTEMS; i++) {
        if (!filesystems[i]) {
            filesystems[i] = type;
            return;
        }
    }
    panic("vfs: too many file system types");
}

struct mount *vfs_mounts(void) {
    return mounts;
}

/* ---- Path lookup ---- */

/* Follows mounts: if something is mounted on `vnode`, returns its root instead. */
static struct vnode *cross_mounts(struct vnode *vnode) {
    while (vnode->mounted_here) {
        struct vnode *root = vnode->mounted_here->root;
        vnode_ref(root);
        vnode_put(vnode);
        vnode = root;
    }
    return vnode;
}

/* Looks up one name in `dir` (which keeps its reference). */
static int lookup_step(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    if (dir->type != VX_TYPE_DIRECTORY) {
        return -VX_ENOTDIR;
    }
    if (length == 1 && name[0] == '.') {
        vnode_ref(dir);
        *out = dir;
        return 0;
    }
    if (length == 2 && name[0] == '.' && name[1] == '.') {
        /* At the root of a mounted file system, ".." leaves it. */
        while (dir->mount->mountpoint && dir == dir->mount->root) {
            dir = dir->mount->mountpoint;
        }
        if (dir == root_mount->root) {
            vnode_ref(dir);
            *out = dir;
            return 0;
        }
    }
    if (length > VX_NAME_MAX) {
        return -VX_ENAMETOOLONG;
    }
    struct vnode *found;
    int error = dir->ops->lookup(dir, name, length, &found);
    if (error) {
        return error;
    }
    *out = cross_mounts(found);
    return 0;
}

/* Splits off the next path component. Returns false at the end of the path. */
static bool next_component(const char **path, const char *end, const char **name, size_t *length) {
    while (*path < end && **path == '/') {
        (*path)++;
    }
    if (*path == end) {
        return false;
    }
    *name = *path;
    while (*path < end && **path != '/') {
        (*path)++;
    }
    *length = *path - *name;
    return true;
}

/* Resolves a path. With `parent` set, stops before the last component and
 * returns the directory plus the last name in *name and *name_length (which
 * is empty for "/"). Returned vnodes carry a reference. */
static int resolve(const char *path, size_t length, bool parent, struct vnode **out,
                   const char **name, size_t *name_length) {
    if (!root_mount) {
        return -VX_ENOENT;
    }
    if (length > VX_PATH_MAX) {
        return -VX_ENAMETOOLONG;
    }
    const char *end = path + length;
    struct vnode *vnode = root_mount->root;
    vnode_ref(vnode);

    const char *component;
    size_t component_length;
    bool have = next_component(&path, end, &component, &component_length);
    while (have) {
        const char *next;
        size_t next_length;
        bool more = next_component(&path, end, &next, &next_length);
        if (!more && parent) {
            *out = vnode;
            *name = component;
            *name_length = component_length;
            return 0;
        }
        struct vnode *child;
        int error = lookup_step(vnode, component, component_length, &child);
        vnode_put(vnode);
        if (error) {
            return error;
        }
        vnode = child;
        component = next;
        component_length = next_length;
        have = more;
    }
    if (parent) {
        *name = "";
        *name_length = 0;
    }
    *out = vnode;
    return 0;
}

/* ---- Mounting ---- */

int vfs_mount(const char *fs, struct block_device *device, const char *source, const char *path) {
    const struct filesystem_type *type = NULL;
    for (int i = 0; i < MAX_FILESYSTEMS && filesystems[i]; i++) {
        if (strcmp(filesystems[i]->name, fs) == 0) {
            type = filesystems[i];
        }
    }
    if (!type) {
        return -VX_EINVAL;
    }
    struct mount *mount = kzalloc(sizeof(*mount));
    if (!mount) {
        return -VX_ENOMEM;
    }
    mount->fs_name = type->name;
    for (size_t i = 0; source[i] && i < sizeof(mount->source) - 1; i++) {
        mount->source[i] = source[i];
    }
    for (size_t i = 0; path[i] && i < sizeof(mount->path) - 1; i++) {
        mount->path[i] = path[i];
    }

    vfs_lock();
    struct vnode *mountpoint = NULL;
    int error = 0;
    if (root_mount) {
        error = resolve(path, strlen(path), false, &mountpoint, NULL, NULL);
        if (!error && mountpoint->type != VX_TYPE_DIRECTORY) {
            error = -VX_ENOTDIR;
        } else if (!error && (mountpoint->mounted_here || mountpoint == root_mount->root)) {
            error = -VX_EBUSY;
        }
    } else if (strcmp(path, "/") != 0) {
        error = -VX_ENOENT; /* The first mount must be the root. */
    }
    if (!error) {
        error = type->mount(mount, device);
    }
    if (error) {
        if (mountpoint) {
            vnode_put(mountpoint);
        }
        vfs_unlock();
        kfree(mount);
        return error;
    }
    mount->mountpoint = mountpoint; /* Keeps its reference while mounted. */
    if (mountpoint) {
        mountpoint->mounted_here = mount;
    } else {
        root_mount = mount;
    }
    struct mount **link = &mounts;
    while (*link) {
        link = &(*link)->next;
    }
    *link = mount;
    vfs_unlock();
    return 0;
}

/* ---- Operations by path ---- */

static void file_destroy(struct object *object) {
    struct file *file = (struct file *)object;
    vfs_lock();
    vnode_put(file->vnode);
    vfs_unlock();
    kfree(file);
}

const struct object_type file_object_type = {
    .name = "file",
    .destroy = file_destroy,
};

static bool writes(uint32_t flags) {
    return flags & (VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE | VX_OPEN_APPEND);
}

int vfs_open(const char *path, size_t length, uint32_t flags, struct file **out) {
    struct file *file = kzalloc(sizeof(*file));
    if (!file) {
        return -VX_ENOMEM;
    }
    vfs_lock();
    struct vnode *dir, *vnode = NULL;
    const char *name;
    size_t name_length;
    int error = resolve(path, length, true, &dir, &name, &name_length);
    if (!error) {
        if (name_length == 0) {
            vnode = dir; /* The path was "/" (or all slashes). */
        } else {
            error = lookup_step(dir, name, name_length, &vnode);
            if (error == -VX_ENOENT && (flags & VX_OPEN_CREATE)) {
                error = dir->mount->read_only ? -VX_EROFS
                        : name_length > VX_NAME_MAX ? -VX_ENAMETOOLONG
                        : !dir->ops->create ? -VX_EROFS
                        : dir->ops->create(dir, name, name_length, VX_TYPE_FILE, &vnode);
            }
            vnode_put(dir);
        }
    }
    if (!error && vnode->type == VX_TYPE_DIRECTORY && writes(flags)) {
        error = -VX_EISDIR;
    } else if (!error && writes(flags) && vnode->mount->read_only &&
               vnode->type == VX_TYPE_FILE) {
        error = -VX_EROFS;
    } else if (!error && (flags & VX_OPEN_TRUNCATE) && vnode->type == VX_TYPE_FILE) {
        error = vnode->ops->truncate ? vnode->ops->truncate(vnode, 0) : -VX_EROFS;
    }
    if (error) {
        if (vnode) {
            vnode_put(vnode);
        }
        vfs_unlock();
        kfree(file);
        return error;
    }
    vfs_unlock();
    object_init(&file->object, &file_object_type);
    file->vnode = vnode;
    file->flags = flags;
    *out = file;
    return 0;
}

static void fill_stat(struct vnode *vnode, struct vx_stat *stat) {
    stat->size = vnode->size;
    stat->inode = vnode->inode;
    stat->type = vnode->type;
    stat->links = vnode->links;
    stat->modified = vnode->modified;
}

int vfs_stat(const char *path, size_t length, struct vx_stat *stat) {
    vfs_lock();
    struct vnode *vnode;
    int error = resolve(path, length, false, &vnode, NULL, NULL);
    if (!error) {
        fill_stat(vnode, stat);
        vnode_put(vnode);
    }
    vfs_unlock();
    return error;
}

/* Runs `create` or `remove` on the parent directory of `path`. */
static int modify_parent(const char *path, size_t length, bool create) {
    vfs_lock();
    struct vnode *dir;
    const char *name;
    size_t name_length;
    int error = resolve(path, length, true, &dir, &name, &name_length);
    if (error) {
        vfs_unlock();
        return error;
    }
    if (name_length == 0 || (name_length == 1 && name[0] == '.') ||
        (name_length == 2 && name[0] == '.' && name[1] == '.')) {
        error = create ? -VX_EEXIST : -VX_EBUSY;
    } else if (dir->type != VX_TYPE_DIRECTORY) {
        error = -VX_ENOTDIR;
    } else if (name_length > VX_NAME_MAX) {
        error = -VX_ENAMETOOLONG;
    } else if (dir->mount->read_only || (create ? !dir->ops->create : !dir->ops->remove)) {
        error = -VX_EROFS;
    } else {
        struct vnode *existing;
        error = dir->ops->lookup(dir, name, name_length, &existing);
        if (create) {
            if (!error) {
                vnode_put(existing);
                error = -VX_EEXIST;
            } else if (error == -VX_ENOENT) {
                struct vnode *created;
                error = dir->ops->create(dir, name, name_length, VX_TYPE_DIRECTORY, &created);
                if (!error) {
                    vnode_put(created);
                }
            }
        } else if (!error) {
            bool busy = existing->mounted_here != NULL;
            vnode_put(existing);
            error = busy ? -VX_EBUSY : dir->ops->remove(dir, name, name_length);
        }
    }
    vnode_put(dir);
    vfs_unlock();
    return error;
}

int vfs_mkdir(const char *path, size_t length) {
    return modify_parent(path, length, true);
}

int vfs_remove(const char *path, size_t length) {
    return modify_parent(path, length, false);
}

/* ---- Operations on open files ---- */

int64_t vfs_pread(struct file *file, void *buffer, size_t size, uint64_t offset) {
    struct vnode *vnode = file->vnode;
    if (vnode->type == VX_TYPE_DIRECTORY) {
        return -VX_EISDIR;
    }
    if (!vnode->ops->read) {
        return -VX_EINVAL;
    }
    vfs_lock();
    int64_t result = vnode->ops->read(vnode, buffer, size, offset);
    vfs_unlock();
    return result;
}

int64_t vfs_read(struct file *file, void *buffer, size_t size) {
    vfs_lock();
    uint64_t offset = file->offset;
    vfs_unlock();
    int64_t result = vfs_pread(file, buffer, size, offset);
    if (result > 0) {
        vfs_lock();
        file->offset = offset + result;
        vfs_unlock();
    }
    return result;
}

int64_t vfs_write(struct file *file, const void *buffer, size_t size) {
    struct vnode *vnode = file->vnode;
    if (!vnode->ops->write) {
        return -VX_EINVAL;
    }
    vfs_lock();
    uint64_t offset = (file->flags & VX_OPEN_APPEND) ? vnode->size : file->offset;
    int64_t result = vnode->ops->write(vnode, buffer, size, offset);
    if (result > 0) {
        file->offset = offset + result;
    }
    vfs_unlock();
    return result;
}

int64_t vfs_seek(struct file *file, int64_t offset, int whence) {
    vfs_lock();
    int64_t base = whence == VX_SEEK_SET       ? 0
                   : whence == VX_SEEK_CURRENT ? (int64_t)file->offset
                   : whence == VX_SEEK_END     ? (int64_t)file->vnode->size
                                               : -1;
    int64_t result = -VX_EINVAL;
    if (base >= 0 && file->vnode->type != VX_TYPE_DIRECTORY && base + offset >= 0) {
        file->offset = base + offset;
        result = base + offset;
    }
    vfs_unlock();
    return result;
}

int vfs_read_dir(struct file *file, struct vx_dir_entry *entry) {
    struct vnode *vnode = file->vnode;
    if (vnode->type != VX_TYPE_DIRECTORY) {
        return -VX_ENOTDIR;
    }
    vfs_lock();
    int result = vnode->ops->read_dir(vnode, &file->offset, entry);
    vfs_unlock();
    return result;
}

void vfs_file_stat(struct file *file, struct vx_stat *stat) {
    vfs_lock();
    fill_stat(file->vnode, stat);
    vfs_unlock();
}

void vfs_close(struct file *file) {
    object_put(&file->object);
}

const char *vfs_error_name(int error) {
    switch (-error) {
    case VX_ENOENT: return "no such file or directory";
    case VX_EEXIST: return "already exists";
    case VX_ENOTDIR: return "not a directory";
    case VX_EISDIR: return "is a directory";
    case VX_ENOTEMPTY: return "directory not empty";
    case VX_EBADF: return "bad handle";
    case VX_EACCES: return "not allowed";
    case VX_ENOSPC: return "no space left";
    case VX_EIO: return "I/O error";
    case VX_ENAMETOOLONG: return "name too long";
    case VX_EMFILE: return "too many open handles";
    case VX_ENOMEM: return "out of memory";
    case VX_EROFS: return "read-only file system";
    case VX_EBUSY: return "busy";
    case VX_EINVAL: return "invalid argument";
    case VX_EFAULT: return "bad address";
    default: return "error";
    }
}
