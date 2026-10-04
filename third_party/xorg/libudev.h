/* A stand-in for udev's library, for the X server's evdev driver, which asks
 * it only whether an input device is a virtual one. Vexa has no udev:
 * udev_new() fails, and the driver takes the device as a real one. */
#ifndef VEXA_LIBUDEV_H
#define VEXA_LIBUDEV_H

#include <stddef.h>
#include <sys/types.h>

struct udev;
struct udev_device;

static inline struct udev *udev_new(void) {
    return NULL;
}

static inline struct udev *udev_unref(struct udev *udev) {
    (void)udev;
    return NULL;
}

static inline struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type,
                                                              dev_t devnum) {
    (void)udev, (void)type, (void)devnum;
    return NULL;
}

static inline const char *udev_device_get_devpath(struct udev_device *device) {
    (void)device;
    return NULL;
}

static inline struct udev_device *udev_device_unref(struct udev_device *device) {
    (void)device;
    return NULL;
}

#endif
