/*
 * EHCI: USB 2 host controllers (PCs from about 2002 to 2012, and QEMU's
 * usb-ehci and ich9-usb-ehci1). High speed devices are EHCI's own; low and
 * full speed ones on its ports are handed to its companion controllers
 * (UHCI or OHCI: dev/usb/uhci.c and ohci.c), which share the ports. Behind a
 * high speed hub (Intel's later chipsets have one built in, "rate matching
 * hubs", instead of companions), they're reached with split transactions
 * through the hub's transaction translator.
 *
 * Each endpoint has a queue head (QH): control and bulk ones in the
 * asynchronous schedule, a ring the controller goes around; interrupt ones in
 * the periodic schedule, visited every frame (1 ms). A transfer is a few
 * qTDs hung on its queue head. The controller has no MSI; its legacy PCI
 * interrupt (finished transfers, errors, port changes) wakes the thread that
 * waits for a transfer and the controller's thread (interrupt endpoints:
 * each report to its callback, then queued again; and port changes).
 * Without a routed interrupt, they check instead.
 */
#include <vexa/arch.h>
#include <vexa/device.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/usb.h>

#include "hcd.h"

/* Capability registers. */
#define CAP_LENGTH 0x00
#define CAP_HCSPARAMS 0x04
#define CAP_HCCPARAMS 0x08

/* Operational registers. */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_USBINTR 0x08
#define OP_SEGMENT 0x10
#define OP_PERIODIC 0x14
#define OP_ASYNC 0x18
#define OP_CONFIGFLAG 0x40
#define OP_PORTSC(port) (0x44 + 4 * ((port) - 1))

#define CMD_RUN (1U << 0)
#define CMD_RESET (1U << 1)
#define CMD_PERIODIC (1U << 4)
#define CMD_ASYNC (1U << 5)
#define CMD_DOORBELL (1U << 6) /* Interrupt on async advance: a QH was unlinked. */
#define CMD_THRESHOLD(n) ((uint32_t)(n) << 16)
#define STS_PORT_CHANGE (1U << 2)
#define STS_SYSTEM_ERROR (1U << 4)
#define STS_DOORBELL (1U << 5)
#define STS_HALTED (1U << 12)
/* The interrupts used (USBINTR and USBSTS): transfers done, transfer errors,
 * port changes, host system errors. (Not the doorbell: it's waited for.) */
#define STS_INTERRUPTS 0x17

#define PORT_CONNECTED (1U << 0)
#define PORT_CONNECT_CHANGE (1U << 1)
#define PORT_ENABLED (1U << 2)
#define PORT_ENABLE_CHANGE (1U << 3)
#define PORT_OC_CHANGE (1U << 5)
#define PORT_RESET (1U << 8)
#define PORT_LINE(portsc) (((portsc) >> 10) & 3) /* 1: K state, a low speed device. */
#define PORT_POWER (1U << 12)
#define PORT_OWNER (1U << 13) /* The companion controller has it. */
#define PORT_CHANGES (PORT_CONNECT_CHANGE | PORT_ENABLE_CHANGE | PORT_OC_CHANGE) /* (Write 1 to clear.) */

/* Link pointers. */
#define LINK_TERMINATE 1U
#define LINK_QH (1U << 1)

/* qTD tokens. */
#define TOKEN_ACTIVE (1U << 7)
#define TOKEN_HALTED (1U << 6)
#define TOKEN_BUFFER_ERROR (1U << 5)
#define TOKEN_BABBLE (1U << 4)
#define TOKEN_TRANSACTION (1U << 3)
#define TOKEN_OUT (0U << 8)
#define TOKEN_IN (1U << 8)
#define TOKEN_SETUP (2U << 8)
#define TOKEN_ERRORS (3U << 10) /* Retries before giving up. */
#define TOKEN_IOC (1U << 15)
#define TOKEN_BYTES(n) ((uint32_t)(n) << 16)
#define TOKEN_REMAINING(token) (((token) >> 16) & 0x7fff)
#define TOKEN_TOGGLE (1U << 31)

/* Queue head words. */
#define INFO1_TOGGLE_FROM_QTD (1U << 14)
#define INFO1_HEAD (1U << 15)    /* The async ring's head. */
#define INFO1_CONTROL (1U << 27) /* A control endpoint that isn't high speed. */
#define INFO2_MULT (1U << 30)

/* A qTD: 32 bytes for the controller (52 with 64-bit addressing), padded. */
struct qtd {
    uint32_t next, alt_next, token, buffer[5], buffer_high[5];
    uint32_t pad[3];
};

/* A queue head, with its overlay (the qTD being worked on). */
struct qh {
    uint32_t link, info1, info2, current;
    uint32_t next, alt_next, token, buffer[5], buffer_high[5];
    uint32_t pad[15];
};

_Static_assert(sizeof(struct qtd) == 64, "qtd");
_Static_assert(sizeof(struct qh) == 128, "qh");

#define QTDS ((PAGE_SIZE - sizeof(struct qh)) / sizeof(struct qtd))
#define BULK_CHUNK 16384 /* One qTD's worth (four pages). */
#define MAX_CONTROLLERS 8

struct ehci;

struct endpoint {
    uint8_t address; /* With USB_DIR_IN. */
    uint8_t type;
    uint16_t max_packet;
    struct qh *qh;   /* The page: the QH, then qTDs. */
    struct qtd *qtds;
    uint8_t *bounce; /* Data goes through here (below 4 GiB). */
    unsigned bounce_order;
    struct mutex lock;
    /* Interrupt endpoints: reports go here. */
    usb_report_fn callback;
    void *arg;
    uint16_t size;
    struct ehci *hc;
    struct endpoint *next; /* In its schedule's list. */
};

struct slot {
    int address;
    struct endpoint *endpoints[32]; /* By number, +16 for IN; 0: endpoint 0. */
};

struct ehci {
    struct usb_hc usb;
    struct pci_device *pci;
    volatile uint8_t *op;
    int ports, companions;
    uint32_t *frames;  /* The periodic frame list. */
    struct qh *head;   /* The async ring's (empty) head. */
    struct qh *anchor; /* Where every frame's list starts. */
    struct endpoint *async, *periodic; /* What's linked behind them. */
    struct mutex lock; /* The schedules and the lists. */
    struct hcd_addresses addresses;
    struct hcd_irq irq;
    volatile bool system_error;
};

static uint32_t read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static bool wait_register(volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value,
                          uint64_t timeout_ms) {
    uint64_t deadline = timer_ms() + timeout_ms;
    while ((read32(base, offset) & mask) != value) {
        if (timer_ms() > deadline) {
            return false;
        }
        thread_sleep_ms(1);
    }
    return true;
}

static int endpoint_index(uint8_t address) {
    return (address & 0xf) | ((address & USB_DIR_IN) && (address & 0xf) ? 16 : 0);
}

/* ---- Queue heads and the schedules ---- */

static uint32_t info1_of(struct usb_device *device, int address, const struct endpoint *ep) {
    uint32_t speed = device->speed == USB_SPEED_HIGH ? 2 : device->speed == USB_SPEED_LOW ? 1 : 0;
    uint32_t info = (uint32_t)address | (uint32_t)(ep->address & 0xf) << 8 | speed << 12 |
                    (uint32_t)ep->max_packet << 16;
    if (ep->type == USB_ENDPOINT_CONTROL) {
        info |= INFO1_TOGGLE_FROM_QTD; /* (Setup DATA0, data DATA1, status DATA1.) */
        if (device->speed != USB_SPEED_HIGH) {
            info |= INFO1_CONTROL;
        }
    }
    return info;
}

static uint32_t info2_of(struct usb_device *device, const struct endpoint *ep) {
    uint32_t info = INFO2_MULT;
    if (device->speed != USB_SPEED_HIGH && device->tt_hub) {
        /* Split transactions through the hub's transaction translator. */
        struct slot *hub = device->tt_hub->hc_data;
        info |= (uint32_t)(hub ? hub->address : 0) << 16 | (uint32_t)device->tt_port << 23;
    }
    if (ep->type == USB_ENDPOINT_INTERRUPT) {
        info |= 0x01; /* Start in microframe 0 of each frame... */
        if (device->speed != USB_SPEED_HIGH) {
            info |= 0x1cU << 8; /* ...and the complete splits in 2 to 4. */
        }
    }
    return info;
}

static struct endpoint *new_endpoint(struct usb_device *device, int address, uint8_t ep_address,
                                     uint8_t type, uint16_t max_packet) {
    struct endpoint *ep = kzalloc(sizeof(*ep));
    if (!ep) {
        return NULL;
    }
    ep->address = ep_address;
    ep->type = type;
    ep->max_packet = max_packet ? max_packet : 8;
    ep->lock = (struct mutex)MUTEX_INIT;
    ep->hc = device->hc->data;
    ep->bounce_order = type == USB_ENDPOINT_BULK ? 2 : type == USB_ENDPOINT_CONTROL ? 1 : 0;
    ep->qh = hcd_pages(0);
    ep->bounce = hcd_pages(ep->bounce_order);
    if (!ep->qh || !ep->bounce) {
        hcd_free_pages(ep->qh, 0);
        hcd_free_pages(ep->bounce, ep->bounce_order);
        kfree(ep);
        return NULL;
    }
    ep->qtds = (struct qtd *)((uint8_t *)ep->qh + sizeof(struct qh));
    ep->qh->info1 = info1_of(device, address, ep);
    ep->qh->info2 = info2_of(device, ep);
    ep->qh->next = LINK_TERMINATE;
    ep->qh->alt_next = LINK_TERMINATE;
    return ep;
}

static void free_endpoint(struct endpoint *ep) {
    hcd_free_pages(ep->qh, 0);
    hcd_free_pages(ep->bounce, ep->bounce_order);
    kfree(ep);
}

static void link(struct ehci *hc, struct endpoint *ep) {
    mutex_lock(&hc->lock);
    bool periodic = ep->type == USB_ENDPOINT_INTERRUPT;
    struct qh *after = periodic ? hc->anchor : hc->head;
    ep->qh->link = after->link;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    after->link = hcd_phys(ep->qh) | LINK_QH;
    struct endpoint **list = periodic ? &hc->periodic : &hc->async;
    ep->next = *list;
    *list = ep;
    mutex_unlock(&hc->lock);
}

/* Takes a QH out of its schedule, and waits until the controller has let go
 * of it. */
static void unlink(struct ehci *hc, struct endpoint *ep) {
    mutex_lock(&hc->lock);
    bool periodic = ep->type == USB_ENDPOINT_INTERRUPT;
    struct endpoint **list = periodic ? &hc->periodic : &hc->async;
    struct qh *before = periodic ? hc->anchor : hc->head;
    for (struct endpoint **p = list; *p; p = &(*p)->next) {
        if (*p == ep) {
            *p = ep->next;
            break;
        }
    }
    /* (The hardware order: newest first, behind the head or the anchor.) */
    for (struct endpoint *e = *list; e; e = e->next) {
        if ((e->qh->link & ~0x1fU) == hcd_phys(ep->qh)) {
            before = e->qh;
        }
    }
    before->link = ep->qh->link;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (periodic) {
        thread_sleep_ms(2); /* (Two frames: no frame is still on it.) */
    } else {
        write32(hc->op, OP_USBSTS, STS_DOORBELL);
        write32(hc->op, OP_USBCMD, read32(hc->op, OP_USBCMD) | CMD_DOORBELL);
        wait_register(hc->op, OP_USBSTS, STS_DOORBELL, STS_DOORBELL, 100);
        write32(hc->op, OP_USBSTS, STS_DOORBELL);
    }
    mutex_unlock(&hc->lock);
}

/* After a halt (or with the QH unlinked): an idle overlay, with the data
 * toggle it had. */
static void reset_overlay(struct endpoint *ep) {
    ep->qh->next = LINK_TERMINATE;
    ep->qh->alt_next = LINK_TERMINATE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ep->qh->token &= TOKEN_TOGGLE;
}

/* ---- Transfers ---- */

static void fill_qtd(struct qtd *qtd, const void *buffer, uint32_t length, uint32_t token) {
    uint32_t phys = buffer ? hcd_phys(buffer) : 0;
    qtd->next = LINK_TERMINATE;
    qtd->alt_next = LINK_TERMINATE;
    qtd->buffer[0] = phys;
    for (int i = 1; i < 5; i++) {
        qtd->buffer[i] = phys ? (phys & ~0xfffU) + (uint32_t)i * PAGE_SIZE : 0;
        qtd->buffer_high[i] = 0;
    }
    qtd->buffer_high[0] = 0;
    qtd->token = token | TOKEN_ACTIVE | TOKEN_ERRORS | TOKEN_BYTES(length);
}

/* The QH (idle) works on `first` and what follows it. */
static void start(struct endpoint *ep, struct qtd *first) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ep->qh->next = hcd_phys(first);
}

struct transfer {
    struct qtd *qtds;
    int count;
};

static bool transfer_done(void *arg) {
    struct transfer *t = arg;
    for (int i = 0; i < t->count; i++) {
        uint32_t token = __atomic_load_n(&t->qtds[i].token, __ATOMIC_ACQUIRE);
        if (token & TOKEN_HALTED) {
            return true;
        }
        if (token & TOKEN_ACTIVE) {
            /* (Not yet, unless a short packet ended it before this one: in
             * a control transfer, the status stage still runs.) */
            return false;
        }
    }
    return true;
}

/* Waits for the qTDs; returns 0 or an error (and makes the QH usable again). */
static int finish(struct ehci *hc, struct endpoint *ep, struct qtd *qtds, int count,
                  uint32_t timeout_ms) {
    struct transfer t = {qtds, count};
    if (!hcd_wait(&hc->irq, transfer_done, &t, timeout_ms)) {
        unlink(hc, ep);
        for (int i = 0; i < count; i++) {
            qtds[i].token &= ~TOKEN_ACTIVE;
        }
        reset_overlay(ep);
        link(hc, ep);
        return -VX_ETIMEDOUT;
    }
    for (int i = 0; i < count; i++) {
        uint32_t token = qtds[i].token;
        if (token & TOKEN_HALTED) {
            reset_overlay(ep);
            return (token & (TOKEN_BABBLE | TOKEN_BUFFER_ERROR | TOKEN_TRANSACTION)) ? -VX_EIO
                                                                                      : -VX_EPIPE;
        }
    }
    return 0;
}

static int ehci_control(struct usb_hc *usb, struct usb_device *device, const uint8_t setup[8],
                        void *data, uint16_t length) {
    struct ehci *hc = usb->data;
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
    struct qtd *q = ep->qtds;
    int count = 0;
    fill_qtd(&q[count++], packet, 8, TOKEN_SETUP);
    int data_index = -1;
    if (length) {
        data_index = count;
        fill_qtd(&q[count++], buffer, length, (in ? TOKEN_IN : TOKEN_OUT) | TOKEN_TOGGLE);
    }
    int status = count;
    fill_qtd(&q[count++], NULL, 0,
             (in && length ? TOKEN_OUT : TOKEN_IN) | TOKEN_TOGGLE | TOKEN_IOC);
    for (int i = 0; i < count - 1; i++) {
        q[i].next = hcd_phys(&q[i + 1]);
    }
    if (data_index >= 0) {
        q[data_index].alt_next = hcd_phys(&q[status]); /* A short answer: then the status. */
    }
    start(ep, q);
    int result = finish(hc, ep, q, count, 3000);
    if (result == 0) {
        result = data_index >= 0 ? (int)(length - TOKEN_REMAINING(q[data_index].token)) : 0;
        if (in && result > 0) {
            memcpy(data, buffer, (size_t)result);
        }
    }
    mutex_unlock(&ep->lock);
    return result;
}

static struct endpoint *find_endpoint(struct usb_device *device, uint8_t address) {
    struct slot *slot = device->hc_data;
    return slot ? slot->endpoints[endpoint_index(address)] : NULL;
}

static int ehci_bulk(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint, void *data,
                     uint32_t length, uint32_t timeout_ms) {
    struct ehci *hc = usb->data;
    struct endpoint *ep = find_endpoint(device, endpoint);
    if (!ep || ep->type != USB_ENDPOINT_BULK) {
        return -VX_ENODEV;
    }
    bool in = endpoint & USB_DIR_IN;
    mutex_lock(&ep->lock);
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t piece = length - done < BULK_CHUNK ? length - done : BULK_CHUNK;
        if (!in) {
            memcpy(ep->bounce, (uint8_t *)data + done, piece);
        }
        fill_qtd(&ep->qtds[0], ep->bounce, piece, (in ? TOKEN_IN : TOKEN_OUT) | TOKEN_IOC);
        start(ep, ep->qtds);
        result = finish(hc, ep, ep->qtds, 1, timeout_ms);
        if (result < 0) {
            break;
        }
        uint32_t moved = piece - TOKEN_REMAINING(ep->qtds[0].token);
        if (in) {
            memcpy((uint8_t *)data + done, ep->bounce, moved);
        }
        done += moved;
        if (moved < piece) {
            break; /* Short: that's all there is. */
        }
    }
    mutex_unlock(&ep->lock);
    return result < 0 ? result : (int)done;
}

static void queue_report(struct endpoint *ep) {
    fill_qtd(&ep->qtds[0], ep->bounce, ep->size, TOKEN_IN | TOKEN_IOC);
    start(ep, ep->qtds);
}

static int ehci_interrupt_in(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint,
                             uint16_t size, usb_report_fn callback, void *arg) {
    struct ehci *hc = usb->data;
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

static int ehci_clear_toggle(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    (void)usb;
    struct endpoint *ep = find_endpoint(device, endpoint);
    if (!ep) {
        return -VX_EINVAL;
    }
    mutex_lock(&ep->lock);
    ep->qh->token &= ~TOKEN_TOGGLE;
    mutex_unlock(&ep->lock);
    return 0;
}

/* ---- Devices ---- */

static int ehci_address_device(struct usb_hc *usb, struct usb_device *device) {
    struct ehci *hc = usb->data;
    struct slot *slot = kzalloc(sizeof(*slot));
    if (!slot) {
        return -VX_ENOMEM;
    }
    mutex_lock(&hc->lock);
    int address = hcd_new_address(&hc->addresses);
    mutex_unlock(&hc->lock);
    struct endpoint *ep0 = address ? new_endpoint(device, 0, 0, USB_ENDPOINT_CONTROL,
                                                  device->speed == USB_SPEED_HIGH ? 64 : 8)
                                   : NULL;
    if (!ep0) {
        hcd_free_address(&hc->addresses, address);
        kfree(slot);
        return -VX_ENOMEM;
    }
    slot->endpoints[0] = ep0;
    device->hc_data = slot;
    link(hc, ep0);
    uint8_t setup[8] = {USB_DIR_OUT, USB_REQ_SET_ADDRESS, (uint8_t)address, 0, 0, 0, 0, 0};
    int result = ehci_control(usb, device, setup, NULL, 0);
    if (result < 0) {
        unlink(hc, ep0);
        free_endpoint(ep0);
        device->hc_data = NULL;
        kfree(slot);
        hcd_free_address(&hc->addresses, address);
        return result;
    }
    thread_sleep_ms(2); /* (The device takes its address.) */
    slot->address = address;
    ep0->qh->info1 = info1_of(device, address, ep0);
    return 0;
}

static int ehci_set_max_packet0(struct usb_hc *usb, struct usb_device *device, uint16_t size) {
    (void)usb;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return -VX_ENODEV;
    }
    struct endpoint *ep0 = slot->endpoints[0];
    ep0->max_packet = size;
    ep0->qh->info1 = info1_of(device, slot->address, ep0);
    return 0;
}

static int ehci_configure(struct usb_hc *usb, struct usb_device *device) {
    struct ehci *hc = usb->data;
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
            /* (Bits 11-12 of a high speed interrupt endpoint's size: more
             * per microframe; one is plenty here.) */
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

static void ehci_free_device(struct usb_hc *usb, struct usb_device *device) {
    struct ehci *hc = usb->data;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return;
    }
    for (int i = 0; i < 32; i++) {
        struct endpoint *ep = slot->endpoints[i];
        if (!ep) {
            continue;
        }
        mutex_lock(&ep->lock); /* (A transfer still going finishes first.) */
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

static int ehci_reset_endpoint(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    return ehci_clear_toggle(usb, device, endpoint);
}

/* ---- Root ports ---- */

static bool ehci_port_connected(struct usb_hc *usb, int port) {
    struct ehci *hc = usb->data;
    uint32_t portsc = read32(hc->op, OP_PORTSC(port));
    return (portsc & PORT_CONNECTED) && !(portsc & PORT_OWNER);
}

/* A low or full speed device: the companion controller's. */
static void hand_over(struct ehci *hc, int port) {
    if (!hc->companions) {
        kprintf("[ehci] port %d: a USB 1 device, but no USB 1 controller to take it\n", port);
        return;
    }
    uint32_t portsc = read32(hc->op, OP_PORTSC(port));
    write32(hc->op, OP_PORTSC(port), (portsc & ~PORT_CHANGES) | PORT_OWNER);
    kprintf("[ehci] port %d: a USB 1 device, handed to the USB 1 controller\n", port);
}

static int ehci_reset_port(struct usb_hc *usb, int port) {
    struct ehci *hc = usb->data;
    uint32_t portsc = read32(hc->op, OP_PORTSC(port));
    if (!(portsc & PORT_CONNECTED) || (portsc & PORT_OWNER)) {
        return -VX_ENODEV;
    }
    if (PORT_LINE(portsc) == 1) {
        hand_over(hc, port);
        return -VX_ENODEV;
    }
    write32(hc->op, OP_PORTSC(port), (portsc & ~(PORT_CHANGES | PORT_ENABLED)) | PORT_RESET);
    thread_sleep_ms(50);
    portsc = read32(hc->op, OP_PORTSC(port));
    write32(hc->op, OP_PORTSC(port), portsc & ~(PORT_CHANGES | PORT_ENABLED | PORT_RESET));
    wait_register(hc->op, OP_PORTSC(port), PORT_RESET, 0, 100);
    thread_sleep_ms(2);
    portsc = read32(hc->op, OP_PORTSC(port));
    if (!(portsc & PORT_CONNECTED)) {
        return -VX_ENODEV;
    }
    if (!(portsc & PORT_ENABLED)) {
        hand_over(hc, port); /* Not high speed after all: full speed. */
        return -VX_ENODEV;
    }
    write32(hc->op, OP_PORTSC(port), (portsc & ~PORT_CHANGES) | PORT_ENABLE_CHANGE);
    thread_sleep_ms(10); /* Reset recovery. */
    return USB_SPEED_HIGH;
}

static const struct usb_hc_ops ehci_ops = {
    .address_device = ehci_address_device,
    .set_max_packet0 = ehci_set_max_packet0,
    .configure = ehci_configure,
    .free_device = ehci_free_device,
    .control = ehci_control,
    .bulk = ehci_bulk,
    .interrupt_in = ehci_interrupt_in,
    .reset_endpoint = ehci_reset_endpoint,
    .clear_toggle = ehci_clear_toggle,
    .reset_port = ehci_reset_port,
    .port_connected = ehci_port_connected,
};

/* ---- The controller's thread ---- */

static bool ehci_interrupt(void *arg) {
    struct ehci *hc = arg;
    uint32_t status = read32(hc->op, OP_USBSTS);
    if (!(status & STS_INTERRUPTS) || status == 0xffffffffU) {
        return false; /* Another device's, on a shared line. */
    }
    if (status & STS_SYSTEM_ERROR) {
        hc->system_error = true; /* (The controller has stopped; the thread says so.) */
    }
    write32(hc->op, OP_USBSTS, status & STS_INTERRUPTS);
    hcd_irq_wake(&hc->irq);
    return true;
}

static void poll_thread(void *arg) {
    struct ehci *hc = arg;
    bool complained = false;
    for (;;) {
        hcd_idle(&hc->irq, hc->periodic ? 2 : 20, 250);
        uint32_t status = read32(hc->op, OP_USBSTS);
        write32(hc->op, OP_USBSTS, status & (STS_PORT_CHANGE | STS_SYSTEM_ERROR | 0x3));
        if (hc->system_error) {
            status |= STS_SYSTEM_ERROR;
        }
        if ((status & (STS_SYSTEM_ERROR | STS_HALTED)) && !complained) {
            kprintf("[ehci] the controller stopped (status %x)\n", status);
            complained = true;
        }
        for (int port = 1; port <= hc->ports; port++) {
            uint32_t portsc = read32(hc->op, OP_PORTSC(port));
            if (portsc & PORT_CONNECT_CHANGE) {
                write32(hc->op, OP_PORTSC(port), (portsc & ~PORT_CHANGES) | PORT_CONNECT_CHANGE);
                usb_port_changed(&hc->usb, port);
            }
        }
        mutex_lock(&hc->lock);
        for (struct endpoint *ep = hc->periodic; ep; ep = ep->next) {
            uint32_t token = __atomic_load_n(&ep->qtds[0].token, __ATOMIC_ACQUIRE);
            if (!ep->callback || (token & TOKEN_ACTIVE)) {
                continue;
            }
            if (token & TOKEN_HALTED) {
                ep->callback = NULL; /* Stalled, or the device is gone: stop polling. */
                reset_overlay(ep);
                continue;
            }
            ep->callback(ep->arg, ep->bounce, (int)(ep->size - TOKEN_REMAINING(token)));
            if (ep->callback) {
                queue_report(ep);
            }
        }
        mutex_unlock(&hc->lock);
    }
}

/* ---- Setting up a controller ---- */

/* Taking it from the firmware (which may be using it for a USB keyboard). */
static void take_over(struct ehci *hc, uint32_t hccparams) {
    uint8_t offset = (uint8_t)(hccparams >> 8);
    for (int guard = 0; offset >= 0x40 && guard < 16; guard++) {
        uint32_t capability = pci_read32(hc->pci, offset);
        if ((capability & 0xff) == 1) { /* USB legacy support. */
            if (capability & (1U << 16)) {
                pci_write32(hc->pci, offset, capability | (1U << 24));
                uint64_t deadline = timer_ms() + 1000;
                while ((pci_read32(hc->pci, offset) & (1U << 16)) && timer_ms() < deadline) {
                    thread_sleep_ms(1);
                }
                if (pci_read32(hc->pci, offset) & (1U << 16)) {
                    kprintf("[ehci] the firmware kept the controller; taking it anyway\n");
                    pci_write32(hc->pci, offset,
                                (pci_read32(hc->pci, offset) & ~(1U << 16)) | (1U << 24));
                }
            }
            pci_write32(hc->pci, offset + 4, 0); /* No more SMIs. */
        }
        offset = (uint8_t)(capability >> 8);
    }
}

static struct ehci *controllers[MAX_CONTROLLERS];
static int controller_count;

static void probe(struct pci_device *pci) {
    if (controller_count == MAX_CONTROLLERS) {
        return;
    }
    pci_enable(pci);
    volatile uint8_t *cap = pci_map_bar(pci, 0);
    struct ehci *hc = kzalloc(sizeof(*hc));
    if (!hc || !cap) {
        kfree(hc);
        return;
    }
    hc->pci = pci;
    hc->op = cap + (read32(cap, CAP_LENGTH) & 0xff);
    hc->lock = (struct mutex)MUTEX_INIT;
    uint32_t params = read32(cap, CAP_HCSPARAMS), cparams = read32(cap, CAP_HCCPARAMS);
    hc->ports = (int)(params & 0xf);
    hc->companions = (int)((params >> 12) & 0xf);

    take_over(hc, cparams);
    write32(hc->op, OP_USBCMD, read32(hc->op, OP_USBCMD) & ~CMD_RUN);
    wait_register(hc->op, OP_USBSTS, STS_HALTED, STS_HALTED, 100);
    write32(hc->op, OP_USBCMD, CMD_RESET);
    if (!wait_register(hc->op, OP_USBCMD, CMD_RESET, 0, 500)) {
        kprintf("[ehci] the controller didn't reset\n");
        return;
    }

    hc->frames = hcd_pages(0);
    uint8_t *page = hcd_pages(0);
    if (!hc->frames || !page) {
        kprintf("[ehci] out of memory\n");
        return;
    }
    hc->head = (struct qh *)page;
    hc->anchor = (struct qh *)(page + sizeof(struct qh));
    /* The async ring: the head alone (it's halted; the controller goes past it). */
    hc->head->link = hcd_phys(hc->head) | LINK_QH;
    hc->head->info1 = INFO1_HEAD;
    hc->head->info2 = INFO2_MULT;
    hc->head->next = LINK_TERMINATE;
    hc->head->alt_next = LINK_TERMINATE;
    hc->head->token = TOKEN_HALTED;
    /* Every frame: the anchor (it does nothing), then the interrupt QHs. */
    hc->anchor->link = LINK_TERMINATE;
    hc->anchor->info2 = INFO2_MULT;
    hc->anchor->next = LINK_TERMINATE;
    hc->anchor->alt_next = LINK_TERMINATE;
    hc->anchor->token = TOKEN_HALTED;
    for (int i = 0; i < 1024; i++) {
        hc->frames[i] = hcd_phys(hc->anchor) | LINK_QH;
    }

    if (cparams & 1) {
        write32(hc->op, OP_SEGMENT, 0); /* (64-bit addressing: the upper half is 0.) */
    }
    write32(hc->op, OP_USBINTR, 0);
    write32(hc->op, OP_PERIODIC, hcd_phys(hc->frames));
    write32(hc->op, OP_ASYNC, hcd_phys(hc->head));
    write32(hc->op, OP_USBCMD, CMD_THRESHOLD(8) | CMD_PERIODIC | CMD_ASYNC | CMD_RUN);
    write32(hc->op, OP_CONFIGFLAG, 1); /* The ports are EHCI's (until handed over). */
    read32(hc->op, OP_CONFIGFLAG);
    if (!wait_register(hc->op, OP_USBSTS, STS_HALTED, 0, 100)) {
        kprintf("[ehci] the controller didn't start\n");
        return;
    }
    if (params & (1U << 4)) { /* The ports' power is the driver's to turn on. */
        for (int port = 1; port <= hc->ports; port++) {
            uint32_t portsc = read32(hc->op, OP_PORTSC(port));
            write32(hc->op, OP_PORTSC(port), (portsc & ~PORT_CHANGES) | PORT_POWER);
        }
    }
    thread_sleep_ms(20);
    hc->irq.queue = (struct wait_queue)WAIT_QUEUE_INIT;
    hc->irq.enabled = pci_attach_interrupt(pci, ehci_interrupt, hc);
    if (hc->irq.enabled) {
        write32(hc->op, OP_USBSTS, STS_INTERRUPTS);
        write32(hc->op, OP_USBINTR, STS_INTERRUPTS);
    }

    hc->usb.ops = &ehci_ops;
    hc->usb.data = hc;
    hc->usb.name = "ehci";
    hc->usb.ports = hc->ports;
    hc->usb.node = pci->node;
    controllers[controller_count++] = hc;
    pci_claim(pci, "ehci", NULL);
    const char *how = hc->irq.enabled ? "interrupts" : "polled";
    device_set_details(pci->node, "USB 2, %d ports, %d USB 1 companion controller%s, %s",
                       hc->ports, hc->companions, hc->companions == 1 ? "" : "s", how);
    kprintf("[ehci] %d ports, %d companion controller%s, %s\n", hc->ports, hc->companions,
            hc->companions == 1 ? "" : "s", how);
    thread_create("ehci", poll_thread, hc);
    usb_add_controller(&hc->usb);
}

void ehci_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x0c && pci->subclass == 0x03 && pci->prog_if == 0x20) {
            probe(pci);
        }
    }
}
