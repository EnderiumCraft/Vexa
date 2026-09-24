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

/* Mounts every ext2 file system found at /mnt/<device>, e.g. /mnt/vda1. */
static void mount_disks(void) {
    for (struct block_device *device = block_first(); device; device = device->next) {
        if (!ext2_probe(device)) {
            continue;
        }
        char path[32] = "/mnt/";
        size_t n = strlen(device->name);
        memcpy(path + 5, device->name, n + 1);
        vfs_mkdir(path, 5 + n);
        int error = vfs_mount("ext2", device, device->name, path);
        if (error) {
            kprintf("[storage] could not mount %s: %s\n", device->name, vfs_error_name(error));
        } else {
            kprintf("[storage] mounted %s at %s\n", device->name, path);
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
