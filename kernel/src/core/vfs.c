#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/string.h>
#include <vexa/tty.h>
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
    vnode->mode = type == VX_TYPE_DIRECTORY    ? 0755
                  : type == VX_TYPE_SYMLINK    ? 0777
                  : type == VX_TYPE_CHAR_DEVICE ? 0666
                                               : 0644;
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

/* Linux's limit too: a path may pass through at most this many symbolic links. */
#define MAX_SYMLINKS 40

/* State for one path walk. Following a symbolic link rewrites the path into a
 * new buffer, which lives here until walk_done() (names returned by resolve()
 * may point into it). */
struct walk {
    char *owned;
    int links;
};

static void walk_done(struct walk *walk) {
    kfree(walk->owned);
    walk->owned = NULL;
}

/* Reads a symbolic link's target into a new NUL-terminated buffer. */
static int read_link(struct vnode *link, char **out, size_t *length) {
    if (link->size == 0 || link->size > VX_PATH_MAX || !link->ops->read) {
        return link->size > VX_PATH_MAX ? -VX_ENAMETOOLONG : -VX_EIO;
    }
    char *target = kmalloc(link->size + 1);
    if (!target) {
        return -VX_ENOMEM;
    }
    int64_t n = link->ops->read(link, target, link->size, 0);
    if (n <= 0) {
        kfree(target);
        return n < 0 ? (int)n : -VX_EIO;
    }
    target[n] = '\0';
    *out = target;
    *length = (size_t)n;
    return 0;
}

/* Resolves a path. With `parent` set, stops before the last component and
 * returns the directory plus the last name in *name and *name_length (which
 * is empty for "/"). Symbolic links along the way are followed, and the last
 * component too if `follow` is set. Returned vnodes carry a reference; call
 * walk_done() once *name isn't needed any more. */
static int resolve(struct walk *walk, const char *path, size_t length, bool parent, bool follow,
                   struct vnode **out, const char **name, size_t *name_length) {
    if (!root_mount) {
        return -VX_ENOENT;
    }
restart:
    if (length > VX_PATH_MAX) {
        return -VX_ENAMETOOLONG;
    }
    const char *start = path, *end = path + length;
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
        if (error) {
            vnode_put(vnode);
            return error;
        }
        if (child->type == VX_TYPE_SYMLINK && (more || follow)) {
            /* Replace this component with the link's target and start over:
             * absolute targets from the root, relative ones from here. */
            char *target;
            size_t target_length;
            error = ++walk->links > MAX_SYMLINKS ? -VX_ELOOP
                                                 : read_link(child, &target, &target_length);
            vnode_put(child);
            vnode_put(vnode);
            if (error) {
                return error;
            }
            size_t prefix = target[0] == '/' ? 0 : (size_t)(component - start);
            const char *rest = more ? next : end;
            size_t rest_length = (size_t)(end - rest);
            char *joined = kmalloc(prefix + target_length + 1 + rest_length + 1);
            if (!joined) {
                kfree(target);
                return -VX_ENOMEM;
            }
            memcpy(joined, start, prefix);
            memcpy(joined + prefix, target, target_length);
            joined[prefix + target_length] = '/';
            memcpy(joined + prefix + target_length + 1, rest, rest_length);
            length = prefix + target_length + 1 + rest_length;
            joined[length] = '\0';
            kfree(target);
            kfree(walk->owned);
            walk->owned = joined;
            path = joined;
            goto restart;
        }
        vnode_put(vnode);
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
        struct walk walk = {0};
        error = resolve(&walk, path, strlen(path), false, true, &mountpoint, NULL, NULL);
        walk_done(&walk);
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
    if (file->opened && file->vnode->ops->close) {
        file->vnode->ops->close(file);
    }
    vfs_lock();
    vnode_put(file->vnode);
    vfs_unlock();
    kfree(file->path);
    kfree(file);
}

static int64_t file_object_read(struct object *object, void *buffer, size_t size) {
    return vfs_read((struct file *)object, buffer, size);
}

static int64_t file_object_write(struct object *object, const void *buffer, size_t size) {
    return vfs_write((struct file *)object, buffer, size);
}

static uint32_t file_object_poll(struct object *object) {
    struct file *file = (struct file *)object;
    if (file->vnode->ops->file_poll) {
        return file->vnode->ops->file_poll(file);
    }

    return OBJECT_READABLE | OBJECT_WRITABLE; /* Files never make you wait for long. */
}

const struct object_type file_object_type = {
    .name = "file",
    .destroy = file_destroy,
    .read = file_object_read,
    .write = file_object_write,
    .poll = file_object_poll,
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
    struct walk walk = {0};
    struct vnode *vnode = NULL;
    int error = resolve(&walk, path, length, false, !(flags & VX_OPEN_NO_FOLLOW), &vnode,
                        NULL, NULL);
    if (error == -VX_ENOENT && (flags & VX_OPEN_CREATE)) {
        /* Create it, in its (existing) parent directory. */
        struct vnode *dir;
        const char *name;
        size_t name_length;
        vnode = NULL;
        error = resolve(&walk, path, length, true, true, &dir, &name, &name_length);
        if (!error) {
            error = name_length == 0 || dir->type != VX_TYPE_DIRECTORY ? -VX_ENOENT
                    : dir->mount->read_only                            ? -VX_EROFS
                    : name_length > VX_NAME_MAX                        ? -VX_ENAMETOOLONG
                    : !dir->ops->create                                ? -VX_EROFS
                    : dir->ops->create(dir, name, name_length, VX_TYPE_FILE, &vnode);
            vnode_put(dir);
        }
    } else if (error) {
        vnode = NULL;
    }
    walk_done(&walk);
    if (!error && vnode->type == VX_TYPE_SYMLINK) {
        error = -VX_ELOOP; /* Opened with VX_OPEN_NO_FOLLOW. */
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
    file->path = kmalloc(length + 1);
    if (file->path) {
        memcpy(file->path, path, length);
        file->path[length] = '\0';
    }
    if (vnode->ops->open) {
        error = vnode->ops->open(file);
        if (error) {
            object_put(&file->object);
            return error;
        }
    }
    file->opened = true; /* So close runs, with or without an open hook. */
    *out = file;
    return 0;
}

bool vfs_is_stream(struct file *file) {
    return file->vnode->type == VX_TYPE_CHAR_DEVICE;
}

int vfs_control(struct file *file, uint32_t request, void *arg, size_t size) {
    return file->vnode->ops->control ? file->vnode->ops->control(file, request, arg, size)
                                     : -VX_ENOTTY;
}

static void fill_stat(struct vnode *vnode, struct vx_stat *stat) {
    stat->size = vnode->size;
    stat->inode = vnode->inode;
    stat->type = vnode->type;
    stat->links = vnode->links;
    stat->modified = vnode->modified;
    stat->mode = vnode->mode;
}

static int stat_path(const char *path, size_t length, bool follow, struct vx_stat *stat) {
    vfs_lock();
    struct walk walk = {0};
    struct vnode *vnode;
    int error = resolve(&walk, path, length, false, follow, &vnode, NULL, NULL);
    walk_done(&walk);
    if (!error) {
        fill_stat(vnode, stat);
        vnode_put(vnode);
    }
    vfs_unlock();
    return error;
}

int vfs_stat(const char *path, size_t length, struct vx_stat *stat) {
    return stat_path(path, length, true, stat);
}

int vfs_lstat(const char *path, size_t length, struct vx_stat *stat) {
    return stat_path(path, length, false, stat);
}

int vfs_readlink(const char *path, size_t length, char *buffer, size_t size) {
    vfs_lock();
    struct walk walk = {0};
    struct vnode *vnode;
    int error = resolve(&walk, path, length, false, false, &vnode, NULL, NULL);
    walk_done(&walk);
    int result = error;
    if (!error) {
        char *target;
        size_t target_length;
        result = vnode->type != VX_TYPE_SYMLINK ? -VX_EINVAL
                                                : read_link(vnode, &target, &target_length);
        if (result == 0) {
            result = (int)(target_length < size ? target_length : size);
            memcpy(buffer, target, result);
            kfree(target);
        }
        vnode_put(vnode);
    }
    vfs_unlock();
    return result;
}

/* Runs `create` (a directory, or a symbolic link to `target`) or `remove` on
 * the parent directory of `path`. */
static int modify_parent(const char *path, size_t length, bool create, const char *target) {
    vfs_lock();
    struct walk walk = {0};
    struct vnode *dir;
    const char *name;
    size_t name_length;
    int error = resolve(&walk, path, length, true, true, &dir, &name, &name_length);
    if (error) {
        walk_done(&walk);
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
                struct vnode *created = NULL;
                error = dir->ops->create(dir, name, name_length,
                                         target ? VX_TYPE_SYMLINK : VX_TYPE_DIRECTORY, &created);
                if (!error && target) {
                    size_t target_length = strlen(target);
                    int64_t n = created->ops->write
                                    ? created->ops->write(created, target, target_length, 0)
                                    : -VX_EROFS;
                    if (n != (int64_t)target_length) {
                        /* Couldn't store the target: take the link away again. */
                        error = n < 0 ? (int)n : -VX_ENOSPC;
                        vnode_put(created);
                        created = NULL;
                        dir->ops->remove(dir, name, name_length);
                    }
                }
                if (created && !error) {
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
    walk_done(&walk);
    vfs_unlock();
    return error;
}

int vfs_mkdir(const char *path, size_t length) {
    return modify_parent(path, length, true, NULL);
}

int vfs_symlink(const char *target, const char *path, size_t length) {
    size_t target_length = strlen(target);
    if (target_length == 0) {
        return -VX_ENOENT;
    }
    if (target_length > VX_PATH_MAX) {
        return -VX_ENAMETOOLONG;
    }
    return modify_parent(path, length, true, target);
}

int vfs_remove(const char *path, size_t length) {
    return modify_parent(path, length, false, NULL);
}

static bool is_dot_or_dotdot(const char *name, size_t length) {
    return (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.');
}

int vfs_link(const char *from, size_t from_length, const char *to, size_t to_length) {
    vfs_lock();
    struct walk from_walk = {0}, to_walk = {0};
    struct vnode *target = NULL, *dir = NULL;
    const char *name;
    size_t name_length;
    int error = resolve(&from_walk, from, from_length, false, false, &target, NULL, NULL);
    if (!error) {
        error = resolve(&to_walk, to, to_length, true, true, &dir, &name, &name_length);
    }
    if (!error && target->type == VX_TYPE_DIRECTORY) {
        error = -VX_EACCES; /* No hard links to directories. */
    } else if (!error && (name_length == 0 || is_dot_or_dotdot(name, name_length))) {
        error = -VX_EEXIST;
    } else if (!error && dir->type != VX_TYPE_DIRECTORY) {
        error = -VX_ENOTDIR;
    } else if (!error && target->mount != dir->mount) {
        error = -VX_EXDEV;
    } else if (!error && (dir->mount->read_only || !dir->ops->link)) {
        error = dir->mount->read_only ? -VX_EROFS : -VX_EACCES;
    } else if (!error && name_length > VX_NAME_MAX) {
        error = -VX_ENAMETOOLONG;
    }
    if (!error) {
        struct vnode *existing;
        if (dir->ops->lookup(dir, name, name_length, &existing) == 0) {
            vnode_put(existing);
            error = -VX_EEXIST;
        } else {
            error = dir->ops->link(dir, name, name_length, target);
        }
    }
    if (dir) {
        vnode_put(dir);
    }
    if (target) {
        vnode_put(target);
    }
    walk_done(&from_walk);
    walk_done(&to_walk);
    vfs_unlock();
    return error;
}

int vfs_rename(const char *from, size_t from_length, const char *to, size_t to_length) {
    /* Moving a directory inside itself would cut it off from the tree. Paths
     * from system calls are absolute and normalized, so a prefix check works. */
    if (to_length > from_length && memcmp(from, to, from_length) == 0 && to[from_length] == '/') {
        return -VX_EINVAL;
    }
    vfs_lock();
    struct vnode *old_dir = NULL, *new_dir = NULL, *moving = NULL;
    const char *old_name, *new_name;
    size_t old_length, new_length;
    struct walk old_walk = {0}, new_walk = {0};
    int error = resolve(&old_walk, from, from_length, true, true, &old_dir, &old_name, &old_length);
    if (!error) {
        error = resolve(&new_walk, to, to_length, true, true, &new_dir, &new_name, &new_length);
    }
    if (!error && (old_length == 0 || new_length == 0 || is_dot_or_dotdot(old_name, old_length) ||
                   is_dot_or_dotdot(new_name, new_length))) {
        error = -VX_EBUSY;
    }
    if (!error && (old_dir->type != VX_TYPE_DIRECTORY || new_dir->type != VX_TYPE_DIRECTORY)) {
        error = -VX_ENOTDIR;
    }
    if (!error && new_length > VX_NAME_MAX) {
        error = -VX_ENAMETOOLONG;
    }
    if (!error && old_dir->mount != new_dir->mount) {
        error = -VX_EXDEV;
    }
    if (!error && (old_dir->mount->read_only || !old_dir->ops->rename)) {
        error = -VX_EROFS;
    }
    if (!error) {
        error = old_dir->ops->lookup(old_dir, old_name, old_length, &moving);
    }
    if (!error && moving->mounted_here) {
        error = -VX_EBUSY;
    }
    if (!error) {
        error = old_dir->ops->rename(old_dir, old_name, old_length, new_dir, new_name, new_length);
    }
    if (moving) {
        vnode_put(moving);
    }
    if (old_dir) {
        vnode_put(old_dir);
    }
    if (new_dir) {
        vnode_put(new_dir);
    }
    walk_done(&old_walk);
    walk_done(&new_walk);
    vfs_unlock();
    return error;
}

/* ---- Operations on open files ---- */

int64_t vfs_pread(struct file *file, void *buffer, size_t size, uint64_t offset) {
    struct vnode *vnode = file->vnode;
    if (vnode->type == VX_TYPE_DIRECTORY) {
        return -VX_EISDIR;
    }
    if (vnode->ops->file_read) {
        return vnode->ops->file_read(file, buffer, size);
    }
    if (!vnode->ops->read) {
        return -VX_EINVAL;
    }
    if (vnode->type == VX_TYPE_CHAR_DEVICE) {
        /* Devices like the terminal can wait a long time: not under the lock. */
        return vnode->ops->read(vnode, buffer, size, offset);
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
    if (vnode->ops->file_write) {
        return vnode->ops->file_write(file, buffer, size);
    }
    if (!vnode->ops->write) {
        return -VX_EINVAL;
    }
    if (vnode->type == VX_TYPE_CHAR_DEVICE) {
        return vnode->ops->write(vnode, buffer, size, file->offset);
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

static int set_mode_locked(struct vnode *vnode, uint32_t mode) {
    if (vnode->mount->read_only) {
        return -VX_EROFS;
    }
    vnode->mode = mode & 07777;
    return vnode->ops->set_mode ? vnode->ops->set_mode(vnode) : 0;
}

int vfs_chmod(const char *path, size_t length, uint32_t mode) {
    vfs_lock();
    struct walk walk = {0};
    struct vnode *vnode;
    int error = resolve(&walk, path, length, false, true, &vnode, NULL, NULL);
    walk_done(&walk);
    if (!error) {
        error = set_mode_locked(vnode, mode);
        vnode_put(vnode);
    }
    vfs_unlock();
    return error;
}

int vfs_file_chmod(struct file *file, uint32_t mode) {
    vfs_lock();
    int error = set_mode_locked(file->vnode, mode);
    vfs_unlock();
    return error;
}

int vfs_statfs(const char *path, size_t length, uint64_t *total, uint64_t *free,
               const char **fs_name) {
    vfs_lock();
    struct walk walk = {0};
    struct vnode *vnode;
    int error = resolve(&walk, path, length, false, true, &vnode, NULL, NULL);
    walk_done(&walk);
    if (!error) {
        struct mount *mount = vnode->mount;
        *total = *free = 0;
        if (mount->root->ops->statfs) {
            mount->root->ops->statfs(mount, total, free);
        }
        *fs_name = mount->fs_name;
        vnode_put(vnode);
    }
    vfs_unlock();
    return error;
}

uint64_t vfs_share_page(struct file *file, uint64_t index) {
    if (!file->vnode->ops->share_page) {
        return 0;
    }
    vfs_lock();
    uint64_t phys = file->vnode->ops->share_page(file->vnode, index);
    vfs_unlock();
    return phys;
}

int vfs_truncate(struct file *file, uint64_t size) {
    struct vnode *vnode = file->vnode;
    if (vnode->type == VX_TYPE_DIRECTORY) {
        return -VX_EISDIR;
    }
    if (vnode->type != VX_TYPE_FILE) {
        return -VX_EINVAL;
    }
    vfs_lock();
    int error = vnode->mount->read_only || !vnode->ops->truncate
                    ? -VX_EROFS
                    : vnode->ops->truncate(vnode, size);
    vfs_unlock();
    return error;
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
    case VX_ELOOP: return "too many symbolic links";
    case VX_EINVAL: return "invalid argument";
    case VX_EFAULT: return "bad address";
    default: return "error";
    }
}
