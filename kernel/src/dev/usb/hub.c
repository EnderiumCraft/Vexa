/*
 * USB hubs (USB 2 and USB 3): ports of their own, which the core enumerates
 * like a controller's. A hub reports which ports changed on its status
 * endpoint; the ports are then looked at from the "usb" thread.
 */
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/sched.h>
#include <vexa/usb.h>

#define HUB_REQUEST (USB_TYPE_CLASS | USB_RECIP_OTHER)
#define REQ_SET_HUB_DEPTH 12

/* Port features (SET_FEATURE / CLEAR_FEATURE on a port). */
#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE 17
#define C_PORT_SUSPEND 18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET 20
#define C_PORT_LINK_STATE 25
#define C_PORT_CONFIG_ERROR 26
#define C_BH_PORT_RESET 29

/* Port status bits. */
#define STATUS_CONNECTED (1U << 0)
#define STATUS_ENABLED (1U << 1)
#define STATUS_RESET (1U << 4)
#define STATUS_LOW_SPEED (1U << 9)
#define STATUS_HIGH_SPEED (1U << 10)
#define CHANGE_RESET (1U << 4)

static bool port_status(struct usb_device *hub, int port, uint16_t *status, uint16_t *change) {
    uint8_t data[4];
    if (usb_control(hub, USB_DIR_IN | HUB_REQUEST, USB_REQ_GET_STATUS, 0, (uint16_t)port, data, 4) <
        4) {
        return false;
    }
    *status = (uint16_t)(data[0] | data[1] << 8);
    *change = (uint16_t)(data[2] | data[3] << 8);
    return true;
}

static void port_feature(struct usb_device *hub, int port, uint8_t request, uint16_t feature) {
    usb_control(hub, USB_DIR_OUT | HUB_REQUEST, request, feature, (uint16_t)port, NULL, 0);
}

static void clear_changes(struct usb_device *hub, int port, uint16_t change) {
    static const uint16_t features[] = {C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_SUSPEND,
                                        C_PORT_OVER_CURRENT, C_PORT_RESET};
    for (int bit = 0; bit < 5; bit++) {
        if (change & (1U << bit)) {
            port_feature(hub, port, USB_REQ_CLEAR_FEATURE, features[bit]);
        }
    }
    if (hub->speed == USB_SPEED_SUPER) {
        if (change & (1U << 5)) {
            port_feature(hub, port, USB_REQ_CLEAR_FEATURE, C_BH_PORT_RESET);
        }
        if (change & (1U << 6)) {
            port_feature(hub, port, USB_REQ_CLEAR_FEATURE, C_PORT_LINK_STATE);
        }
        if (change & (1U << 7)) {
            port_feature(hub, port, USB_REQ_CLEAR_FEATURE, C_PORT_CONFIG_ERROR);
        }
    }
}

/* Resets a port with something on it; returns the device's speed, or 0. */
static enum usb_speed reset_port(struct usb_device *hub, int port) {
    port_feature(hub, port, USB_REQ_SET_FEATURE, PORT_RESET);
    uint16_t status = 0, change = 0;
    for (int tries = 0; tries < 50; tries++) {
        thread_sleep_ms(10);
        if (!port_status(hub, port, &status, &change)) {
            return 0;
        }
        if ((change & CHANGE_RESET) && !(status & STATUS_RESET)) {
            break;
        }
    }
    clear_changes(hub, port, change);
    if (!(status & STATUS_ENABLED) || !(status & STATUS_CONNECTED)) {
        return 0;
    }
    thread_sleep_ms(10); /* Reset recovery. */
    if (hub->speed == USB_SPEED_SUPER) {
        return USB_SPEED_SUPER;
    }
    return (status & STATUS_LOW_SPEED)    ? USB_SPEED_LOW
           : (status & STATUS_HIGH_SPEED) ? USB_SPEED_HIGH
                                          : USB_SPEED_FULL;
}

/* Every port: what's new, and what's gone. */
static void check_ports(void *arg) {
    struct usb_device *hub = arg;
    if (hub->gone) {
        return;
    }
    for (int port = 1; port <= hub->hub_ports; port++) {
        uint16_t status, change;
        if (!port_status(hub, port, &status, &change)) {
            continue;
        }
        clear_changes(hub, port, change);
        struct usb_device *existing = hub->children[port - 1];
        bool connected = status & STATUS_CONNECTED;
        if (existing && (!connected || (change & 1))) {
            usb_disconnect(existing); /* Gone (or replaced by something else). */
            existing = NULL;
        }
        if (!existing && connected) {
            thread_sleep_ms(50); /* Let the connection settle. */
            enum usb_speed speed = reset_port(hub, port);
            if (speed) {
                usb_enumerate(hub->hc, hub, port, speed);
            }
        }
    }
}

void usb_hub_changed(struct usb_device *hub) {
    usb_queue_work(check_ports, hub);
}

static void status_report(void *arg, const uint8_t *data, int length) {
    struct usb_device *hub = arg;
    bool any = false;
    for (int i = 0; i < length; i++) {
        any = any || data[i];
    }
    if (any && !hub->gone) {
        usb_hub_changed(hub);
    }
}

static bool hub_probe(struct usb_interface *interface) {
    struct usb_device *hub = interface->device;
    if (interface->interface_class != USB_CLASS_HUB || !hub->is_hub) {
        return false;
    }
    const struct usb_endpoint *status = NULL;
    for (int i = 0; i < interface->endpoint_count; i++) {
        if (interface->endpoints[i].type == USB_ENDPOINT_INTERRUPT &&
            (interface->endpoints[i].address & USB_DIR_IN)) {
            status = &interface->endpoints[i];
        }
    }
    if (!status) {
        return false;
    }
    if (hub->speed == USB_SPEED_SUPER) {
        usb_control(hub, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE, REQ_SET_HUB_DEPTH,
                    hub->depth, 0, NULL, 0);
    }
    for (int port = 1; port <= hub->hub_ports; port++) {
        port_feature(hub, port, USB_REQ_SET_FEATURE, PORT_POWER);
    }
    thread_sleep_ms(100); /* Power on to power good. */
    uint16_t size = (uint16_t)(1 + hub->hub_ports / 8);
    usb_interrupt_in(hub, status->address, size, status_report, hub);
    usb_hub_changed(hub); /* What's already plugged in. */
    return true;
}

static void hub_disconnect(struct usb_interface *interface) {
    (void)interface; /* (The core unplugs what was on it first.) */
}

const struct usb_driver usb_hub_driver = {
    .name = "usb-hub",
    .probe = hub_probe,
    .disconnect = hub_disconnect,
};
