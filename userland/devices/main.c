/* devices: the devices the kernel found, as a tree, with their drivers.
 *
 *   devices         the tree
 *   devices -l      with each one's ids, location and details
 *   devices usb     only those on a bus or of a kind: usb, pci, disk, input... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

static const char *kind_names[VX_DEVICE_KIND_COUNT] = {
    [VX_DEVICE_OTHER] = "other",          [VX_DEVICE_COMPUTER] = "computer",
    [VX_DEVICE_PROCESSOR] = "processor",  [VX_DEVICE_BRIDGE] = "bridge",
    [VX_DEVICE_DISPLAY] = "display",      [VX_DEVICE_STORAGE] = "storage",
    [VX_DEVICE_DISK] = "disk",            [VX_DEVICE_NETWORK] = "network",
    [VX_DEVICE_SOUND] = "sound",          [VX_DEVICE_KEYBOARD] = "keyboard",
    [VX_DEVICE_POINTER] = "pointer",      [VX_DEVICE_INPUT] = "input",
    [VX_DEVICE_USB_CONTROLLER] = "usb-controller", [VX_DEVICE_USB_HUB] = "usb-hub",
    [VX_DEVICE_SERIAL] = "serial",        [VX_DEVICE_SYSTEM] = "system",
};
static const char *bus_names[] = {"", "platform", "pci", "usb", ""};

static struct vx_device_info *devices;
static long count;
static int long_form;
static const char *only;

static int matches(const struct vx_device_info *d) {
    if (!only) {
        return 1;
    }
    const char *kind = d->kind < VX_DEVICE_KIND_COUNT ? kind_names[d->kind] : "";
    const char *bus = d->bus < sizeof bus_names / sizeof *bus_names ? bus_names[d->bus] : "";
    return strcmp(bus, only) == 0 || strncmp(kind, only, strlen(only)) == 0 ||
           (strcmp(only, "input") == 0 &&
            (d->kind == VX_DEVICE_KEYBOARD || d->kind == VX_DEVICE_POINTER));
}

static void show(const struct vx_device_info *d, const char *prefix, int last, int top) {
    if (!only || matches(d)) {
        printf("%s%s%s", only ? "" : prefix, only || top ? "" : last ? "`- " : "|- ", d->name);
        if (d->driver[0]) {
            printf("  [%s]", d->driver);
        } else if (d->bus == VX_BUS_PCI || d->bus == VX_BUS_USB) {
            printf("  (no driver)");
        }
        printf("\n");
        if (long_form) {
            char indent[160];
            snprintf(indent, sizeof indent, "%s%s", only ? "" : prefix,
                     only || top ? "    " : last ? "      " : "|     ");
            if (d->location[0]) {
                printf("%s%s\n", indent, d->location);
            }
            if (d->vendor_id || d->product_id) {
                printf("%sid %04x:%04x\n", indent, d->vendor_id, d->product_id);
            }
            if (d->details[0]) {
                printf("%s%s\n", indent, d->details);
            }
        }
    }
    char child_prefix[160];
    snprintf(child_prefix, sizeof child_prefix, "%s%s", prefix, top ? "" : last ? "   " : "|  ");
    long children[256], n = 0;
    for (long i = 0; i < count && n < 256; i++) {
        if (devices[i].parent == d->id) {
            children[n++] = i;
        }
    }
    for (long i = 0; i < n; i++) {
        show(&devices[children[i]], child_prefix, i == n - 1, 0);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_form = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: devices [-l] [usb|pci|disk|keyboard|pointer|input|...]\n");
            return 2;
        } else {
            only = argv[i];
        }
    }
    count = vx_device_list(NULL, 0, NULL);
    if (count < 0) {
        fprintf(stderr, "devices: %s\n", vx_strerror(count));
        return 1;
    }
    devices = calloc((size_t)count + 16, sizeof *devices);
    count = vx_device_list(devices, (size_t)count + 16, NULL);
    if (!devices || count < 0) {
        fprintf(stderr, "devices: out of memory\n");
        return 1;
    }
    for (long i = 0; i < count; i++) {
        if (devices[i].parent == 0) {
            show(&devices[i], "", 1, 1);
        }
    }
    return 0;
}
