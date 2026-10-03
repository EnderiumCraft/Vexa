#ifndef VEXA_DEVICE_H
#define VEXA_DEVICE_H

#include <stdint.h>
#include <vexa/abi.h>

/*
 * The device tree: every device the kernel found, and which driver has it,
 * for programs (vx_device_list: Device Manager, `devices`). Buses add what
 * they find (PCI functions, USB devices); drivers name their devices, say
 * they have them, and add what they make of them (a disk, a keyboard) as
 * children. Kinds and buses are VX_DEVICE_* and VX_BUS_* (abi/vexa/abi.h).
 */
struct device;

/* The computer: the top of the tree. */
struct device *device_root(void);
/* Adds a device under `parent` (NULL: the computer). Never fails: if memory
 * runs out it returns a placeholder that ignores everything below. */
struct device *device_add(struct device *parent, unsigned bus, unsigned kind, const char *name);
/* Removes a device and everything under it. */
void device_remove(struct device *device);

void device_set_name(struct device *device, const char *name);
void device_set_kind(struct device *device, unsigned kind);
/* The driver that has it (sets VX_DEVICE_HAS_DRIVER). */
void device_set_driver(struct device *device, const char *driver);
void device_set_ids(struct device *device, uint16_t vendor_id, uint16_t product_id);
void device_set_flags(struct device *device, unsigned flags);
void device_set_location(struct device *device, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void device_set_details(struct device *device, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Copies up to `count` devices into `out` (parents first); returns how many
 * there are, and the tree's generation. */
unsigned device_list(struct vx_device_info *out, unsigned count, uint64_t *generation);

/* Adds the processors (init, once the CPUs are up). */
void device_add_processors(void);

/* Common vendors' names for PCI and USB ids ("Intel"), or NULL. */
const char *device_vendor_name(uint16_t vendor_id);

#endif
