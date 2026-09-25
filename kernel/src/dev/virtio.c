#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/virtio.h>

/* What the virtio drivers share: finding the registers and the handshake. */

#define CAP_VENDOR 0x09
#define CFG_COMMON 1
#define CFG_NOTIFY 2
#define CFG_DEVICE 4

/* Finds a virtio capability of the given type; returns its config space offset. */
static uint8_t find_cap(struct pci_device *pci, uint8_t cfg_type) {
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

bool virtio_find(struct pci_device *pci, struct virtio_device *device) {
    uint8_t common_cap = find_cap(pci, CFG_COMMON);
    uint8_t notify_cap = find_cap(pci, CFG_NOTIFY);
    uint8_t device_cap = find_cap(pci, CFG_DEVICE);
    if (!common_cap || !notify_cap || !device_cap) {
        return false;
    }
    pci_enable(pci);
    device->pci = pci;
    device->common = cap_address(pci, common_cap);
    device->notify_base = cap_address(pci, notify_cap);
    device->notify_multiplier = pci_read32(pci, notify_cap + 16);
    device->device_config = cap_address(pci, device_cap);
    return device->common && device->notify_base && device->device_config;
}

bool virtio_negotiate(struct virtio_device *device, uint32_t wanted, uint32_t *features) {
    volatile struct virtio_common_cfg *common = device->common;
    /* Reset, then say hello. */
    common->device_status = 0;
    while (common->device_status != 0) {
    }
    common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    common->device_feature_select = 1;
    uint32_t features_high = common->device_feature;
    common->device_feature_select = 0;
    uint32_t features_low = common->device_feature;
    if (!(features_high & (1U << (VIRTIO_FEATURE_VERSION_1 - 32)))) {
        kprintf("[virtio] device doesn't speak virtio 1.0, skipping\n");
        return false;
    }
    *features = features_low & wanted;
    common->driver_feature_select = 0;
    common->driver_feature = *features;
    common->driver_feature_select = 1;
    common->driver_feature = 1U << (VIRTIO_FEATURE_VERSION_1 - 32);
    common->device_status =
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    if (!(common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio] device rejected our features, skipping\n");
        return false;
    }
    return true;
}

volatile uint16_t *virtio_queue_notify(struct virtio_device *device) {
    return (volatile uint16_t *)(device->notify_base +
                                 device->common->queue_notify_off * device->notify_multiplier);
}

void *virtio_dma_page(void) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return NULL;
    }
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys_to_virt(phys);
}
