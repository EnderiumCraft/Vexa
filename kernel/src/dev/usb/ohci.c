/*
 * OHCI: the other USB 1 host controllers (AMD, SiS, NVIDIA and others, and
 * Apple's), alone or as an EHCI controller's companions; QEMU's pci-ohci.
 *
 * The controller keeps three lists of endpoint descriptors (EDs): control,
 * bulk, and the interrupt ones it visits every frame (through the table in
 * the HCCA, a page it shares with the driver). Each ED has a queue of
 * transfer descriptors (TDs) that ends in an empty one, its tail: queuing a
 * transfer fills the tail and adds a new empty one after it. The controller
 * keeps the data toggle. As with EHCI and UHCI there are no interrupts here:
 * waiting threads check their TDs, and the controller's thread the interrupt
 * endpoints and the ports.
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

/* Registers. */
#define REG_CONTROL 0x04
#define REG_COMMAND_STATUS 0x08
#define REG_INTERRUPT_STATUS 0x0c
#define REG_INTERRUPT_DISABLE 0x14
#define REG_HCCA 0x18
#define REG_CONTROL_HEAD 0x20
#define REG_CONTROL_CURRENT 0x24
#define REG_BULK_HEAD 0x28
#define REG_BULK_CURRENT 0x2c
#define REG_FM_INTERVAL 0x34
#define REG_PERIODIC_START 0x40
#define REG_LS_THRESHOLD 0x44
#define REG_RH_DESCRIPTOR_A 0x48
#define REG_RH_STATUS 0x50
#define REG_RH_PORT(port) (0x54 + 4 * ((port) - 1))

#define CONTROL_RATIO 3U         /* Control and bulk: 4 to 1. */
#define CONTROL_PERIODIC (1U << 2)
#define CONTROL_CONTROL_LIST (1U << 4)
#define CONTROL_BULK_LIST (1U << 5)
#define CONTROL_OPERATIONAL (2U << 6)
#define CONTROL_STATE (3U << 6)
#define CONTROL_SMM (1U << 8)    /* The firmware (SMM) has it. */
#define COMMAND_RESET (1U << 0)
#define COMMAND_CONTROL_FILLED (1U << 1)
#define COMMAND_BULK_FILLED (1U << 2)
#define COMMAND_OWNERSHIP (1U << 3)

/* Root hub ports: reading. */
#define PORT_CONNECTED (1U << 0)
#define PORT_ENABLED (1U << 1)
#define PORT_LOW_SPEED (1U << 9)
#define PORT_CONNECT_CHANGE (1U << 16)
#define PORT_RESET_CHANGE (1U << 20)
/* Writing. */
#define PORT_SET_RESET (1U << 4)
#define PORT_SET_POWER (1U << 8)
#define PORT_CHANGES 0x001f0000U

/* ED words. */
#define ED_LOW_SPEED (1U << 13)
#define ED_SKIP (1U << 14)
#define ED_HALTED 1U         /* In the head pointer. */
#define ED_TOGGLE_CARRY 2U

/* TD info. */
#define TD_ROUNDING (1U << 18) /* A short packet is fine. */
#define TD_SETUP (0U << 19)
#define TD_OUT (1U << 19)
#define TD_IN (2U << 19)
#define TD_NO_INTERRUPT (7U << 21)
#define TD_DATA0 (2U << 24)
#define TD_DATA1 (3U << 24)
#define TD_CODE(info) ((info) >> 28)
#define TD_NOT_ACCESSED (15U << 28)
#define CODE_STALL 4

struct ed {
    uint32_t info, tail, head, next;
};

struct td {
    uint32_t info, buffer, next, end;
};

#define MAX_CONTROLLERS 8
#define TD_CHUNK 8192 /* A TD reaches two pages. */

struct ohci;

struct endpoint {
    uint8_t address; /* With USB_DIR_IN. */
    uint8_t type;
    uint16_t max_packet;
    struct ed *ed;    /* The page: the ED, then TDs. */
    struct td *tds;
    int td_count;
    int tail;         /* The empty TD at the end of the queue. */
    int pending;      /* Interrupt endpoints: the TD queued. */
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

struct ohci {
    struct usb_hc usb;
    struct pci_device *pci;
    volatile uint8_t *regs;
    int ports;
    uint32_t *hcca;
    struct ed *anchors; /* Control, bulk, interrupt: empty, skipped EDs the lists start at. */
    struct endpoint *lists[3];
    struct mutex lock;
    struct hcd_addresses addresses;
};

enum { LIST_CONTROL, LIST_BULK, LIST_INTERRUPT };

static uint32_t read32(struct ohci *hc, uint32_t offset) {
    return *(volatile uint32_t *)(hc->regs + offset);
}

static void write32(struct ohci *hc, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(hc->regs + offset) = value;
}

static int endpoint_index(uint8_t address) {
    return (address & 0xf) | ((address & USB_DIR_IN) && (address & 0xf) ? 16 : 0);
}

static int list_of(const struct endpoint *ep) {
    return ep->type == USB_ENDPOINT_CONTROL ? LIST_CONTROL
           : ep->type == USB_ENDPOINT_BULK  ? LIST_BULK
                                            : LIST_INTERRUPT;
}

/* ---- Endpoints and the lists ---- */

static uint32_t ed_info(struct usb_device *device, int address, const struct endpoint *ep) {
    return (uint32_t)address | (uint32_t)(ep->address & 0xf) << 7 |
           (device->speed == USB_SPEED_LOW ? ED_LOW_SPEED : 0) | (uint32_t)ep->max_packet << 16;
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
    ep->bounce_order = type == USB_ENDPOINT_BULK ? 1 : type == USB_ENDPOINT_CONTROL ? 1 : 0;
    ep->ed = hcd_pages(0);
    ep->bounce = hcd_pages(ep->bounce_order);
    if (!ep->ed || !ep->bounce) {
        hcd_free_pages(ep->ed, 0);
        hcd_free_pages(ep->bounce, ep->bounce_order);
        kfree(ep);
        return NULL;
    }
    ep->tds = (struct td *)((uint8_t *)ep->ed + 32);
    ep->td_count = (int)((PAGE_SIZE - 32) / sizeof(struct td));
    ep->ed->info = ed_info(device, address, ep);
    ep->ed->tail = hcd_phys(&ep->tds[0]);
    ep->ed->head = hcd_phys(&ep->tds[0]);
    return ep;
}

static void free_endpoint(struct endpoint *ep) {
    hcd_free_pages(ep->ed, 0);
    hcd_free_pages(ep->bounce, ep->bounce_order);
    kfree(ep);
}

static void link(struct ohci *hc, struct endpoint *ep) {
    mutex_lock(&hc->lock);
    int list = list_of(ep);
    struct ed *anchor = &hc->anchors[list];
    ep->ed->next = anchor->next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    anchor->next = hcd_phys(ep->ed);
    ep->next = hc->lists[list];
    hc->lists[list] = ep;
    mutex_unlock(&hc->lock);
}

static void unlink(struct ohci *hc, struct endpoint *ep) {
    mutex_lock(&hc->lock);
    int list = list_of(ep);
    ep->ed->info |= ED_SKIP;
    for (struct endpoint **p = &hc->lists[list]; *p; p = &(*p)->next) {
        if (*p == ep) {
            *p = ep->next;
            break;
        }
    }
    struct ed *before = &hc->anchors[list];
    for (struct endpoint *e = hc->lists[list]; e; e = e->next) {
        if (e->ed->next == hcd_phys(ep->ed)) {
            before = e->ed;
        }
    }
    uint32_t enable = list == LIST_CONTROL ? CONTROL_CONTROL_LIST
                      : list == LIST_BULK  ? CONTROL_BULK_LIST
                                           : 0;
    /* The controller may be on it: stop that list a moment (the periodic
     * one starts again from the HCCA each frame). */
    if (enable) {
        write32(hc, REG_CONTROL, read32(hc, REG_CONTROL) & ~enable);
    }
    thread_sleep_ms(2);
    before->next = ep->ed->next;
    if (enable) {
        write32(hc, list == LIST_CONTROL ? REG_CONTROL_CURRENT : REG_BULK_CURRENT, 0);
        write32(hc, REG_CONTROL, read32(hc, REG_CONTROL) | enable);
    }
    thread_sleep_ms(1);
    mutex_unlock(&hc->lock);
}

/* ---- Transfers ---- */

static struct td *next_td(struct endpoint *ep, int *index) {
    *index = (*index + 1) % ep->td_count;
    return &ep->tds[*index];
}

/* Queues TDs: fills the tail and the ones after it, and adds an empty tail. */
struct queue {
    struct endpoint *ep;
    int first, count, cursor;
};

static void queue_start(struct queue *q, struct endpoint *ep) {
    q->ep = ep;
    q->first = ep->tail;
    q->count = 0;
    q->cursor = ep->tail;
}

static void queue_td(struct queue *q, uint32_t info, const uint8_t *buffer, uint32_t length) {
    struct td *td = &q->ep->tds[q->cursor];
    int index = q->cursor;
    struct td *following = next_td(q->ep, &index);
    td->info = info | TD_NOT_ACCESSED | TD_NO_INTERRUPT;
    td->buffer = length ? hcd_phys(buffer) : 0;
    td->end = length ? hcd_phys(buffer + length - 1) : 0;
    td->next = hcd_phys(following);
    q->cursor = index;
    q->count++;
}

static void queue_go(struct ohci *hc, struct queue *q) {
    struct td *tail = &q->ep->tds[q->cursor];
    memset(tail, 0, sizeof(*tail));
    q->ep->tail = q->cursor;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->ep->ed->tail = hcd_phys(tail);
    int list = list_of(q->ep);
    if (list != LIST_INTERRUPT) {
        write32(hc, REG_COMMAND_STATUS,
                list == LIST_CONTROL ? COMMAND_CONTROL_FILLED : COMMAND_BULK_FILLED);
    }
}

/* Done: the controller has gone through to the tail (a TD it retries after
 * an error has its code written, but stays), or stopped at an error. */
static bool queue_done(void *arg) {
    struct queue *q = arg;
    uint32_t head = __atomic_load_n(&q->ep->ed->head, __ATOMIC_ACQUIRE);
    return (head & ED_HALTED) || (head & ~0xfU) == hcd_phys(&q->ep->tds[q->ep->tail]);
}

/* The queue starts from the tail again (dropping what's left on it). */
static void reset_queue(struct endpoint *ep, bool skip_first) {
    if (skip_first) {
        ep->ed->info |= ED_SKIP;
        thread_sleep_ms(2);
    }
    ep->ed->head = hcd_phys(&ep->tds[ep->tail]) | (ep->ed->head & ED_TOGGLE_CARRY);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ep->ed->info &= ~ED_SKIP;
}

/* Waits for the queued TDs; returns 0 or an error. */
static int finish(struct endpoint *ep, struct queue *q, uint32_t timeout_ms) {
    if (!hcd_wait(queue_done, q, timeout_ms)) {
        reset_queue(ep, true);
        kprintf("[ohci] endpoint %x: a transfer timed out\n", ep->address);
        return -VX_ETIMEDOUT;
    }
    if (ep->ed->head & ED_HALTED) {
        int code = 0;
        for (int i = 0; i < q->count; i++) {
            uint32_t c = TD_CODE(ep->tds[(q->first + i) % ep->td_count].info);
            if (c != 0 && c != 15) {
                code = (int)c;
                break;
            }
        }
        reset_queue(ep, false);
        if (code != CODE_STALL) {
            kprintf("[ohci] endpoint %x: a transfer failed (condition code %d)\n", ep->address,
                    code);
        }
        return code == CODE_STALL ? -VX_EPIPE : -VX_EIO;
    }
    return 0;
}

/* What a finished TD moved, of `length` bytes. */
static uint32_t td_moved(const struct td *td, uint32_t length) {
    if (!td->buffer || !length) {
        return length; /* (All of it.) */
    }
    uint32_t remaining;
    if ((td->buffer & ~0xfffU) == (td->end & ~0xfffU)) {
        remaining = td->end - td->buffer + 1;
    } else {
        remaining = (0x1000 - (td->buffer & 0xfff)) + (td->end & 0xfff) + 1;
    }
    return remaining < length ? length - remaining : 0;
}

static int ohci_control(struct usb_hc *usb, struct usb_device *device, const uint8_t setup[8],
                        void *data, uint16_t length) {
    struct ohci *hc = usb->data;
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
    struct queue q;
    queue_start(&q, ep);
    queue_td(&q, TD_SETUP | TD_DATA0, packet, 8);
    int data_td = -1;
    if (length) {
        data_td = q.cursor;
        queue_td(&q, (in ? TD_IN : TD_OUT) | TD_DATA1 | TD_ROUNDING, buffer, length);
    }
    queue_td(&q, (in && length ? TD_OUT : TD_IN) | TD_DATA1, NULL, 0);
    queue_go(hc, &q);
    int result = finish(ep, &q, 3000);
    if (result == 0) {
        result = data_td >= 0 ? (int)td_moved(&ep->tds[data_td], length) : 0;
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

static int ohci_bulk(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint, void *data,
                     uint32_t length, uint32_t timeout_ms) {
    struct ohci *hc = usb->data;
    struct endpoint *ep = find_endpoint(device, endpoint);
    if (!ep || ep->type != USB_ENDPOINT_BULK) {
        return -VX_ENODEV;
    }
    bool in = endpoint & USB_DIR_IN;
    mutex_lock(&ep->lock);
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t piece = length - done < TD_CHUNK ? length - done : TD_CHUNK;
        if (!in) {
            memcpy(ep->bounce, (uint8_t *)data + done, piece);
        }
        struct queue q;
        queue_start(&q, ep);
        int td = q.cursor;
        queue_td(&q, (in ? TD_IN : TD_OUT) | TD_ROUNDING, ep->bounce, piece);
        queue_go(hc, &q);
        result = finish(ep, &q, timeout_ms);
        if (result < 0) {
            break;
        }
        uint32_t got = td_moved(&ep->tds[td], piece);
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

static void queue_report(struct ohci *hc, struct endpoint *ep) {
    struct queue q;
    queue_start(&q, ep);
    ep->pending = q.cursor;
    queue_td(&q, TD_IN | TD_ROUNDING, ep->bounce, ep->size);
    queue_go(hc, &q);
}

static int ohci_interrupt_in(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint,
                             uint16_t size, usb_report_fn callback, void *arg) {
    struct ohci *hc = usb->data;
    struct endpoint *ep = find_endpoint(device, endpoint | USB_DIR_IN);
    if (!ep || ep->type != USB_ENDPOINT_INTERRUPT || size == 0 || size > PAGE_SIZE) {
        return -VX_EINVAL;
    }
    mutex_lock(&hc->lock);
    ep->size = size;
    ep->arg = arg;
    ep->callback = callback;
    queue_report(hc, ep);
    mutex_unlock(&hc->lock);
    return 0;
}

static int ohci_clear_toggle(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    (void)usb;
    struct endpoint *ep = find_endpoint(device, endpoint);
    if (!ep) {
        return -VX_EINVAL;
    }
    mutex_lock(&ep->lock);
    ep->ed->info |= ED_SKIP;
    thread_sleep_ms(2);
    ep->ed->head &= ~ED_TOGGLE_CARRY;
    ep->ed->info &= ~ED_SKIP;
    mutex_unlock(&ep->lock);
    return 0;
}

/* ---- Devices ---- */

static int ohci_address_device(struct usb_hc *usb, struct usb_device *device) {
    struct ohci *hc = usb->data;
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
    int result = ohci_control(usb, device, setup, NULL, 0);
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
    ep0->ed->info = ed_info(device, address, ep0);
    return 0;
}

static int ohci_set_max_packet0(struct usb_hc *usb, struct usb_device *device, uint16_t size) {
    (void)usb;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return -VX_ENODEV;
    }
    struct endpoint *ep0 = slot->endpoints[0];
    ep0->max_packet = size;
    ep0->ed->info = ed_info(device, slot->address, ep0);
    return 0;
}

static int ohci_configure(struct usb_hc *usb, struct usb_device *device) {
    struct ohci *hc = usb->data;
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

static void ohci_free_device(struct usb_hc *usb, struct usb_device *device) {
    struct ohci *hc = usb->data;
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

static int ohci_reset_endpoint(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    return ohci_clear_toggle(usb, device, endpoint);
}

/* ---- Root ports ---- */

static bool ohci_port_connected(struct usb_hc *usb, int port) {
    return read32(usb->data, REG_RH_PORT(port)) & PORT_CONNECTED;
}

static int ohci_reset_port(struct usb_hc *usb, int port) {
    struct ohci *hc = usb->data;
    if (!(read32(hc, REG_RH_PORT(port)) & PORT_CONNECTED)) {
        return -VX_ENODEV;
    }
    write32(hc, REG_RH_PORT(port), PORT_SET_RESET);
    uint64_t deadline = timer_ms() + 200;
    while (!(read32(hc, REG_RH_PORT(port)) & PORT_RESET_CHANGE) && timer_ms() < deadline) {
        thread_sleep_ms(2);
    }
    write32(hc, REG_RH_PORT(port), PORT_RESET_CHANGE);
    thread_sleep_ms(10); /* Reset recovery. */
    uint32_t status = read32(hc, REG_RH_PORT(port));
    if (!(status & PORT_ENABLED)) {
        return -VX_EIO;
    }
    return (status & PORT_LOW_SPEED) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static const struct usb_hc_ops ohci_ops = {
    .address_device = ohci_address_device,
    .set_max_packet0 = ohci_set_max_packet0,
    .configure = ohci_configure,
    .free_device = ohci_free_device,
    .control = ohci_control,
    .bulk = ohci_bulk,
    .interrupt_in = ohci_interrupt_in,
    .reset_endpoint = ohci_reset_endpoint,
    .clear_toggle = ohci_clear_toggle,
    .reset_port = ohci_reset_port,
    .port_connected = ohci_port_connected,
};

/* ---- The controller's thread ---- */

static void poll_thread(void *arg) {
    struct ohci *hc = arg;
    for (;;) {
        thread_sleep_ms(hc->lists[LIST_INTERRUPT] ? 2 : 20);
        write32(hc, REG_INTERRUPT_STATUS, read32(hc, REG_INTERRUPT_STATUS) & 0x7f);
        for (int port = 1; port <= hc->ports; port++) {
            uint32_t status = read32(hc, REG_RH_PORT(port));
            if (status & PORT_CONNECT_CHANGE) {
                write32(hc, REG_RH_PORT(port), status & PORT_CHANGES & ~PORT_RESET_CHANGE);
                usb_port_changed(&hc->usb, port);
            }
        }
        mutex_lock(&hc->lock);
        for (struct endpoint *ep = hc->lists[LIST_INTERRUPT]; ep; ep = ep->next) {
            if (!ep->callback) {
                continue;
            }
            struct td *td = &ep->tds[ep->pending];
            uint32_t code = TD_CODE(__atomic_load_n(&td->info, __ATOMIC_ACQUIRE));
            bool halted = ep->ed->head & ED_HALTED;
            if (code == 15 && !halted) {
                continue; /* (Still waiting for a report.) */
            }
            if (halted || code != 0) {
                ep->callback = NULL; /* Stalled, or the device is gone: stop polling. */
                reset_queue(ep, false);
                continue;
            }
            ep->callback(ep->arg, ep->bounce, (int)td_moved(td, ep->size));
            if (ep->callback) {
                queue_report(hc, ep);
            }
        }
        mutex_unlock(&hc->lock);
    }
}

/* ---- Setting up a controller ---- */

static int controller_count;

static void probe(struct pci_device *pci) {
    if (controller_count == MAX_CONTROLLERS) {
        return;
    }
    pci_enable(pci);
    struct ohci *hc = kzalloc(sizeof(*hc));
    volatile uint8_t *regs = pci_map_bar(pci, 0);
    if (!hc || !regs) {
        kfree(hc);
        return;
    }
    hc->pci = pci;
    hc->regs = regs;
    hc->lock = (struct mutex)MUTEX_INIT;

    /* From the firmware: ask SMM to let go, or just reset it (BIOS driver). */
    if (read32(hc, REG_CONTROL) & CONTROL_SMM) {
        write32(hc, REG_COMMAND_STATUS, COMMAND_OWNERSHIP);
        uint64_t deadline = timer_ms() + 1000;
        while ((read32(hc, REG_CONTROL) & CONTROL_SMM) && timer_ms() < deadline) {
            thread_sleep_ms(1);
        }
        if (read32(hc, REG_CONTROL) & CONTROL_SMM) {
            kprintf("[ohci] the firmware kept the controller; taking it anyway\n");
        }
    }
    uint32_t interval = read32(hc, REG_FM_INTERVAL);
    uint32_t fi = interval & 0x3fff;
    if (fi < 11000 || fi > 13000) {
        fi = 11999;
    }
    write32(hc, REG_INTERRUPT_DISABLE, 0xc000007fU);
    write32(hc, REG_CONTROL, 0); /* (The reset state.) */
    thread_sleep_ms(10);
    write32(hc, REG_COMMAND_STATUS, COMMAND_RESET);
    for (int tries = 0; tries < 100 && (read32(hc, REG_COMMAND_STATUS) & COMMAND_RESET); tries++) {
        thread_sleep_ms(1);
    }

    hc->hcca = hcd_pages(0);
    hc->anchors = hcd_pages(0);
    if (!hc->hcca || !hc->anchors) {
        kprintf("[ohci] out of memory\n");
        return;
    }
    for (int i = 0; i < 3; i++) {
        hc->anchors[i].info = ED_SKIP;
    }
    for (int i = 0; i < 32; i++) {
        hc->hcca[i] = hcd_phys(&hc->anchors[LIST_INTERRUPT]);
    }
    write32(hc, REG_HCCA, hcd_phys(hc->hcca));
    write32(hc, REG_CONTROL_HEAD, hcd_phys(&hc->anchors[LIST_CONTROL]));
    write32(hc, REG_BULK_HEAD, hcd_phys(&hc->anchors[LIST_BULK]));
    write32(hc, REG_CONTROL_CURRENT, 0);
    write32(hc, REG_BULK_CURRENT, 0);
    uint32_t largest = (6 * (fi - 210)) / 7;
    write32(hc, REG_FM_INTERVAL,
            fi | largest << 16 | ((read32(hc, REG_FM_INTERVAL) & 0x80000000U) ^ 0x80000000U));
    write32(hc, REG_PERIODIC_START, fi * 9 / 10);
    write32(hc, REG_LS_THRESHOLD, 0x628);
    write32(hc, REG_INTERRUPT_STATUS, 0xffffffffU);
    write32(hc, REG_CONTROL, CONTROL_OPERATIONAL | CONTROL_RATIO | CONTROL_PERIODIC |
                                 CONTROL_CONTROL_LIST | CONTROL_BULK_LIST);
    thread_sleep_ms(2);
    if ((read32(hc, REG_CONTROL) & CONTROL_STATE) != CONTROL_OPERATIONAL) {
        kprintf("[ohci] the controller didn't start\n");
        return;
    }

    /* The root hub: power on its ports. */
    uint32_t a = read32(hc, REG_RH_DESCRIPTOR_A);
    hc->ports = (int)(a & 0xff) < 15 ? (int)(a & 0xff) : 15;
    if (!(a & (1U << 9))) {
        write32(hc, REG_RH_STATUS, 1U << 16); /* (All of them.) */
        if (a & (1U << 8)) {
            for (int port = 1; port <= hc->ports; port++) {
                write32(hc, REG_RH_PORT(port), PORT_SET_POWER);
            }
        }
    }
    uint32_t power_good = (a >> 24) * 2;
    thread_sleep_ms(power_good > 20 ? power_good : 20);

    hc->usb.ops = &ohci_ops;
    hc->usb.data = hc;
    hc->usb.name = "ohci";
    hc->usb.ports = hc->ports;
    hc->usb.node = pci->node;
    controller_count++;
    pci_claim(pci, "ohci", NULL);
    device_set_details(pci->node, "USB 1, %d ports, polled", hc->ports);
    kprintf("[ohci] %d ports\n", hc->ports);
    thread_create("ohci", poll_thread, hc);
    usb_add_controller(&hc->usb);
}

void ohci_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x0c && pci->subclass == 0x03 && pci->prog_if == 0x10) {
            probe(pci);
        }
    }
}
