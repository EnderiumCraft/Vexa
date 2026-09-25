#include <vexa/abi.h>
#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/string.h>
#include <vexa/virtio.h>

/*
 * virtio block device (the virtual disk QEMU, KVM and others provide), through
 * the modern virtio 1.0 PCI interface. One request queue, one request at a
 * time: each request is three descriptors (header, data, status byte).
 */

#define VIRTIO_BLK_TRANSITIONAL 0x1001
#define VIRTIO_BLK_MODERN 0x1042

#define BLK_FEATURE_RO 5

#define REQUEST_IN 0
#define REQUEST_OUT 1

#define QUEUE_SIZE 16

struct __attribute__((packed)) avail_ring {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[QUEUE_SIZE];
};

struct __attribute__((packed)) used_ring {
    uint16_t flags;
    uint16_t index;
    struct {
        uint32_t id;
        uint32_t length;
    } ring[QUEUE_SIZE];
};

struct __attribute__((packed)) request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct virtio_disk {
    struct block_device block;
    volatile uint16_t *notify;
    struct virtio_descriptor *descriptors;
    volatile struct avail_ring *avail;
    volatile struct used_ring *used;
    struct request_header *header; /* And the status byte right after it. */
    volatile uint8_t *status;
    uint16_t last_used;
    struct mutex lock;
    struct device_waiter waiter;
};

#define MAX_DISKS 8
static struct virtio_disk *disks[MAX_DISKS];
static int disk_count;

static void virtio_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    for (int i = 0; i < disk_count; i++) {
        device_wake(&disks[i]->waiter);
    }
}

static bool request_done(void *arg) {
    struct virtio_disk *disk = arg;
    return disk->used->index != disk->last_used;
}

static int submit(struct virtio_disk *disk, uint32_t type, uint64_t sector, void *buffer,
                  uint32_t length) {
    mutex_lock(&disk->lock);
    disk->header->type = type;
    disk->header->reserved = 0;
    disk->header->sector = sector;
    *disk->status = 0xff;

    disk->descriptors[0] = (struct virtio_descriptor){
        .address = virt_to_phys(disk->header), .length = sizeof(struct request_header),
        .flags = VIRTIO_DESC_NEXT, .next = 1};
    disk->descriptors[1] = (struct virtio_descriptor){
        .address = virt_to_phys(buffer), .length = length,
        .flags = VIRTIO_DESC_NEXT | (type == REQUEST_IN ? VIRTIO_DESC_WRITE : 0), .next = 2};
    disk->descriptors[2] = (struct virtio_descriptor){
        .address = virt_to_phys((void *)disk->status), .length = 1, .flags = VIRTIO_DESC_WRITE};

    disk->avail->ring[disk->avail->index % QUEUE_SIZE] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Descriptors before the index. */
    disk->avail->index++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *disk->notify = 0; /* Queue 0. */

    device_wait(&disk->waiter, request_done, disk);
    disk->last_used = disk->used->index;
    int result = *disk->status == 0 ? 0 : -VX_EIO;
    mutex_unlock(&disk->lock);
    return result;
}

static int virtio_read(struct block_device *block, uint64_t sector, uint32_t count, void *buffer) {
    return submit((struct virtio_disk *)block, REQUEST_IN, sector, buffer, count * 512);
}

static int virtio_write(struct block_device *block, uint64_t sector, uint32_t count,
                        const void *buffer) {
    return submit((struct virtio_disk *)block, REQUEST_OUT, sector, (void *)buffer, count * 512);
}

static void probe(struct pci_device *pci) {
    struct virtio_device device;
    if (disk_count == MAX_DISKS || !virtio_find(pci, &device)) {
        kprintf("[virtio] %x:%x.%u: no modern virtio interface, skipping\n", pci->bus,
                pci->slot, pci->function);
        return;
    }
    uint32_t features;
    if (!virtio_negotiate(&device, 1U << BLK_FEATURE_RO, &features)) {
        return;
    }
    bool read_only = features & (1U << BLK_FEATURE_RO);
    volatile struct virtio_common_cfg *common = device.common;
    volatile uint64_t *capacity = device.device_config;
    struct virtio_disk *disk = kzalloc(sizeof(*disk));
    if (!disk) {
        return;
    }

    /* Queue 0: descriptor table, driver (avail) ring and device (used) ring. */
    disk->descriptors = virtio_dma_page();
    disk->avail = virtio_dma_page();
    disk->used = virtio_dma_page();
    disk->header = virtio_dma_page();
    if (!disk->descriptors || !disk->avail || !disk->used || !disk->header) {
        kprintf("[virtio] out of memory\n");
        return;
    }
    disk->status = (volatile uint8_t *)(disk->header + 1);
    common->queue_select = 0;
    uint16_t size = common->queue_size < QUEUE_SIZE ? common->queue_size : QUEUE_SIZE;
    common->queue_size = size;
    common->queue_desc = virt_to_phys(disk->descriptors);
    common->queue_driver = virt_to_phys((void *)disk->avail);
    common->queue_device = virt_to_phys((void *)disk->used);
    disk->notify = virtio_queue_notify(&device);

    disks[disk_count++] = disk;
    if (pci_enable_msi(pci, virtio_interrupt)) {
        common->msix_config = VIRTIO_MSIX_NO_VECTOR;
        common->queue_msix_vector = 0; /* MSI-X table entry 0. */
        disk->waiter.has_interrupt = common->queue_msix_vector == 0;
    }
    common->queue_enable = 1;
    common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    /* Name it vda, vdb, ... */
    int number = disk_count - 1;
    memcpy(disk->block.name, "vda", 4);
    disk->block.name[2] = (char)('a' + number);
    disk->block.sector_count = *capacity;
    disk->block.sector_size = 512;
    disk->block.read = virtio_read;
    disk->block.write = read_only ? NULL : virtio_write;
    kprintf("[virtio] %s: virtio disk%s, %s\n", disk->block.name, read_only ? " (read-only)" : "",
            disk->waiter.has_interrupt ? "MSI-X interrupts" : "polling");
    block_register(&disk->block);
}

void virtio_blk_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->vendor_id == VIRTIO_VENDOR &&
            (pci->device_id == VIRTIO_BLK_MODERN || pci->device_id == VIRTIO_BLK_TRANSITIONAL)) {
            probe(pci);
        }
    }
}
