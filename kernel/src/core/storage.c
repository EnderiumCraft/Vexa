#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/pci.h>
#include <vexa/storage.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

void virtio_blk_init(void);                     /* dev/virtio_blk.c */
void ahci_init(void);                           /* dev/ahci.c */
void nvme_init(void);                           /* dev/nvme.c */
void ata_init(void);                            /* dev/ata.c */
bool ext2_probe(struct block_device *device);   /* fs/ext2.c */
bool iso9660_probe(struct block_device *device); /* fs/iso9660.c */

/* Mounts an ext2 file system or a CD at /mnt/<device>, e.g. /mnt/vda1 or
 * /mnt/cd0. The first CD with Vexa on it (the boot CD, normally) is also
 * /cdrom: /linux/usr and the like point there, and Doom's game files. */
bool storage_root_on_disk;

static bool already_mounted(struct block_device *device) {
    bool found = false;
    vfs_lock();
    for (struct mount *m = vfs_mounts(); m && !found; m = m->next) {
        found = strcmp(m->source, device->name) == 0;
    }
    vfs_unlock();
    return found;
}

static void mount_one(struct block_device *device) {
    if (already_mounted(device)) {
        return; /* (The root file system, say.) */
    }
    const char *fs = ext2_probe(device) ? "ext2" : iso9660_probe(device) ? "iso9660" : NULL;
    if (!fs) {
        return;
    }
    char path[32] = "/mnt/";
    size_t n = strlen(device->name);
    memcpy(path + 5, device->name, n + 1);
    vfs_mkdir(path, 5 + n);
    int error = vfs_mount(fs, device, device->name, path);
    if (error) {
        kprintf("[storage] could not mount %s: %s\n", device->name, vfs_error_name(error));
        return;
    }
    kprintf("[storage] mounted %s at %s\n", device->name, path);
    block_update_details(device->parent ? device->parent : device);
    struct vx_stat stat;
    char kernel[48];
    ksnprintf(kernel, sizeof(kernel), "%s/boot/vexa-kernel", path);
    if (fs[0] == 'i' && !storage_root_on_disk && vfs_stat("/cdrom", 6, &stat) != 0 &&
        vfs_stat(kernel, strlen(kernel), &stat) == 0) {
        vfs_symlink(path, "/cdrom", 6);
        kprintf("[storage] /cdrom is %s\n", path);
    }
}

void storage_mount_disk(struct block_device *disk) {
    for (struct block_device *device = block_first(); device; device = device->next) {
        if (device == disk || device->parent == disk) {
            mount_one(device);
        }
    }
}

static void mount_disks(void) {
    for (struct block_device *device = block_first(); device; device = device->next) {
        mount_one(device);
    }
}

static bool probed;

void storage_probe(void) {
    if (probed) {
        return;
    }
    probed = true;
    pci_init();
    virtio_blk_init();
    ahci_init();
    nvme_init();
    ata_init();
}

/* An ext2 file system's UUID ("8-4-4-4-12" hex digits) and name. */
static bool hex_uuid(const char *text, uint8_t out[16]) {
    int n = 0;
    for (const char *p = text; *p && n < 32; p++) {
        int digit = *p >= '0' && *p <= '9'   ? *p - '0'
                    : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
                    : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10
                                             : -1;
        if (*p == '-') {
            continue;
        }
        if (digit < 0) {
            return false;
        }
        out[n / 2] = (uint8_t)(n % 2 ? out[n / 2] | digit : digit << 4);
        n++;
    }
    return n == 32;
}

struct block_device *storage_find_root(const char *spec) {
    uint8_t uuid[16];
    bool by_uuid = strncmp(spec, "UUID=", 5) == 0 && hex_uuid(spec + 5, uuid);
    for (struct block_device *device = block_first(); device; device = device->next) {
        if (!by_uuid) {
            if (strcmp(device->name, spec) == 0) {
                return device;
            }
            continue;
        }
        uint8_t super[128];
        if (block_read_bytes(device, 1024, super, sizeof(super)) == 0 &&
            super[56] == 0x53 && super[57] == 0xef && memcmp(super + 104, uuid, 16) == 0) {
            return device;
        }
    }
    return NULL;
}

void storage_init(void) {
    storage_probe();
    mount_disks();
}
