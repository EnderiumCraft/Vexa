#ifndef VEXA_STORAGE_H
#define VEXA_STORAGE_H

/* Finds storage controllers and disks, and mounts the file systems on them.
 * Runs in the init thread (it may wait for devices). */
void storage_init(void);
/* Set when the root file system is a disk's (an installed Vexa): the boot
 * CD, if there is one, is just a CD then (no /cdrom). */
extern bool storage_root_on_disk;
/* Finds the controllers and disks (once), without mounting anything. */
void storage_probe(void);
/* The disk holding the root file system: "UUID=<ext2 uuid>" or a name ("vda2"). */
struct block_device *storage_find_root(const char *spec);
/* Mounts a disk found later (a USB stick) and its partitions. */
struct block_device;
void storage_mount_disk(struct block_device *disk);

#endif
