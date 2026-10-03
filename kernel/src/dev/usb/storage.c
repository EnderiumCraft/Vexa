/* USB mass storage (bulk-only transport, SCSI commands): USB sticks and disks. */
#include <vexa/usb.h>

static bool storage_probe(struct usb_interface *interface) {
    (void)interface;
    return false;
}

const struct usb_driver usb_storage_driver = {
    .name = "usb-storage",
    .probe = storage_probe,
};
