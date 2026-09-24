#include <vexa/abi.h>
#include <vexa/block.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/string.h>

/*
 * virtio block device (the virtual disk QEMU, KVM and others provide), through
 * the modern virtio 1.0 PCI interface. One request queue, one request at a
 * time: each request is three descriptors (header, data, status byte).
 */

#define VIRTIO_VENDOR 0x1af4
#define VIRTIO_BLK_TRANSITIONAL 0x1001
#define VIRTIO_BLK_MODERN 0x1042

#define CAP_VENDOR 0x09
#define CFG_COMMON 1
#define CFG_NOTIFY 2
#define CFG_DEVICE 4

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER 2
#define STATUS_DRIVER_OK 4
#define STATUS_FEATURES_OK 8

#define FEATURE_VERSION_1 32 /* Bit number in the 64-bit feature set. */
#define BLK_FEATURE_RO 5

#define DESC_NEXT 1
#define DESC_WRITE 2 /* The device writes this buffer. */

#define REQUEST_IN 0
#define REQUEST_OUT 1

#define QUEUE_SIZE 16
#define MSIX_NO_VECTOR 0xffff

struct __attribute__((packed)) common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

struct __attribute__((packed)) descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

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
    volatile struct common_cfg *common;
    volatile uint16_t *notify;
    struct descriptor *descriptors;
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

    disk->descriptors[0] = (struct descriptor){
        .address = virt_to_phys(disk->header), .length = sizeof(struct request_header),
        .flags = DESC_NEXT, .next = 1};
    disk->descriptors[1] = (struct descriptor){
        .address = virt_to_phys(buffer), .length = length,
        .flags = DESC_NEXT | (type == REQUEST_IN ? DESC_WRITE : 0), .next = 2};
    disk->descriptors[2] = (struct descriptor){
        .address = virt_to_phys((void *)disk->status), .length = 1, .flags = DESC_WRITE};

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

/* Finds a virtio capability of the given type; returns its config space offset. */
static uint8_t find_virtio_cap(struct pci_device *pci, uint8_t cfg_type) {
    for (uint8_t cap = pci_find_capability(pci, CAP_VENDOR, 0); cap;
         cap = pci_find_capability(pci, CAP_VENDOR, cap)) {
        if (pci_read8(pci, cap + 3) == cfg_type) {
            return cap;
        }
    }
    return 0;
}

static volatile void *cap_address(struct pci_device *pci, uint8_t cap) {
    uint8_t bar = pci_read8(pci, cap + 4);
    uint32_t offset = pci_read32(pci, cap + 8);
    volatile uint8_t *base = pci_map_bar(pci, bar);
    return base ? base + offset : NULL;
}

static void *alloc_dma_page(void) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys_to_virt(phys);
}

static void probe(struct pci_device *pci) {
    uint8_t common_cap = find_virtio_cap(pci, CFG_COMMON);
    uint8_t notify_cap = find_virtio_cap(pci, CFG_NOTIFY);
    uint8_t device_cap = find_virtio_cap(pci, CFG_DEVICE);
    if (!common_cap || !notify_cap || !device_cap || disk_count == MAX_DISKS) {
        kprintf("[virtio] %x:%x.%u: no modern virtio interface, skipping\n", pci->bus,
                pci->slot, pci->function);
        return;
    }
    pci_enable(pci);
    struct virtio_disk *disk = kzalloc(sizeof(*disk));
    if (!disk) {
        return;
    }
    disk->common = cap_address(pci, common_cap);
    volatile uint8_t *notify_base = cap_address(pci, notify_cap);
    uint32_t notify_multiplier = pci_read32(pci, notify_cap + 16);
    volatile uint64_t *capacity = cap_address(pci, device_cap);
    volatile struct common_cfg *common = disk->common;

    /* Reset, then say hello. */
    common->device_status = 0;
    while (common->device_status != 0) {
    }
    common->device_status = STATUS_ACKNOWLEDGE | STATUS_DRIVER;

    common->device_feature_select = 1;
    uint32_t features_high = common->device_feature;
    common->device_feature_select = 0;
    uint32_t features_low = common->device_feature;
    if (!(features_high & (1U << (FEATURE_VERSION_1 - 32)))) {
        kprintf("[virtio] device doesn't speak virtio 1.0, skipping\n");
        kfree(disk);
        return;
    }
    bool read_only = features_low & (1U << BLK_FEATURE_RO);
    common->driver_feature_select = 0;
    common->driver_feature = read_only ? (1U << BLK_FEATURE_RO) : 0;
    common->driver_feature_select = 1;
    common->driver_feature = 1U << (FEATURE_VERSION_1 - 32);
    common->device_status = STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK;
    if (!(common->device_status & STATUS_FEATURES_OK)) {
        kprintf("[virtio] device rejected our features, skipping\n");
        kfree(disk);
        return;
    }

    /* Queue 0: descriptor table, driver (avail) ring and device (used) ring. */
    disk->descriptors = alloc_dma_page();
    disk->avail = alloc_dma_page();
    disk->used = alloc_dma_page();
    disk->header = alloc_dma_page();
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
    disk->notify = (volatile uint16_t *)(notify_base + common->queue_notify_off * notify_multiplier);

    disks[disk_count++] = disk;
    if (pci_enable_msi(pci, virtio_interrupt)) {
        common->msix_config = MSIX_NO_VECTOR;
        common->queue_msix_vector = 0; /* MSI-X table entry 0. */
        disk->waiter.has_interrupt = common->queue_msix_vector == 0;
    }
    common->queue_enable = 1;
    common->device_status =
        STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK;

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
