/*
 * UHCI: Intel's (and VIA's) USB 1 host controllers, alone (older PCs, and
 * QEMU's piix3-usb-uhci) or as the companions of an EHCI controller, which
 * hands them the low and full speed devices on its ports (QEMU's
 * ich9-usb-uhci1 to 3).
 *
 * The controller goes through a list of 1024 frames, one each millisecond.
 * Every frame starts at the same place here: the queue heads (QHs) of the
 * interrupt endpoints, then those of the control and bulk endpoints. Each
 * QH has a chain of transfer descriptors (TDs), one per packet; the driver
 * keeps the endpoints' data toggles. As with EHCI there are no interrupts:
 * waiting threads check their TDs, and the controller's thread checks the
 * interrupt endpoints and the ports.
 */
#include <vexa/arch.h>
#include <vexa/device.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/usb.h>

#include "hcd.h"

/* I/O registers. */
#define REG_USBCMD 0x00
#define REG_USBSTS 0x02
#define REG_USBINTR 0x04
#define REG_FRNUM 0x06
#define REG_FRBASEADD 0x08
#define REG_SOFMOD 0x0c
#define REG_PORTSC(port) (0x10 + 2 * ((port) - 1))

#define CMD_RUN (1U << 0)
#define CMD_RESET (1U << 1)
#define CMD_GLOBAL_RESET (1U << 2)
#define CMD_CONFIGURED (1U << 6)
#define CMD_MAX_PACKET_64 (1U << 7)
#define STS_HALTED (1U << 5)

#define PORT_CONNECTED (1U << 0)
#define PORT_CONNECT_CHANGE (1U << 1)
#define PORT_ENABLED (1U << 2)
#define PORT_ENABLE_CHANGE (1U << 3)
#define PORT_PRESENT (1U << 7) /* (Always 1 on a port that's there.) */
#define PORT_LOW_SPEED (1U << 8)
#define PORT_RESET (1U << 9)
#define PORT_CHANGES (PORT_CONNECT_CHANGE | PORT_ENABLE_CHANGE) /* (Write 1 to clear.) */

#define LEGACY_SUPPORT 0xc0 /* In PCI configuration space. */

/* Link pointers. */
#define LINK_TERMINATE 1U
#define LINK_QH (1U << 1)
#define LINK_DEPTH_FIRST (1U << 2)

/* TD status. */
#define TD_ACTIVE (1U << 23)
#define TD_STALLED (1U << 22) /* (Set with any error that ends the TD.) */
#define TD_BUFFER_ERROR (1U << 21)
#define TD_BABBLE (1U << 20)
#define TD_CRC_TIMEOUT (1U << 18)
#define TD_BITSTUFF (1U << 17)
#define TD_IOC (1U << 24)
#define TD_LOW_SPEED (1U << 26)
#define TD_RETRIES (3U << 27)
#define TD_SHORT_PACKET_DETECT (1U << 29)
#define TD_LENGTH(status) (((status) + 1) & 0x7ff) /* (n - 1 is stored.) */

#define PID_SETUP 0x2d
#define PID_IN 0x69
#define PID_OUT 0xe1

struct td {
    uint32_t link, status, token, buffer;
};

struct qh {
    uint32_t head, element;
    uint32_t pad[6];
};

#define MAX_CONTROLLERS 8
#define MAX_PORTS 8

struct uhci;

struct endpoint {
    uint8_t address; /* With USB_DIR_IN. */
    uint8_t type;
    uint16_t max_packet;
    uint8_t device_address;
    bool low_speed;
    bool toggle;      /* The next packet's: DATA1 if true. */
    void *page;       /* The QH, then TDs. */
    unsigned order;
    struct qh *qh;
    struct td *tds;
    int td_count;
    uint8_t *bounce;
    unsigned bounce_order;
    struct mutex lock;
    usb_report_fn callback;
    void *arg;
    uint16_t size;
    struct endpoint *next;
};

struct slot {
    int address;
    struct endpoint *endpoints[32];
};

struct uhci {
    struct usb_hc usb;
    struct pci_device *pci;
    uint16_t io;
    int ports;
    uint32_t *frames;
    struct qh *interrupt_anchor, *async_anchor;
    struct endpoint *interrupts, *async;
    struct mutex lock;
    struct hcd_addresses addresses;
};

static uint16_t port_read(struct uhci *hc, int port) {
    return inw((uint16_t)(hc->io + REG_PORTSC(port)));
}

static void port_write(struct uhci *hc, int port, uint16_t value) {
    outw((uint16_t)(hc->io + REG_PORTSC(port)), value);
}

static int endpoint_index(uint8_t address) {
    return (address & 0xf) | ((address & USB_DIR_IN) && (address & 0xf) ? 16 : 0);
}

/* ---- Endpoints and the schedule ---- */

static struct endpoint *new_endpoint(struct usb_device *device, int address, uint8_t ep_address,
                                     uint8_t type, uint16_t max_packet) {
    struct endpoint *ep = kzalloc(sizeof(*ep));
    if (!ep) {
        return NULL;
    }
    ep->address = ep_address;
    ep->type = type;
    ep->max_packet = max_packet ? max_packet : 8;
    ep->device_address = (uint8_t)address;
    ep->low_speed = device->speed == USB_SPEED_LOW;
    ep->lock = (struct mutex)MUTEX_INIT;
    ep->order = type == USB_ENDPOINT_CONTROL ? 1 : 0;
    ep->bounce_order = type == USB_ENDPOINT_BULK ? 2 : type == USB_ENDPOINT_CONTROL ? 1 : 0;
    ep->page = hcd_pages(ep->order);
    ep->bounce = hcd_pages(ep->bounce_order);
    if (!ep->page || !ep->bounce) {
        hcd_free_pages(ep->page, ep->order);
        hcd_free_pages(ep->bounce, ep->bounce_order);
        kfree(ep);
        return NULL;
    }
    ep->qh = ep->page;
    ep->tds = (struct td *)((uint8_t *)ep->page + sizeof(struct qh));
    ep->td_count = (int)(((PAGE_SIZE << ep->order) - sizeof(struct qh)) / sizeof(struct td));
    ep->qh->element = LINK_TERMINATE;
    return ep;
}

static void free_endpoint(struct endpoint *ep) {
    hcd_free_pages(ep->page, ep->order);
    hcd_free_pages(ep->bounce, ep->bounce_order);
    kfree(ep);
}

static void link(struct uhci *hc, struct endpoint *ep) {
    mutex_lock(&hc->lock);
    bool periodic = ep->type == USB_ENDPOINT_INTERRUPT;
    struct qh *after = periodic ? hc->interrupt_anchor : hc->async_anchor;
    ep->qh->head = after->head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    after->head = hcd_phys(ep->qh) | LINK_QH;
    struct endpoint **list = periodic ? &hc->interrupts : &hc->async;
    ep->next = *list;
    *list = ep;
    mutex_unlock(&hc->lock);
}

static void unlink(struct uhci *hc, struct endpoint *ep) {
    mutex_lock(&hc->lock);
    bool periodic = ep->type == USB_ENDPOINT_INTERRUPT;
    struct endpoint **list = periodic ? &hc->interrupts : &hc->async;
    struct qh *before = periodic ? hc->interrupt_anchor : hc->async_anchor;
    for (struct endpoint **p = list; *p; p = &(*p)->next) {
        if (*p == ep) {
            *p = ep->next;
            break;
        }
    }
    for (struct endpoint *e = *list; e; e = e->next) {
        if ((e->qh->head & ~0xfU) == hcd_phys(ep->qh)) {
            before = e->qh;
        }
    }
    before->head = ep->qh->head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    thread_sleep_ms(2); /* (No frame is still on it.) */
    mutex_unlock(&hc->lock);
}

/* ---- Transfers ---- */

/* Fills TDs from `first` for `length` bytes in packets (at least one, for
 * a zero-length one); returns how many, or -1 if they don't fit. */
static int fill_tds(struct endpoint *ep, int first, uint8_t pid, const uint8_t *buffer,
                    uint32_t length, bool *toggle) {
    int count = 0;
    uint32_t done = 0;
    do {
        if (first + count >= ep->td_count) {
            return -1;
        }
        uint32_t piece = length - done < ep->max_packet ? length - done : ep->max_packet;
        struct td *td = &ep->tds[first + count];
        td->link = hcd_phys(td + 1) | LINK_DEPTH_FIRST;
        td->status = TD_ACTIVE | TD_RETRIES | (ep->low_speed ? TD_LOW_SPEED : 0) |
                     (pid == PID_IN ? TD_SHORT_PACKET_DETECT : 0);
        td->token = pid | (uint32_t)ep->device_address << 8 | (uint32_t)(ep->address & 0xf) << 15 |
                    (uint32_t)*toggle << 19 | ((piece - 1) & 0x7ffU) << 21;
        td->buffer = piece ? hcd_phys(buffer + done) : 0;
        *toggle = !*toggle;
        done += piece;
        count++;
    } while (done < length);
    return count;
}

struct transfer {
    struct td *tds;
    int count;
};

static bool td_short(const struct td *td) {
    uint32_t expected = ((td->token >> 21) + 1) & 0x7ff;
    return (td->token & 0xff) == PID_IN && TD_LENGTH(td->status) < expected;
}

/* Done: every TD, or up to an error or a short packet (which stops the queue). */
static bool transfer_done(void *arg) {
    struct transfer *t = arg;
    for (int i = 0; i < t->count; i++) {
        uint32_t status = __atomic_load_n(&t->tds[i].status, __ATOMIC_ACQUIRE);
        if (status & TD_ACTIVE) {
            return false;
        }
        if ((status & TD_STALLED) || td_short(&t->tds[i])) {
            return true;
        }
    }
    return true;
}

/* Runs `count` TDs from `first`; returns 0 or an error. `finished`: how
 * many went through (the last of them may be short). */
static int run(struct endpoint *ep, int first, int count, uint32_t timeout_ms, int *finished) {
    struct td *tds = &ep->tds[first];
    struct transfer t = {tds, count};
    tds[count - 1].link = LINK_TERMINATE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ep->qh->element = hcd_phys(tds);
    bool done = hcd_wait(transfer_done, &t, timeout_ms);
    ep->qh->element = LINK_TERMINATE;
    *finished = 0;
    if (!done) {
        thread_sleep_ms(2); /* (Until no frame is on a TD.) */
        return -VX_ETIMEDOUT;
    }
    for (int i = 0; i < count; i++) {
        uint32_t status = tds[i].status;
        if (status & TD_ACTIVE) {
            break;
        }
        if (status & TD_STALLED) {
            return (status & (TD_BABBLE | TD_BUFFER_ERROR | TD_CRC_TIMEOUT | TD_BITSTUFF))
                       ? -VX_EIO
                       : -VX_EPIPE;
        }
        (*finished)++;
        if (td_short(&tds[i])) {
            break;
        }
    }
    return 0;
}

/* The bytes the TDs that went through moved. */
static uint32_t moved(struct endpoint *ep, int first, int count) {
    uint32_t total = 0;
    for (int i = first; i < first + count; i++) {
        if (!(ep->tds[i].status & TD_ACTIVE)) {
            total += TD_LENGTH(ep->tds[i].status);
        }
    }
    return total;
}

static int uhci_control(struct usb_hc *usb, struct usb_device *device, const uint8_t setup[8],
                        void *data, uint16_t length) {
    (void)usb;
    struct slot *slot = device->hc_data;
    struct endpoint *ep = slot ? slot->endpoints[0] : NULL;
    if (!ep) {
        return -VX_ENODEV;
    }
    if (length > PAGE_SIZE) {
        return -VX_EINVAL;
    }
    bool in = hcd_setup_in(setup);
    mutex_lock(&ep->lock);
    uint8_t *packet = ep->bounce, *buffer = ep->bounce + PAGE_SIZE;
    memcpy(packet, setup, 8);
    if (!in && length) {
        memcpy(buffer, data, length);
    }
    bool toggle = false;
    fill_tds(ep, 0, PID_SETUP, packet, 8, &toggle);
    toggle = true;
    int data_count = length ? fill_tds(ep, 1, in ? PID_IN : PID_OUT, buffer, length, &toggle) : 0;
    int status = 1 + data_count, result = -VX_EINVAL;
    toggle = true;
    if (data_count >= 0 &&
        fill_tds(ep, status, in && length ? PID_OUT : PID_IN, NULL, 0, &toggle) == 1) {
        int finished;
        result = run(ep, 0, status + 1, 3000, &finished);
        if (result == 0 && finished <= status) {
            /* A short answer stopped the queue before the status stage. */
            result = run(ep, status, 1, 3000, &finished);
        }
        if (result == 0) {
            result = (int)moved(ep, 1, data_count);
            if (in && result > 0) {
                memcpy(data, buffer, (size_t)result);
            }
        }
    }
    mutex_unlock(&ep->lock);
    return result;
}

static struct endpoint *find_endpoint(struct usb_device *device, uint8_t address) {
    struct slot *slot = device->hc_data;
    return slot ? slot->endpoints[endpoint_index(address)] : NULL;
}

static int uhci_bulk(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint, void *data,
                     uint32_t length, uint32_t timeout_ms) {
    (void)usb;
    struct endpoint *ep = find_endpoint(device, endpoint);
    if (!ep || ep->type != USB_ENDPOINT_BULK) {
        return -VX_ENODEV;
    }
    bool in = endpoint & USB_DIR_IN;
    uint32_t chunk = (uint32_t)ep->td_count * ep->max_packet;
    chunk = chunk < (PAGE_SIZE << ep->bounce_order) ? chunk : (PAGE_SIZE << ep->bounce_order);
    mutex_lock(&ep->lock);
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t piece = length - done < chunk ? length - done : chunk;
        if (!in) {
            memcpy(ep->bounce, (uint8_t *)data + done, piece);
        }
        bool toggle = ep->toggle;
        int count = fill_tds(ep, 0, in ? PID_IN : PID_OUT, ep->bounce, piece, &toggle);
        int finished;
        result = run(ep, 0, count, timeout_ms, &finished);
        if (finished & 1) {
            ep->toggle = !ep->toggle;
        }
        if (result < 0) {
            break;
        }
        uint32_t got = moved(ep, 0, finished);
        if (in) {
            memcpy((uint8_t *)data + done, ep->bounce, got);
        }
        done += got;
        if (got < piece) {
            break; /* Short: that's all there is. */
        }
    }
    mutex_unlock(&ep->lock);
    return result < 0 ? result : (int)done;
}

static void queue_report(struct endpoint *ep) {
    bool toggle = ep->toggle;
    fill_tds(ep, 0, PID_IN, ep->bounce, ep->size < ep->max_packet ? ep->size : ep->max_packet,
             &toggle);
    ep->tds[0].link = LINK_TERMINATE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ep->qh->element = hcd_phys(ep->tds);
}

static int uhci_interrupt_in(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint,
                             uint16_t size, usb_report_fn callback, void *arg) {
    struct uhci *hc = usb->data;
    struct endpoint *ep = find_endpoint(device, endpoint | USB_DIR_IN);
    if (!ep || ep->type != USB_ENDPOINT_INTERRUPT || size == 0 || size > PAGE_SIZE) {
        return -VX_EINVAL;
    }
    mutex_lock(&hc->lock);
    ep->size = size;
    ep->arg = arg;
    ep->callback = callback;
    queue_report(ep);
    mutex_unlock(&hc->lock);
    return 0;
}

static int uhci_clear_toggle(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    (void)usb;
    struct endpoint *ep = find_endpoint(device, endpoint);
    if (!ep) {
        return -VX_EINVAL;
    }
    mutex_lock(&ep->lock);
    ep->toggle = false;
    mutex_unlock(&ep->lock);
    return 0;
}

/* ---- Devices ---- */

static int uhci_address_device(struct usb_hc *usb, struct usb_device *device) {
    struct uhci *hc = usb->data;
    struct slot *slot = kzalloc(sizeof(*slot));
    if (!slot) {
        return -VX_ENOMEM;
    }
    mutex_lock(&hc->lock);
    int address = hcd_new_address(&hc->addresses);
    mutex_unlock(&hc->lock);
    struct endpoint *ep0 = address ? new_endpoint(device, 0, 0, USB_ENDPOINT_CONTROL, 8) : NULL;
    if (!ep0) {
        hcd_free_address(&hc->addresses, address);
        kfree(slot);
        return -VX_ENOMEM;
    }
    slot->endpoints[0] = ep0;
    device->hc_data = slot;
    link(hc, ep0);
    uint8_t setup[8] = {USB_DIR_OUT, USB_REQ_SET_ADDRESS, (uint8_t)address, 0, 0, 0, 0, 0};
    int result = uhci_control(usb, device, setup, NULL, 0);
    if (result < 0) {
        unlink(hc, ep0);
        free_endpoint(ep0);
        device->hc_data = NULL;
        kfree(slot);
        hcd_free_address(&hc->addresses, address);
        return result;
    }
    thread_sleep_ms(2);
    slot->address = address;
    ep0->device_address = (uint8_t)address;
    return 0;
}

static int uhci_set_max_packet0(struct usb_hc *usb, struct usb_device *device, uint16_t size) {
    (void)usb;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return -VX_ENODEV;
    }
    slot->endpoints[0]->max_packet = size;
    return 0;
}

static int uhci_configure(struct usb_hc *usb, struct usb_device *device) {
    struct uhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return -VX_ENODEV;
    }
    for (int i = 0; i < device->interface_count; i++) {
        struct usb_interface *interface = &device->interfaces[i];
        for (int j = 0; j < interface->endpoint_count; j++) {
            const struct usb_endpoint *e = &interface->endpoints[j];
            int index = endpoint_index(e->address);
            if ((e->type != USB_ENDPOINT_BULK && e->type != USB_ENDPOINT_INTERRUPT) ||
                slot->endpoints[index]) {
                continue;
            }
            struct endpoint *ep = new_endpoint(device, slot->address, e->address, e->type,
                                               e->max_packet & 0x7ff);
            if (!ep) {
                return -VX_ENOMEM;
            }
            slot->endpoints[index] = ep;
            link(hc, ep);
        }
    }
    return 0;
}

static void uhci_free_device(struct usb_hc *usb, struct usb_device *device) {
    struct uhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return;
    }
    for (int i = 0; i < 32; i++) {
        struct endpoint *ep = slot->endpoints[i];
        if (!ep) {
            continue;
        }
        mutex_lock(&ep->lock);
        mutex_lock(&hc->lock);
        ep->callback = NULL;
        mutex_unlock(&hc->lock);
        unlink(hc, ep);
        mutex_unlock(&ep->lock);
        free_endpoint(ep);
    }
    mutex_lock(&hc->lock);
    hcd_free_address(&hc->addresses, slot->address);
    mutex_unlock(&hc->lock);
    device->hc_data = NULL;
    kfree(slot);
}

static int uhci_reset_endpoint(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    return uhci_clear_toggle(usb, device, endpoint);
}

/* ---- Root ports ---- */

static bool uhci_port_connected(struct usb_hc *usb, int port) {
    return port_read(usb->data, port) & PORT_CONNECTED;
}

static int uhci_reset_port(struct usb_hc *usb, int port) {
    struct uhci *hc = usb->data;
    uint16_t status = port_read(hc, port);
    if (!(status & PORT_CONNECTED)) {
        return -VX_ENODEV;
    }
    port_write(hc, port, (uint16_t)((status & ~PORT_CHANGES) | PORT_RESET));
    thread_sleep_ms(50);
    port_write(hc, port, (uint16_t)(port_read(hc, port) & ~(PORT_CHANGES | PORT_RESET)));
    thread_sleep_ms(1);
    for (int tries = 0; tries < 10; tries++) {
        status = port_read(hc, port);
        if (!(status & PORT_CONNECTED)) {
            return -VX_ENODEV;
        }
        if (status & PORT_CHANGES) {
            port_write(hc, port, (uint16_t)((status & ~PORT_CHANGES) | (status & PORT_CHANGES)));
            continue;
        }
        if (status & PORT_ENABLED) {
            break;
        }
        port_write(hc, port, (uint16_t)((status & ~PORT_CHANGES) | PORT_ENABLED));
        thread_sleep_ms(10);
    }
    if (!(status & PORT_ENABLED)) {
        return -VX_EIO;
    }
    thread_sleep_ms(10); /* Reset recovery. */
    return (status & PORT_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static const struct usb_hc_ops uhci_ops = {
    .address_device = uhci_address_device,
    .set_max_packet0 = uhci_set_max_packet0,
    .configure = uhci_configure,
    .free_device = uhci_free_device,
    .control = uhci_control,
    .bulk = uhci_bulk,
    .interrupt_in = uhci_interrupt_in,
    .reset_endpoint = uhci_reset_endpoint,
    .clear_toggle = uhci_clear_toggle,
    .reset_port = uhci_reset_port,
    .port_connected = uhci_port_connected,
};

/* ---- The controller's thread ---- */

static void poll_thread(void *arg) {
    struct uhci *hc = arg;
    for (;;) {
        thread_sleep_ms(hc->interrupts ? 2 : 20);
        outw((uint16_t)(hc->io + REG_USBSTS), 0x1f); /* (Clear what it says.) */
        for (int port = 1; port <= hc->ports; port++) {
            uint16_t status = port_read(hc, port);
            if (status & PORT_CONNECT_CHANGE) {
                port_write(hc, port, (uint16_t)((status & ~PORT_CHANGES) | PORT_CONNECT_CHANGE));
                usb_port_changed(&hc->usb, port);
            }
        }
        mutex_lock(&hc->lock);
        for (struct endpoint *ep = hc->interrupts; ep; ep = ep->next) {
            uint32_t status = __atomic_load_n(&ep->tds[0].status, __ATOMIC_ACQUIRE);
            if (!ep->callback || (status & TD_ACTIVE)) {
                continue;
            }
            if (status & TD_STALLED) {
                if (!(status & TD_CRC_TIMEOUT)) {
                    ep->callback = NULL; /* Stalled: stop polling. */
                    ep->qh->element = LINK_TERMINATE;
                    continue;
                }
                queue_report(ep); /* (A bad packet: try again.) */
                continue;
            }
            ep->toggle = !ep->toggle;
            ep->callback(ep->arg, ep->bounce, (int)TD_LENGTH(status));
            if (ep->callback) {
                queue_report(ep);
            }
        }
        mutex_unlock(&hc->lock);
    }
}

/* ---- Setting up a controller ---- */

static int count_ports(struct uhci *hc) {
    int ports = 0;
    while (ports < MAX_PORTS) {
        uint16_t status = port_read(hc, ports + 1);
        if (!(status & PORT_PRESENT) || status == 0xffff) {
            break;
        }
        ports++;
    }
    return ports >= 2 ? ports : 2;
}

static int controller_count;

static void probe(struct pci_device *pci) {
    if (controller_count == MAX_CONTROLLERS || !pci->bar_is_io[4] || !pci->bar[4]) {
        return;
    }
    pci_enable(pci);
    pci_write16(pci, 0x04, (uint16_t)(pci_read16(pci, 0x04) | 1)); /* I/O space too. */
    struct uhci *hc = kzalloc(sizeof(*hc));
    if (!hc) {
        return;
    }
    hc->pci = pci;
    hc->io = (uint16_t)pci->bar[4];
    hc->lock = (struct mutex)MUTEX_INIT;
    /* From the firmware: no more legacy keyboard emulation, no SMIs. */
    pci_write16(pci, LEGACY_SUPPORT, 0x8f00);
    outw((uint16_t)(hc->io + REG_USBINTR), 0);
    outw((uint16_t)(hc->io + REG_USBCMD), CMD_GLOBAL_RESET);
    thread_sleep_ms(50);
    outw((uint16_t)(hc->io + REG_USBCMD), 0);
    thread_sleep_ms(10);
    outw((uint16_t)(hc->io + REG_USBCMD), CMD_RESET);
    for (int tries = 0; tries < 100 && (inw((uint16_t)(hc->io + REG_USBCMD)) & CMD_RESET); tries++) {
        thread_sleep_ms(1);
    }

    hc->frames = hcd_pages(0);
    uint8_t *page = hcd_pages(0);
    if (!hc->frames || !page) {
        kprintf("[uhci] out of memory\n");
        return;
    }
    hc->interrupt_anchor = (struct qh *)page;
    hc->async_anchor = (struct qh *)(page + sizeof(struct qh));
    hc->async_anchor->head = LINK_TERMINATE;
    hc->async_anchor->element = LINK_TERMINATE;
    hc->interrupt_anchor->head = hcd_phys(hc->async_anchor) | LINK_QH;
    hc->interrupt_anchor->element = LINK_TERMINATE;
    for (int i = 0; i < 1024; i++) {
        hc->frames[i] = hcd_phys(hc->interrupt_anchor) | LINK_QH;
    }
    outw((uint16_t)(hc->io + REG_USBINTR), 0);
    outw((uint16_t)(hc->io + REG_FRNUM), 0);
    outl((uint16_t)(hc->io + REG_FRBASEADD), hcd_phys(hc->frames));
    outb((uint16_t)(hc->io + REG_SOFMOD), 64);
    outw((uint16_t)(hc->io + REG_USBSTS), 0x3f);
    outw((uint16_t)(hc->io + REG_USBCMD), CMD_RUN | CMD_CONFIGURED | CMD_MAX_PACKET_64);
    thread_sleep_ms(2);
    if (inw((uint16_t)(hc->io + REG_USBSTS)) & STS_HALTED) {
        kprintf("[uhci] the controller didn't start\n");
        return;
    }
    hc->ports = count_ports(hc);

    hc->usb.ops = &uhci_ops;
    hc->usb.data = hc;
    hc->usb.name = "uhci";
    hc->usb.ports = hc->ports;
    hc->usb.node = pci->node;
    controller_count++;
    pci_claim(pci, "uhci", NULL);
    device_set_details(pci->node, "USB 1, %d ports, polled", hc->ports);
    kprintf("[uhci] %d ports\n", hc->ports);
    thread_create("uhci", poll_thread, hc);
    usb_add_controller(&hc->usb);
}

void uhci_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x0c && pci->subclass == 0x03 && pci->prog_if == 0x00) {
            probe(pci);
        }
    }
}
