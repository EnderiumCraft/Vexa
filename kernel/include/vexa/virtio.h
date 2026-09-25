#ifndef VEXA_VIRTIO_H
#define VEXA_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>
#include <vexa/pci.h>

/* The modern (virtio 1.0) PCI interface, shared by the virtio drivers. */

#define VIRTIO_VENDOR 0x1af4

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VIRTIO_FEATURE_VERSION_1 32 /* Bit number in the 64-bit feature set. */

#define VIRTIO_DESC_NEXT 1
#define VIRTIO_DESC_WRITE 2 /* The device writes this buffer. */

#define VIRTIO_MSIX_NO_VECTOR 0xffff

struct __attribute__((packed)) virtio_common_cfg {
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

struct __attribute__((packed)) virtio_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

/* The device's registers, found through its PCI capabilities. */
struct virtio_device {
    struct pci_device *pci;
    volatile struct virtio_common_cfg *common;
    volatile uint8_t *notify_base;
    uint32_t notify_multiplier;
    volatile void *device_config;
};

/* Finds the registers and enables the device on the bus. False if it has no
 * modern interface. */
bool virtio_find(struct pci_device *pci, struct virtio_device *device);
/* Resets the device and agrees on features: those in `wanted` (low 32 bits)
 * that the device offers, plus VERSION_1. Returns false if the device
 * refuses; *features gets the agreed low 32 bits. */
bool virtio_negotiate(struct virtio_device *device, uint32_t wanted, uint32_t *features);
/* Where to write a queue's index to notify the device about it (with the
 * queue selected). */
volatile uint16_t *virtio_queue_notify(struct virtio_device *device);
/* A zeroed page for rings and buffers, as a kernel pointer. */
void *virtio_dma_page(void);

#endif
