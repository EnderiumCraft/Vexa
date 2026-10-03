/*
 * The USB core: what's plugged in, what it is, and which driver has it.
 *
 * Plugging and unplugging happen on one kernel thread, "usb", that runs
 * work queued by the controllers (a root port changed) and by hubs (one of
 * their ports changed). A new device gets an address, its descriptors are
 * read (device, configuration, strings), it's configured, and each of its
 * interfaces goes to the first class driver that takes it. A device that's
 * unplugged goes away with everything under it (a hub's devices).
 */
#include <vexa/arch.h>
#include <vexa/device.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/usb.h>

/* ---- The "usb" thread and its work ---- */

#define WORK_MAX 128

struct work {
    void (*fn)(void *arg);
    void *arg;
};

static struct work queue[WORK_MAX];
static uint32_t queue_head, queue_tail;
static struct spinlock queue_lock = SPINLOCK_INIT;
static struct wait_queue queue_waiters = WAIT_QUEUE_INIT;
static bool thread_started;

void usb_queue_work(void (*fn)(void *arg), void *arg) {
    uint64_t flags = spin_lock_irqsave(&queue_lock);
    uint32_t next = (queue_head + 1) % WORK_MAX;
    if (next != queue_tail) {
        queue[queue_head] = (struct work){fn, arg};
        queue_head = next;
    } else {
        kprintf("[usb] too much to do: a change was dropped\n");
    }
    spin_unlock_irqrestore(&queue_lock, flags);
    wait_queue_wake_all(&queue_waiters);
}

static bool work_waiting(void *unused) {
    (void)unused;
    return queue_head != queue_tail;
}

static void usb_thread(void *unused) {
    (void)unused;
    for (;;) {
        wait_queue_wait(&queue_waiters, work_waiting, NULL);
        uint64_t flags = spin_lock_irqsave(&queue_lock);
        struct work work = queue[queue_tail];
        queue_tail = (queue_tail + 1) % WORK_MAX;
        spin_unlock_irqrestore(&queue_lock, flags);
        work.fn(work.arg);
    }
}

/* ---- Transfers, for drivers ---- */

int usb_control(struct usb_device *device, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length) {
    if (device->gone) {
        return -VX_ENODEV;
    }
    uint8_t setup[8] = {
        request_type, request, (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8),
    };
    return device->hc->ops->control(device->hc, device, setup, data, length);
}

int usb_bulk(struct usb_device *device, uint8_t endpoint, void *data, uint32_t length,
             uint32_t timeout_ms) {
    if (device->gone) {
        return -VX_ENODEV;
    }
    return device->hc->ops->bulk(device->hc, device, endpoint, data, length, timeout_ms);
}

int usb_clear_halt(struct usb_device *device, uint8_t endpoint) {
    /* (The controller's side of the endpoint is restarted after the stall.) */
    return usb_control(device, USB_DIR_OUT | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0,
                       endpoint, NULL, 0);
}

int usb_interrupt_in(struct usb_device *device, uint8_t endpoint, uint16_t size,
                     usb_report_fn callback, void *arg) {
    if (device->gone) {
        return -VX_ENODEV;
    }
    return device->hc->ops->interrupt_in(device->hc, device, endpoint, size, callback, arg);
}

static uint16_t language;

void usb_string(struct usb_device *device, uint8_t index, char *out, int size) {
    out[0] = '\0';
    if (!index || size < 2) {
        return;
    }
    uint8_t buffer[256];
    if (!language) {
        int n = usb_control(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_STRING << 8, 0,
                            buffer, 4);
        language = n >= 4 ? (uint16_t)(buffer[2] | buffer[3] << 8) : 0x0409;
    }
    int n = usb_control(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                        (uint16_t)(USB_DESC_STRING << 8 | index), language, buffer, 255);
    if (n < 2 || buffer[1] != USB_DESC_STRING) {
        return;
    }
    int length = buffer[0] < n ? buffer[0] : n, j = 0;
    for (int i = 2; i + 1 < length && j < size - 1; i += 2) {
        uint16_t c = (uint16_t)(buffer[i] | buffer[i + 1] << 8);
        out[j++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    while (j > 0 && out[j - 1] == ' ') {
        j--; /* (Some devices pad their names.) */
    }
    out[j] = '\0';
}

void usb_device_name(struct usb_device *device, char *out, int size) {
    if (device->product[0] && device->manufacturer[0] &&
        strncmp(device->product, device->manufacturer, strlen(device->manufacturer)) != 0) {
        ksnprintf(out, (size_t)size, "%s %s", device->manufacturer, device->product);
    } else if (device->product[0]) {
        ksnprintf(out, (size_t)size, "%s", device->product);
    } else {
        const char *vendor = device_vendor_name(device->descriptor.vendor_id);
        ksnprintf(out, (size_t)size, "%s%sUSB device", vendor ? vendor : "", vendor ? " " : "");
    }
}

const char *usb_speed_name(enum usb_speed speed) {
    switch (speed) {
    case USB_SPEED_LOW: return "low speed (1.5 Mbit/s)";
    case USB_SPEED_FULL: return "full speed (12 Mbit/s)";
    case USB_SPEED_HIGH: return "high speed (480 Mbit/s)";
    default: return "SuperSpeed (5 Gbit/s)";
    }
}

/* ---- New devices ---- */

/* The interfaces (first alternate setting) and their endpoints. */
static void parse_config(struct usb_device *device) {
    const uint8_t *p = device->config, *end = device->config + device->config_length;
    struct usb_interface *current = NULL;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        uint8_t type = p[1];
        if (type == USB_DESC_INTERFACE && p[0] >= sizeof(struct usb_interface_descriptor)) {
            const struct usb_interface_descriptor *d = (const void *)p;
            current = NULL;
            if (d->alternate == 0 && device->interface_count < USB_MAX_INTERFACES) {
                current = &device->interfaces[device->interface_count++];
                current->device = device;
                current->number = d->number;
                current->interface_class = d->interface_class;
                current->subclass = d->interface_subclass;
                current->protocol = d->interface_protocol;
                current->extra = p + p[0];
            }
        } else if (type == USB_DESC_ENDPOINT && current &&
                   p[0] >= sizeof(struct usb_endpoint_descriptor) &&
                   current->endpoint_count < USB_MAX_ENDPOINTS) {
            const struct usb_endpoint_descriptor *d = (const void *)p;
            if (current->endpoint_count == 0) {
                /* Class descriptors (HID's) come before the endpoints. */
                current->extra_length = (int)(p - current->extra);
            }
            struct usb_endpoint *e = &current->endpoints[current->endpoint_count++];
            e->address = d->address;
            e->type = d->attributes & 3;
            e->max_packet = d->max_packet;
            e->interval = d->interval;
        } else if (type == 0x30 && current && current->endpoint_count && p[0] >= 6) {
            /* SuperSpeed endpoint companion: bursts. */
            current->endpoints[current->endpoint_count - 1].max_burst = p[2];
        }
        p += p[0];
    }
}

static const struct usb_driver *const drivers[] = {
    &usb_hub_driver,
    &usb_hid_driver,
    &usb_storage_driver,
};

static const char *class_name(uint8_t c) {
    switch (c) {
    case 1: return "audio";
    case 2: return "communications";
    case 3: return "input (HID)";
    case 6: return "imaging";
    case 7: return "printer";
    case 8: return "mass storage";
    case 9: return "hub";
    case 0x0a: return "CDC data";
    case 0x0b: return "smart card";
    case 0x0e: return "video";
    case 0xe0: return "wireless";
    case 0xef: return "miscellaneous";
    case 0xff: return "vendor specific";
    default: return "unknown";
    }
}

static int read_descriptors(struct usb_device *device) {
    struct usb_device_descriptor *d = &device->descriptor;
    int n = usb_control(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, d, 8);
    if (n < 8) {
        return n < 0 ? n : -VX_EIO;
    }
    uint16_t packet0 = device->speed == USB_SPEED_SUPER ? (uint16_t)(1U << d->max_packet0)
                                                        : d->max_packet0;
    if (packet0 && device->hc->ops->set_max_packet0(device->hc, device, packet0) < 0) {
        return -VX_EIO;
    }
    n = usb_control(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, d,
                    sizeof(*d));
    if (n < (int)sizeof(*d)) {
        return n < 0 ? n : -VX_EIO;
    }
    struct usb_config_descriptor config;
    n = usb_control(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, &config,
                    sizeof(config));
    if (n < (int)sizeof(config)) {
        return n < 0 ? n : -VX_EIO;
    }
    uint16_t length = config.total_length < PAGE_SIZE ? config.total_length : PAGE_SIZE;
    device->config = kzalloc(length);
    if (!device->config) {
        return -VX_ENOMEM;
    }
    n = usb_control(device, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0,
                    device->config, length);
    if (n < (int)sizeof(config)) {
        return n < 0 ? n : -VX_EIO;
    }
    device->config_length = (uint16_t)n;
    usb_string(device, d->manufacturer, device->manufacturer, sizeof(device->manufacturer));
    usb_string(device, d->product, device->product, sizeof(device->product));
    usb_string(device, d->serial, device->serial, sizeof(device->serial));
    n = usb_control(device, USB_DIR_OUT, USB_REQ_SET_CONFIGURATION, config.value, 0, NULL, 0);
    if (n < 0) {
        return n;
    }
    parse_config(device);
    return 0;
}

/* A hub must say how many ports it has before the controller configures it. */
static void hub_details(struct usb_device *device) {
    bool hub = device->descriptor.device_class == USB_CLASS_HUB;
    for (int i = 0; i < device->interface_count; i++) {
        hub = hub || device->interfaces[i].interface_class == USB_CLASS_HUB;
    }
    if (!hub) {
        return;
    }
    uint8_t descriptor[16] = {0};
    uint8_t type = device->speed == USB_SPEED_SUPER ? USB_DESC_SS_HUB : USB_DESC_HUB;
    int n = usb_control(device, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                        USB_REQ_GET_DESCRIPTOR, (uint16_t)(type << 8), 0, descriptor,
                        sizeof(descriptor));
    if (n < 3) {
        return;
    }
    device->is_hub = true;
    device->hub_ports = descriptor[2] < 15 ? descriptor[2] : 15;
    device->hub_multi_tt = device->descriptor.device_protocol == 2;
}

void usb_enumerate(struct usb_hc *hc, struct usb_device *parent, int port, enum usb_speed speed) {
    if ((parent && parent->depth >= 5) || port < 1 || port > 15 + (parent ? 0 : 49)) {
        return; /* (USB allows five hubs deep.) */
    }
    struct usb_device *device = kzalloc(sizeof(*device));
    if (!device) {
        return;
    }
    device->hc = hc;
    device->speed = speed;
    device->parent = parent;
    device->port = (uint8_t)port;
    if (parent) {
        device->depth = parent->depth + 1;
        device->root_port = parent->root_port;
        device->route = parent->route | (uint32_t)port << (4 * parent->depth);
        if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
            if (parent->speed == USB_SPEED_HIGH) {
                device->tt_hub = parent;
                device->tt_port = (uint8_t)port;
            } else {
                device->tt_hub = parent->tt_hub;
                device->tt_port = parent->tt_port;
            }
        }
        parent->children[port - 1] = device;
    } else {
        device->root_port = (uint8_t)port;
        hc->root[port - 1] = device;
    }

    const char *failed = NULL;
    if (hc->ops->address_device(hc, device) < 0) {
        failed = "it didn't take an address";
    } else {
        thread_sleep_ms(10);
        int error = read_descriptors(device);
        if (error) {
            failed = "its descriptors couldn't be read";
        } else {
            hub_details(device);
            if (hc->ops->configure(hc, device) < 0) {
                failed = "it couldn't be configured";
            }
        }
    }

    char name[64];
    usb_device_name(device, name, sizeof(name));
    struct device *under = parent ? parent->node : hc->node;
    device->node = device_add(under, VX_BUS_USB, device->is_hub ? VX_DEVICE_USB_HUB : VX_DEVICE_OTHER,
                              failed ? "Unknown USB device" : name);
    device_set_ids(device->node, device->descriptor.vendor_id, device->descriptor.product_id);
    device_set_flags(device->node, VX_DEVICE_REMOVABLE);
    if (parent) {
        device_set_location(device->node, "port %d of a hub", port);
    } else {
        device_set_location(device->node, "USB port %d", port);
    }
    if (failed) {
        device_set_details(device->node, "%s, %s", usb_speed_name(speed), failed);
        kprintf("[usb] port %d: a device, but %s\n", port, failed);
        return; /* (It stays, so unplugging it is noticed.) */
    }

    /* Each interface to the first driver that takes it. */
    const char *driver = NULL;
    uint8_t first_class = device->descriptor.device_class;
    for (int i = 0; i < device->interface_count; i++) {
        struct usb_interface *interface = &device->interfaces[i];
        if (!first_class) {
            first_class = interface->interface_class;
        }
        for (unsigned d = 0; d < sizeof(drivers) / sizeof(drivers[0]); d++) {
            if (drivers[d]->probe(interface)) {
                interface->driver = drivers[d];
                driver = driver ? driver : drivers[d]->name;
                break;
            }
        }
    }
    if (driver) {
        device_set_driver(device->node, driver);
    }
    if (device->is_hub) {
        device_set_details(device->node, "%s, %d ports", usb_speed_name(speed), device->hub_ports);
    } else {
        device_set_details(device->node, "%s, %s%s%s", usb_speed_name(speed), class_name(first_class),
                           device->serial[0] ? ", serial " : "", device->serial);
    }
    kprintf("[usb] %s %d: %s (%04x:%04x), %s, %s\n", parent ? "hub port" : "port", port, name,
            device->descriptor.vendor_id, device->descriptor.product_id,
            usb_speed_name(speed), driver ? driver : "no driver");
}

/* ---- Unplugging ---- */

static void free_device(void *arg) {
    struct usb_device *device = arg;
    kfree(device->config);
    kfree(device);
}

/* Freed after the work already queued (a hub's port check may still name it). */
static void free_later(void *arg) {
    usb_queue_work(free_device, arg);
}

void usb_disconnect(struct usb_device *device) {
    for (int i = 0; i < 16; i++) {
        if (device->children[i]) {
            usb_disconnect(device->children[i]);
        }
    }
    char name[64];
    usb_device_name(device, name, sizeof(name));
    kprintf("[usb] %s unplugged\n", name);
    device->gone = true;
    device->hc->ops->free_device(device->hc, device);
    thread_sleep_ms(20); /* (A report being handled as it went finishes.) */
    for (int i = 0; i < device->interface_count; i++) {
        struct usb_interface *interface = &device->interfaces[i];
        if (interface->driver && interface->driver->disconnect) {
            interface->driver->disconnect(interface);
        }
    }
    device_remove(device->node);
    if (device->parent) {
        device->parent->children[device->port - 1] = NULL;
    } else {
        device->hc->root[device->port - 1] = NULL;
    }
    usb_queue_work(free_later, device);
}

/* ---- Root ports ---- */

struct port_change {
    struct usb_hc *hc;
    int port;
};

static void root_port_work(void *arg) {
    struct port_change *change = arg;
    struct usb_hc *hc = change->hc;
    int port = change->port;
    kfree(change);
    bool connected = hc->ops->port_connected(hc, port);
    struct usb_device *existing = hc->root[port - 1];
    if (existing && !connected) {
        usb_disconnect(existing);
    } else if (!existing && connected) {
        thread_sleep_ms(50); /* Let the connection settle. */
        int speed = hc->ops->reset_port(hc, port);
        if (speed > 0) {
            usb_enumerate(hc, NULL, port, (enum usb_speed)speed);
        }
    }
}

void usb_port_changed(struct usb_hc *hc, int port) {
    struct port_change *change = kzalloc(sizeof(*change));
    if (change) {
        change->hc = hc;
        change->port = port;
        usb_queue_work(root_port_work, change);
    }
}

void usb_add_controller(struct usb_hc *hc) {
    if (!thread_started) {
        thread_started = true;
        thread_create("usb", usb_thread, NULL);
    }
    for (int port = 1; port <= hc->ports; port++) {
        if (hc->ops->port_connected(hc, port)) {
            usb_port_changed(hc, port);
        }
    }
}

void usb_init(void) {
    xhci_init();
}
