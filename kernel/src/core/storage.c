#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/pci.h>
#include <vexa/storage.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

void virtio_blk_init(void);                     /* dev/virtio_blk.c */
void ahci_init(void);                           /* dev/ahci.c */
void nvme_init(void);                           /* dev/nvme.c */
bool ext2_probe(struct block_device *device);   /* fs/ext2.c */
bool iso9660_probe(struct block_device *device); /* fs/iso9660.c */

/* Mounts every ext2 file system and CD found at /mnt/<device>, e.g.
 * /mnt/vda1 or /mnt/cd0. The first CD with Vexa's Linux files on it (the
 * boot CD, normally) is also /cdrom: /linux/usr and the like point there. */
static void mount_disks(void) {
    for (struct block_device *device = block_first(); device; device = device->next) {
        const char *fs = ext2_probe(device) ? "ext2" : iso9660_probe(device) ? "iso9660" : NULL;
        if (!fs) {
            continue;
        }
        char path[32] = "/mnt/";
        size_t n = strlen(device->name);
        memcpy(path + 5, device->name, n + 1);
        vfs_mkdir(path, 5 + n);
        int error = vfs_mount(fs, device, device->name, path);
        if (error) {
            kprintf("[storage] could not mount %s: %s\n", device->name, vfs_error_name(error));
            continue;
        }
        kprintf("[storage] mounted %s at %s\n", device->name, path);
        struct vx_stat stat;
        char linux_dir[40];
        ksnprintf(linux_dir, sizeof(linux_dir), "%s/linux", path);
        if (fs[0] == 'i' && vfs_stat("/cdrom", 6, &stat) != 0 &&
            vfs_stat(linux_dir, strlen(linux_dir), &stat) == 0) {
            vfs_symlink(path, "/cdrom", 6);
            kprintf("[storage] /cdrom is %s\n", path);
        }
    }
}

void storage_init(void) {
    pci_init();
    virtio_blk_init();
    ahci_init();
    nvme_init();
    mount_disks();
}
