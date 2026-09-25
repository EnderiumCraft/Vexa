#ifndef VEXA_FS_H
#define VEXA_FS_H

#include <stddef.h>

struct block_device;

/* File system types, registered with the VFS by fs_init(). */
void fs_init(void);

/* devfs: adds /dev/<name> for a block device. */
void devfs_add_block_device(struct block_device *device);
/* devfs: adds a character device at /dev/<path> (e.g. "input/event0"),
 * making directories as needed. `data` is the driver's, for devfs_data(). */
struct vnode_ops;
struct vnode;
void devfs_add(const char *path, const struct vnode_ops *ops, void *data);
void *devfs_data(struct vnode *vnode);
/* A directory whose entries its `ops` provide (lookup, read_dir), such as
 * /dev/pts; devfs_parent answers its "..". */
void devfs_add_directory(const char *path, const struct vnode_ops *ops);
int devfs_parent(struct vnode *dir, struct vnode **out);

/* Unpacks a tar archive (the initramfs) into the root file system. */
int initramfs_unpack(const void *data, size_t size);

/* The current time as seconds since 1970, from the CMOS clock (dev/rtc.c). */
long long time_now(void);
void rtc_init(void);

#endif
