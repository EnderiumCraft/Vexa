/* The device tree (see <vexa/device.h>). */
#include <stdarg.h>
#include <vexa/device.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

struct device {
    struct vx_device_info info;
    struct device *parent;
    struct device *next; /* In order of addition: parents come first. */
    bool placeholder;
};

static struct spinlock lock = SPINLOCK_INIT;
static struct device *devices; /* The computer, then the rest. */
static struct device **tail = &devices;
static unsigned next_id = 1;
static uint64_t generation = 1;
static struct device placeholder = {.placeholder = true};

static void copy(char *to, size_t size, const char *from) {
    size_t n = strlen(from);
    n = n < size - 1 ? n : size - 1;
    memcpy(to, from, n);
    to[n] = '\0';
}

static struct device *new_device(struct device *parent, unsigned bus, unsigned kind,
                                 const char *name) {
    struct device *device = kzalloc(sizeof(*device));
    if (!device) {
        return &placeholder;
    }
    device->parent = parent;
    device->info.bus = (unsigned short)bus;
    device->info.kind = (unsigned short)kind;
    copy(device->info.name, sizeof(device->info.name), name);
    uint64_t flags = spin_lock_irqsave(&lock);
    device->info.id = next_id++;
    device->info.parent = parent ? parent->info.id : 0;
    *tail = device;
    tail = &device->next;
    generation++;
    spin_unlock_irqrestore(&lock, flags);
    return device;
}

struct device *device_root(void) {
    static struct device *root;
    if (!root) {
        root = new_device(NULL, VX_BUS_NONE, VX_DEVICE_COMPUTER, "Computer");
    }
    return root;
}

struct device *device_add(struct device *parent, unsigned bus, unsigned kind, const char *name) {
    if (!parent) {
        parent = device_root();
    }
    if (parent->placeholder) {
        return &placeholder;
    }
    return new_device(parent, bus, kind, name);
}

static bool under(struct device *device, struct device *ancestor) {
    for (; device; device = device->parent) {
        if (device == ancestor) {
            return true;
        }
    }
    return false;
}

void device_remove(struct device *device) {
    if (!device || device->placeholder) {
        return;
    }
    struct device *removed = NULL;
    uint64_t flags = spin_lock_irqsave(&lock);
    /* Children come after their parents, so one pass finds them all. */
    for (struct device **link = &devices; *link;) {
        struct device *d = *link;
        if (under(d, device)) {
            *link = d->next;
            d->next = removed;
            removed = d;
        } else {
            link = &d->next;
        }
    }
    tail = &devices;
    while (*tail) {
        tail = &(*tail)->next;
    }
    generation++;
    spin_unlock_irqrestore(&lock, flags);
    /* (Freed last-in first, after the list no longer reaches them; the
     * parent pointers of later ones point into this set only.) */
    while (removed) {
        struct device *next = removed->next;
        kfree(removed);
        removed = next;
    }
}

/* Changes under the lock, and a new generation. */
#define CHANGE(device, statement)                                                         \
    do {                                                                                  \
        if (!(device) || (device)->placeholder) {                                         \
            return;                                                                       \
        }                                                                                 \
        uint64_t flags_ = spin_lock_irqsave(&lock);                                       \
        statement;                                                                        \
        generation++;                                                                     \
        spin_unlock_irqrestore(&lock, flags_);                                            \
    } while (0)

void device_set_name(struct device *device, const char *name) {
    CHANGE(device, copy(device->info.name, sizeof(device->info.name), name));
}

void device_set_kind(struct device *device, unsigned kind) {
    CHANGE(device, device->info.kind = (unsigned short)kind);
}

void device_set_driver(struct device *device, const char *driver) {
    CHANGE(device, {
        copy(device->info.driver, sizeof(device->info.driver), driver);
        device->info.flags |= VX_DEVICE_HAS_DRIVER;
    });
}

void device_set_ids(struct device *device, uint16_t vendor_id, uint16_t product_id) {
    CHANGE(device, {
        device->info.vendor_id = vendor_id;
        device->info.product_id = product_id;
    });
}

void device_set_flags(struct device *device, unsigned flags) {
    CHANGE(device, device->info.flags |= flags);
}

void device_set_location(struct device *device, const char *format, ...) {
    char text[sizeof(device->info.location)];
    va_list args;
    va_start(args, format);
    kvsnprintf(text, sizeof(text), format, args);
    va_end(args);
    CHANGE(device, copy(device->info.location, sizeof(device->info.location), text));
}

void device_set_details(struct device *device, const char *format, ...) {
    char text[sizeof(device->info.details)];
    va_list args;
    va_start(args, format);
    kvsnprintf(text, sizeof(text), format, args);
    va_end(args);
    CHANGE(device, copy(device->info.details, sizeof(device->info.details), text));
}

unsigned device_list(struct vx_device_info *out, unsigned count, uint64_t *generation_out) {
    unsigned total = 0;
    device_root();
    uint64_t flags = spin_lock_irqsave(&lock);
    for (struct device *d = devices; d; d = d->next) {
        if (total < count) {
            out[total] = d->info;
        }
        total++;
    }
    *generation_out = generation;
    spin_unlock_irqrestore(&lock, flags);
    return total;
}

const char *device_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x1002: return "AMD/ATI";
    case 0x10ec: return "Realtek";
    case 0x14e4: return "Broadcom";
    case 0x168c: return "Qualcomm Atheros";
    case 0x1b21: return "ASMedia";
    case 0x1106: return "VIA";
    case 0x104c: return "Texas Instruments";
    case 0x1912: return "Renesas";
    case 0x1033: return "NEC";
    case 0x15ad: return "VMware";
    case 0x80ee: return "VirtualBox";
    case 0x1af4: return "Red Hat (virtio)";
    case 0x1b36: return "Red Hat (QEMU)";
    case 0x1234: return "QEMU";
    case 0x0627: return "QEMU";   /* (USB) */
    case 0x046d: return "Logitech";
    case 0x045e: return "Microsoft";
    case 0x05ac: return "Apple";
    case 0x0781: return "SanDisk";
    case 0x0951: return "Kingston";
    case 0x090c: return "Silicon Motion";
    case 0x058f: return "Alcor Micro";
    case 0x0bda: return "Realtek";   /* (USB) */
    case 0x1d6b: return "Linux Foundation";
    case 0x413c: return "Dell";
    case 0x04f2: return "Chicony";
    case 0x046a: return "Cherry";
    case 0x1532: return "Razer";
    case 0x0409: return "NEC";       /* (USB) */
    case 0x05e3: return "Genesys Logic";
    case 0x2109: return "VIA Labs";
    case 0x0424: return "Microchip (SMSC)";
    default: return NULL;
    }
}

/* The processors: one entry, with how many and what they are. */
#include <vexa/arch.h>
#include <vexa/cpu.h>

void device_add_processors(void) {
    uint32_t a, b, c, d;
    char brand[49];
    memset(brand, 0, sizeof(brand));
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (uint32_t leaf = 0; leaf < 3; leaf++) {
            cpuid(0x80000002 + leaf, &a, &b, &c, &d);
            memcpy(brand + leaf * 16, &a, 4);
            memcpy(brand + leaf * 16 + 4, &b, 4);
            memcpy(brand + leaf * 16 + 8, &c, 4);
            memcpy(brand + leaf * 16 + 12, &d, 4);
        }
    }
    const char *name = brand;
    while (*name == ' ') {
        name++;
    }
    unsigned online = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        online += cpus[i].online;
    }
    struct device *cpu = device_add(NULL, VX_BUS_NONE, VX_DEVICE_PROCESSOR,
                                    *name ? name : "x86-64 processor");
    device_set_details(cpu, "%u %s", online, online == 1 ? "core" : "cores");
    device_set_driver(cpu, "kernel");
}
