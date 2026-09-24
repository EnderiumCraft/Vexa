#ifndef VEXA_FS_H
#define VEXA_FS_H

#include <stddef.h>

struct block_device;

/* File system types, registered with the VFS by fs_init(). */
void fs_init(void);

/* devfs: adds /dev/<name> for a block device. */
void devfs_add_block_device(struct block_device *device);

/* Unpacks a tar archive (the initramfs) into the root file system. */
int initramfs_unpack(const void *data, size_t size);

/* The current time as seconds since 1970, from the CMOS clock (dev/rtc.c). */
long long time_now(void);
void rtc_init(void);

#endif
