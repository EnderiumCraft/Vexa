#ifndef VEXA_USB_H
#define VEXA_USB_H

#include <stdbool.h>
#include <stdint.h>

/*
 * USB: host controllers (dev/usb/xhci.c, ehci.c, uhci.c, ohci.c) find devices on their ports; the
 * core (dev/usb/usb.c) reads what each device is, configures it and hands
 * its interfaces to class drivers (hubs, HID keyboards and mice, mass
 * storage). Plugging and unplugging is handled by one kernel thread, "usb",
 * so drivers may sleep in probe and disconnect.
 */

struct device;
struct usb_hc;

enum usb_speed { USB_SPEED_LOW = 1, USB_SPEED_FULL, USB_SPEED_HIGH, USB_SPEED_SUPER };

/* Standard requests and descriptors. */
#define USB_DIR_OUT 0x00
#define USB_DIR_IN 0x80
#define USB_TYPE_STANDARD 0x00
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_DEVICE 0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02
#define USB_RECIP_OTHER 0x03

#define USB_REQ_GET_STATUS 0
#define USB_REQ_CLEAR_FEATURE 1
#define USB_REQ_SET_FEATURE 3
#define USB_REQ_SET_ADDRESS 5
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_SET_INTERFACE 11

#define USB_DESC_DEVICE 1
#define USB_DESC_CONFIG 2
#define USB_DESC_STRING 3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT 5
#define USB_DESC_HID 0x21
#define USB_DESC_REPORT 0x22
#define USB_DESC_HUB 0x29
#define USB_DESC_SS_HUB 0x2a

#define USB_CLASS_HID 3
#define USB_CLASS_STORAGE 8
#define USB_CLASS_HUB 9

#define USB_ENDPOINT_CONTROL 0
#define USB_ENDPOINT_ISOCHRONOUS 1
#define USB_ENDPOINT_BULK 2
#define USB_ENDPOINT_INTERRUPT 3

struct __attribute__((packed)) usb_device_descriptor {
    uint8_t length, type;
    uint16_t usb_version;
    uint8_t device_class, device_subclass, device_protocol, max_packet0;
    uint16_t vendor_id, product_id, device_version;
    uint8_t manufacturer, product, serial, configurations;
};

struct __attribute__((packed)) usb_config_descriptor {
    uint8_t length, type;
    uint16_t total_length;
    uint8_t interfaces, value, name, attributes, max_power;
};

struct __attribute__((packed)) usb_interface_descriptor {
    uint8_t length, type;
    uint8_t number, alternate, endpoints, interface_class, interface_subclass,
        interface_protocol, name;
};

struct __attribute__((packed)) usb_endpoint_descriptor {
    uint8_t length, type;
    uint8_t address; /* Bit 7: IN. */
    uint8_t attributes; /* Bits 0-1: USB_ENDPOINT_* */
    uint16_t max_packet;
    uint8_t interval;
};

#define USB_MAX_INTERFACES 8
#define USB_MAX_ENDPOINTS 8

struct usb_endpoint {
    uint8_t address;    /* With USB_DIR_IN for IN endpoints. */
    uint8_t type;       /* USB_ENDPOINT_* */
    uint16_t max_packet;
    uint8_t interval;
    uint8_t max_burst;  /* SuperSpeed: from the companion descriptor. */
};

struct usb_device;
struct usb_driver;

struct usb_interface {
    struct usb_device *device;
    uint8_t number, interface_class, subclass, protocol;
    struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
    int endpoint_count;
    const uint8_t *extra;  /* Its part of the configuration descriptor (class descriptors). */
    int extra_length;
    const struct usb_driver *driver;
    void *driver_data;
    struct device *node;   /* What the driver made of it (a disk...), if it says so. */
};

struct usb_device {
    struct usb_hc *hc;
    void *hc_data;            /* The controller's (its slot). */
    enum usb_speed speed;
    struct usb_device *parent; /* The hub it's on; NULL on a root port. */
    uint8_t port;             /* On the hub (or the controller's root port number). */
    uint8_t root_port;        /* The root port it's under. */
    uint8_t depth;            /* 0 on a root port. */
    uint32_t route;           /* Route string: ports below the root port, 4 bits each. */
    /* Low and full speed devices behind a high speed hub: its transaction translator. */
    struct usb_device *tt_hub;
    uint8_t tt_port;
    bool is_hub;
    uint8_t hub_ports;
    bool hub_multi_tt;
    struct usb_device_descriptor descriptor;
    uint8_t *config;          /* The whole configuration descriptor. */
    uint16_t config_length;
    char manufacturer[48], product[48], serial[40];
    struct usb_interface interfaces[USB_MAX_INTERFACES];
    int interface_count;
    struct device *node;
    bool gone;                /* Unplugged: transfers fail. */
    struct usb_device *children[16]; /* Hubs: what's on each port (index port - 1). */
    volatile bool check_queued;       /* Hubs: a look at the ports is already waiting. */
};

struct usb_driver {
    const char *name;
    /* Takes the interface if it's the driver's (may sleep). */
    bool (*probe)(struct usb_interface *interface);
    /* The device is gone (may sleep); free what probe made. */
    void (*disconnect)(struct usb_interface *interface);
};

/* ---- For class drivers ---- */

/* A control transfer on endpoint 0. `data` is any kernel memory. Returns
 * the bytes transferred, or a negative VX_E* error (-VX_EPIPE: stalled). */
int usb_control(struct usb_device *device, uint8_t request_type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length);
/* A bulk transfer, waiting for it to finish (up to timeout_ms). `data` must
 * be in the direct map (kmalloc'd or pages). Returns the bytes
 * transferred or a negative error. */
int usb_bulk(struct usb_device *device, uint8_t endpoint, void *data, uint32_t length,
             uint32_t timeout_ms);
/* Clears a stalled endpoint (CLEAR_FEATURE ENDPOINT_HALT, and the
 * controller's side). */
int usb_clear_halt(struct usb_device *device, uint8_t endpoint);
/* Keeps an interrupt IN endpoint polled: `callback` gets each report (from
 * the controller's thread; it must not sleep for long). */
typedef void (*usb_report_fn)(void *arg, const uint8_t *data, int length);
int usb_interrupt_in(struct usb_device *device, uint8_t endpoint, uint16_t size,
                     usb_report_fn callback, void *arg);
/* Keeps an isochronous OUT endpoint (of the alternate setting the driver
 * chose with SET_INTERFACE) sending: `fill` writes each packet (at most
 * `packet` bytes) and returns its length, from the controller's thread, a
 * few packets ahead of when it goes. Only on xHCI: -VX_ENOSYS elsewhere. */
typedef int (*usb_iso_fill_fn)(void *arg, uint8_t *data, int max);
int usb_iso_out(struct usb_device *device, const struct usb_endpoint *endpoint, uint16_t packet,
                usb_iso_fill_fn fill, void *arg);
void usb_iso_stop(struct usb_device *device, uint8_t endpoint);
/* A string descriptor as ASCII (others become '?'); "" if it has none. */
void usb_string(struct usb_device *device, uint8_t index, char *out, int size);
/* "Logitech USB Optical Mouse": the device's names, for driver nodes. */
void usb_device_name(struct usb_device *device, char *out, int size);

/* ---- For host controllers ---- */

struct usb_hc_ops {
    /* Gives a new device (speed, route, ports set) an address: endpoint 0 works after. */
    int (*address_device)(struct usb_hc *hc, struct usb_device *device);
    /* Endpoint 0's max packet size, once read from the device descriptor. */
    int (*set_max_packet0)(struct usb_hc *hc, struct usb_device *device, uint16_t size);
    /* The configuration's endpoints (after SET_CONFIGURATION); hubs' details too. */
    int (*configure)(struct usb_hc *hc, struct usb_device *device);
    void (*free_device)(struct usb_hc *hc, struct usb_device *device);
    int (*control)(struct usb_hc *hc, struct usb_device *device, const uint8_t setup[8],
                   void *data, uint16_t length);
    int (*bulk)(struct usb_hc *hc, struct usb_device *device, uint8_t endpoint, void *data,
                uint32_t length, uint32_t timeout_ms);
    int (*interrupt_in)(struct usb_hc *hc, struct usb_device *device, uint8_t endpoint,
                        uint16_t size, usb_report_fn callback, void *arg);
    int (*iso_out)(struct usb_hc *hc, struct usb_device *device, const struct usb_endpoint *e,
                   uint16_t packet, usb_iso_fill_fn fill, void *arg); /* (Optional.) */
    void (*iso_stop)(struct usb_hc *hc, struct usb_device *device, uint8_t endpoint);
    int (*reset_endpoint)(struct usb_hc *hc, struct usb_device *device, uint8_t endpoint);
    /* After CLEAR_FEATURE ENDPOINT_HALT: the next packet is DATA0 again
     * (controllers that keep data toggles in software; NULL for xHCI). */
    int (*clear_toggle)(struct usb_hc *hc, struct usb_device *device, uint8_t endpoint);
    /* Root ports: resets one (returns its speed, or a negative error). */
    int (*reset_port)(struct usb_hc *hc, int port);
    bool (*port_connected)(struct usb_hc *hc, int port);
};

struct usb_hc {
    const struct usb_hc_ops *ops;
    void *data;
    const char *name;
    int ports;
    struct device *node;
    struct usb_device *root[64]; /* What's on each root port (index port - 1). */
    volatile bool port_queued[64]; /* A check of that port is already waiting. */
};

/* A root port changed (connected or disconnected): checked from the "usb" thread. */
void usb_port_changed(struct usb_hc *hc, int port);
/* A host controller is ready: its ports are looked at. */
void usb_add_controller(struct usb_hc *hc);

/* Class drivers (dev/usb/hub.c, hid.c, storage.c). */
extern const struct usb_driver usb_hub_driver, usb_hid_driver, usb_storage_driver,
    usb_audio_driver;
/* A hub's port changed (from its status endpoint): checked from the "usb" thread. */
void usb_hub_changed(struct usb_device *hub);
/* For the hub driver: a device on a hub's port, or gone from one. */
void usb_enumerate(struct usb_hc *hc, struct usb_device *parent, int port, enum usb_speed speed);
void usb_disconnect(struct usb_device *device);
/* Runs `work` on the "usb" thread. */
void usb_queue_work(void (*work)(void *arg), void *arg);

void usb_init(void); /* Finds the controllers. */
void xhci_init(void);

const char *usb_speed_name(enum usb_speed speed);

#endif
