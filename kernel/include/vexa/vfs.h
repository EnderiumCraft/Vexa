#ifndef VEXA_VFS_H
#define VEXA_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>
#include <vexa/object.h>

/*
 * Virtual file system: one directory tree, with file systems mounted in it.
 *
 * Every file, directory and device is a vnode. File systems provide the vnodes
 * and their operations; the VFS does path lookup, mounting and open files.
 *
 * All file system operations run under one lock (vfs_lock), a sleeping mutex,
 * so they may wait for disk I/O. Simple rather than fast; finer-grained
 * locking can come once there are workloads to measure.
 */

struct vnode;
struct mount;
struct block_device;

struct vnode_ops {
    /* Directories. `name` is not NUL-terminated. Returned vnodes carry a reference. */
    int (*lookup)(struct vnode *dir, const char *name, size_t length, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, size_t length, uint32_t type,
                  struct vnode **out);
    int (*remove)(struct vnode *dir, const char *name, size_t length);
    /* Moves an entry, replacing any file (or empty directory) at the target.
     * Both directories are on this file system. */
    int (*rename)(struct vnode *old_dir, const char *old_name, size_t old_length,
                  struct vnode *new_dir, const char *new_name, size_t new_length);
    /* Fills *entry and returns 1, or returns 0 at the end. *cookie starts at 0. */
    int (*read_dir)(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry);
    /* Files and devices. Return bytes transferred or a negative VX_E* error. */
    int64_t (*read)(struct vnode *vnode, void *buffer, size_t size, uint64_t offset);
    int64_t (*write)(struct vnode *vnode, const void *buffer, size_t size, uint64_t offset);
    int (*truncate)(struct vnode *vnode, uint64_t size);
    /* The last reference is gone. */
    void (*release)(struct vnode *vnode);
};

struct vnode {
    uint32_t type; /* VX_TYPE_* */
    uint32_t refs;
    uint64_t inode;
    uint64_t size;
    uint32_t links;
    int64_t modified;
    const struct vnode_ops *ops;
    struct mount *mount;        /* The file system this vnode belongs to. */
    struct mount *mounted_here; /* A file system mounted on this directory. */
    void *data;                 /* File system specific. */
};

struct mount {
    char path[64];
    char source[32];  /* e.g. "vda1", "tmpfs" */
    const char *fs_name;
    bool read_only;
    struct vnode *root;
    struct vnode *mountpoint; /* NULL for the root file system. */
    void *data;
    struct mount *next;
};

struct filesystem_type {
    const char *name;
    /* Sets mount->root and mount->data. `device` is NULL for virtual file systems. */
    int (*mount)(struct mount *mount, struct block_device *device);
};

/* An open file: a vnode plus a position and the way it was opened. */
struct file {
    struct object object;
    struct vnode *vnode;
    uint64_t offset;
    uint32_t flags; /* VX_OPEN_* */
    char *path;     /* As opened (absolute), for fchdir and openat. */
};

extern const struct object_type file_object_type;

/* For file systems: set up a new vnode with one reference. */
void vnode_init(struct vnode *vnode, struct mount *mount, uint32_t type,
                const struct vnode_ops *ops);
void vnode_ref(struct vnode *vnode);
void vnode_put(struct vnode *vnode); /* Call with vfs_lock held. */

void vfs_lock(void);
void vfs_unlock(void);

/* Mounts a file system of type `fs` at `path` (the first mount, at "/", becomes
 * the root). */
int vfs_mount(const char *fs, struct block_device *device, const char *source, const char *path);
struct mount *vfs_mounts(void); /* The list of mounts (read with vfs_lock held). */

/* Operations by path. Paths are (pointer, length) pairs. */
int vfs_open(const char *path, size_t length, uint32_t flags, struct file **out);
int vfs_stat(const char *path, size_t length, struct vx_stat *stat);
int vfs_mkdir(const char *path, size_t length);
int vfs_remove(const char *path, size_t length);
int vfs_rename(const char *from, size_t from_length, const char *to, size_t to_length);

/* Operations on open files. Buffers are kernel memory. */
int64_t vfs_read(struct file *file, void *buffer, size_t size);
int64_t vfs_pread(struct file *file, void *buffer, size_t size, uint64_t offset);
int64_t vfs_write(struct file *file, const void *buffer, size_t size);
int64_t vfs_seek(struct file *file, int64_t offset, int whence);
int vfs_read_dir(struct file *file, struct vx_dir_entry *entry);
void vfs_file_stat(struct file *file, struct vx_stat *stat);
void vfs_close(struct file *file);
int vfs_truncate(struct file *file, uint64_t size);
/* True if the file is the terminal (/dev/console or /dev/tty). */
bool vfs_is_terminal(struct file *file);

void vfs_register_filesystem(const struct filesystem_type *type);
const char *vfs_error_name(int error);

#endif
