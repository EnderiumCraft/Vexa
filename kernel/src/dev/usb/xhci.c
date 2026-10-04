/*
 * xHCI: USB host controllers (USB 1 to 3, the only kind on PCs made since
 * about 2012, and QEMU's qemu-xhci).
 *
 * The controller works from memory the driver sets up: a table of device
 * contexts (one per slot, a slot being a device it knows), rings of 16-byte
 * TRBs (a command ring, a transfer ring per endpoint), and an event ring it
 * writes completions and port changes to. A kernel thread per controller
 * reads the event ring, woken by the controller's MSI interrupt (or every
 * 10 ms without one); threads that started a command or a transfer sleep
 * until their event arrives. Interrupt endpoints (keyboards, mice, hubs) stay
 * queued: each report goes to its callback and the TRB is queued again.
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

/* Capability registers. */
#define CAP_LENGTH 0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18

/* Operational registers. */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(port) (0x400 + 0x10 * ((port) - 1))

#define CMD_RUN (1U << 0)
#define CMD_RESET (1U << 1)
#define CMD_INTERRUPTS (1U << 2)
#define STS_HALTED (1U << 0)
#define STS_EVENT (1U << 3)
#define STS_NOT_READY (1U << 11)

#define PORT_CONNECTED (1U << 0)
#define PORT_ENABLED (1U << 1)
#define PORT_RESET (1U << 4)
#define PORT_POWER (1U << 9)
#define PORT_SPEED(portsc) (((portsc) >> 10) & 0xf)
#define PORT_WARM_RESET (1U << 31)
#define PORT_RESET_CHANGE (1U << 21)
#define PORT_WARM_RESET_CHANGE (1U << 19)
/* Writing 1 clears these (and PORT_ENABLED, which writing 1 disables). */
#define PORT_CHANGES 0x00fe0000U

/* Runtime registers: interrupter 0. */
#define RT_IMAN 0x20
#define RT_IMOD 0x24
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP 0x38
#define IMAN_PENDING (1U << 0)
#define IMAN_ENABLE (1U << 1)
#define ERDP_BUSY (1ULL << 3)

/* TRBs. */
struct trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

#define TRB_CYCLE (1U << 0)
#define TRB_TOGGLE (1U << 1)  /* Link TRBs. */
#define TRB_ISP (1U << 2)     /* Interrupt on a short packet. */
#define TRB_CHAIN (1U << 4)
#define TRB_IOC (1U << 5)     /* Interrupt (an event) on completion. */
#define TRB_IDT (1U << 6)     /* Immediate data (setup packets). */
#define TRB_DIR_IN (1U << 16)
#define TRB_TYPE(type) ((uint32_t)(type) << 10)
#define TRB_TYPE_OF(control) (((control) >> 10) & 0x3f)

enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_ISOCH = 5, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11,
    TRB_CONFIGURE_ENDPOINT = 12, TRB_EVALUATE_CONTEXT = 13, TRB_RESET_ENDPOINT = 14,
    TRB_STOP_ENDPOINT = 15, TRB_SET_DEQUEUE = 16,
    TRB_TRANSFER_EVENT = 32, TRB_COMMAND_COMPLETION = 33, TRB_PORT_STATUS_CHANGE = 34,
};

enum {
    CODE_SUCCESS = 1, CODE_DATA_BUFFER = 2, CODE_BABBLE = 3, CODE_TRANSACTION = 4,
    CODE_TRB = 5, CODE_STALL = 6, CODE_SHORT_PACKET = 13, CODE_RING_UNDERRUN = 14,
    CODE_RING_OVERRUN = 15, CODE_MISSED_SERVICE = 23,
};

#define TRB_SIA (1U << 31) /* Isoch TRBs: start as soon as possible. */
#define ISO_SLOTS 16       /* Isochronous packets kept queued. */

#define RING_TRBS 256 /* One page; the last is the link back to the start. */
#define MAX_SLOTS 64
#define MAX_CONTROLLERS 4
#define BULK_MAX 65536

struct ring {
    struct trb *trbs;
    uint64_t phys;
    uint32_t enqueue;
    uint32_t cycle;
};

/* One endpoint (a DCI: 1 is endpoint 0, then 2 * number + IN). */
struct endpoint {
    struct ring ring;
    bool configured;
    uint16_t max_packet;
    /* A transfer someone waits for: its TRBs (a control transfer's data
     * stage, then the one that ends it), and how it went. */
    uint64_t trbs[2];
    uint32_t lengths[2];
    int trb_count;
    volatile bool done;
    uint32_t code;
    uint32_t transferred;
    bool partial;      /* A control transfer's data stage came back short. */
    uint32_t partial_length;
    /* An isochronous OUT endpoint kept fed: packets in `buffer`, refilled
     * in turn as each is sent. */
    usb_iso_fill_fn iso_fill;
    uint16_t iso_packet;
    int iso_next, iso_slots;
    /* An interrupt IN endpoint kept polled. */
    usb_report_fn callback;
    void *arg;
    uint8_t *buffer;
    uint16_t size;
};

struct slot {
    int id;
    uint8_t *output;        /* The device context the controller keeps. */
    uint8_t *input;         /* An input context, for commands. */
    uint8_t *bounce;        /* A page for control transfers' data. */
    struct endpoint *endpoints[32];
    struct mutex control_lock;
    struct usb_device *device;
};

struct xhci {
    struct usb_hc usb;
    struct pci_device *pci;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *doorbells;
    int max_slots, ports, context_size;
    bool usb3_port[64];
    uint64_t *dcbaa;
    struct ring commands;
    struct trb *events;
    uint32_t event_dequeue, event_cycle;
    struct mutex command_lock;
    uint64_t command_trb;
    volatile bool command_done;
    uint32_t command_code, command_slot;
    struct slot *slots[MAX_SLOTS + 1];
    struct wait_queue waiters;     /* Commands and transfers waiting for their events. */
    struct wait_queue interrupts;  /* The event thread, waiting for the interrupt. */
    volatile bool interrupted;
};

static struct xhci *controllers[MAX_CONTROLLERS];
static int controller_count;

/* ---- Registers and memory ---- */

static uint32_t read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static void write64(volatile uint8_t *base, uint32_t offset, uint64_t value) {
    write32(base, offset, (uint32_t)value);
    write32(base, offset + 4, (uint32_t)(value >> 32));
}

/* A zeroed page for the controller (NULL if out of memory). */
static void *dma_page(void) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return NULL;
    }
    void *page = phys_to_virt(phys);
    memset(page, 0, PAGE_SIZE);
    return page;
}

static void free_page(void *page) {
    if (page) {
        pmm_free(virt_to_phys(page), 0);
    }
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

/* ---- Rings ---- */

static bool ring_init(struct ring *ring) {
    ring->trbs = dma_page();
    if (!ring->trbs) {
        return false;
    }
    ring->phys = virt_to_phys(ring->trbs);
    ring->enqueue = 0;
    ring->cycle = 1;
    struct trb *link = &ring->trbs[RING_TRBS - 1];
    link->parameter = ring->phys;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
    return true;
}

/* Queues a TRB (the cycle bit last, so the controller sees it whole);
 * returns its physical address. */
static uint64_t ring_push(struct ring *ring, uint64_t parameter, uint32_t status,
                          uint32_t control) {
    struct trb *trb = &ring->trbs[ring->enqueue];
    uint64_t phys = ring->phys + ring->enqueue * sizeof(struct trb);
    trb->parameter = parameter;
    trb->status = status;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    trb->control = (control & ~TRB_CYCLE) | ring->cycle;
    if (++ring->enqueue == RING_TRBS - 1) {
        struct trb *link = &ring->trbs[RING_TRBS - 1];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        link->control = (link->control & ~TRB_CYCLE) | ring->cycle |
                        (control & TRB_CHAIN); /* (A chain goes on through the link.) */
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    return phys;
}

static uint64_t ring_dequeue_pointer(struct ring *ring) {
    return (ring->phys + ring->enqueue * sizeof(struct trb)) | ring->cycle;
}

/* ---- Contexts ---- */

static uint32_t *context(struct xhci *hc, uint8_t *base, int index) {
    return (uint32_t *)(base + index * hc->context_size);
}

/* In an input context: 0 is the input control context, 1 the slot's, then DCIs. */
static uint32_t *input_context(struct xhci *hc, struct slot *slot, int index) {
    return context(hc, slot->input, index);
}

static int speed_id(enum usb_speed speed) {
    switch (speed) {
    case USB_SPEED_LOW: return 2;
    case USB_SPEED_FULL: return 1;
    case USB_SPEED_HIGH: return 3;
    default: return 4;
    }
}

static enum usb_speed speed_of(int id) {
    switch (id) {
    case 1: return USB_SPEED_FULL;
    case 2: return USB_SPEED_LOW;
    case 3: return USB_SPEED_HIGH;
    default: return USB_SPEED_SUPER;
    }
}

static int dci_of(uint8_t endpoint) {
    int number = endpoint & 0x0f;
    return number == 0 ? 1 : number * 2 + ((endpoint & USB_DIR_IN) ? 1 : 0);
}

/* The slot context, from what the core knows of the device. */
static void fill_slot_context(struct xhci *hc, struct slot *slot, struct usb_device *device,
                              int entries) {
    uint32_t *ctx = input_context(hc, slot, 1);
    memset(ctx, 0, hc->context_size);
    ctx[0] = (device->route & 0xfffff) | (uint32_t)speed_id(device->speed) << 20 |
             (uint32_t)entries << 27;
    ctx[1] = (uint32_t)device->root_port << 16;
    if (device->is_hub) {
        ctx[0] |= 1U << 26;
        if (device->hub_multi_tt && device->speed == USB_SPEED_HIGH) {
            ctx[0] |= 1U << 25;
        }
        ctx[1] |= (uint32_t)device->hub_ports << 24;
    }
    if (device->tt_hub && device->tt_hub->hc_data &&
        (device->speed == USB_SPEED_LOW || device->speed == USB_SPEED_FULL)) {
        struct slot *tt = device->tt_hub->hc_data;
        ctx[2] = (uint32_t)tt->id | (uint32_t)device->tt_port << 8;
        if (device->tt_hub->hub_multi_tt) {
            ctx[0] |= 1U << 25;
        }
    }
}

/* ---- Events ---- */

/* Fills the next isochronous packet and queues it. */
static void iso_queue(struct endpoint *ep) {
    uint8_t *packet = ep->buffer + ep->iso_next * ep->iso_packet;
    int length = ep->iso_fill(ep->arg, packet, ep->iso_packet);
    length = length < 0 ? 0 : length > ep->iso_packet ? ep->iso_packet : length;
    ring_push(&ep->ring, virt_to_phys(packet), (uint32_t)length,
              TRB_TYPE(TRB_ISOCH) | TRB_IOC | TRB_SIA);
    ep->iso_next = (ep->iso_next + 1) % ep->iso_slots;
}

static void endpoint_event(struct xhci *hc, struct endpoint *ep, struct trb *event) {
    uint32_t code = event->status >> 24, residual = event->status & 0xffffff;
    if (ep->iso_fill) { /* A packet went (or its moment did): the next one. */
        if (code != CODE_RING_UNDERRUN && code != CODE_RING_OVERRUN) {
            iso_queue(ep);
        }
        return;
    }
    if (ep->callback) { /* An interrupt endpoint: a report, then queue it again. */
        if (code == CODE_SUCCESS || code == CODE_SHORT_PACKET) {
            int length = ep->size > residual ? (int)(ep->size - residual) : 0;
            ep->callback(ep->arg, ep->buffer, length);
        }
        if (code == CODE_SUCCESS || code == CODE_SHORT_PACKET || code == CODE_TRANSACTION ||
            code == CODE_BABBLE) {
            ep->trbs[0] = ring_push(&ep->ring, virt_to_phys(ep->buffer), ep->size,
                                    TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
            ep->trb_count = 1;
            return; /* (The doorbell rings below, in the caller.) */
        }
        ep->callback = NULL; /* Stalled, or the device is gone: stop polling. */
        return;
    }
    if (ep->done || ep->trb_count == 0) {
        return; /* Nobody waits for this one (it finished, or timed out). */
    }
    int which = -1;
    for (int i = 0; i < ep->trb_count; i++) {
        if (ep->trbs[i] == event->parameter) {
            which = i;
        }
    }
    bool error = code != CODE_SUCCESS && code != CODE_SHORT_PACKET;
    if (which < 0 && !error) {
        return;
    }
    if (which >= 0 && which < ep->trb_count - 1 && !error) {
        /* A control transfer's short data stage: its status stage still comes. */
        ep->partial = true;
        ep->partial_length = ep->lengths[which] > residual ? ep->lengths[which] - residual : 0;
        return;
    }
    if (ep->partial) {
        ep->transferred = ep->partial_length;
    } else if (which >= 0) {
        ep->transferred = ep->lengths[0] > residual ? ep->lengths[0] - residual : 0;
        if (ep->trb_count == 2) { /* (The status stage's event: all of the data came.) */
            ep->transferred = ep->lengths[0];
        }
    }
    ep->code = code;
    __atomic_store_n(&ep->done, true, __ATOMIC_RELEASE);
    wait_queue_wake_all(&hc->waiters);
}

static void process_events(struct xhci *hc) {
    bool any = false;
    for (;;) {
        struct trb *event = &hc->events[hc->event_dequeue];
        uint32_t control = __atomic_load_n(&event->control, __ATOMIC_ACQUIRE);
        if ((control & TRB_CYCLE) != hc->event_cycle) {
            break;
        }
        any = true;
        switch (TRB_TYPE_OF(control)) {
        case TRB_COMMAND_COMPLETION:
            if (event->parameter == hc->command_trb) {
                hc->command_code = event->status >> 24;
                hc->command_slot = control >> 24;
                __atomic_store_n(&hc->command_done, true, __ATOMIC_RELEASE);
                wait_queue_wake_all(&hc->waiters);
            }
            break;
        case TRB_TRANSFER_EVENT: {
            uint32_t slot_id = control >> 24, dci = (control >> 16) & 0x1f;
            struct slot *slot = slot_id <= MAX_SLOTS ? hc->slots[slot_id] : NULL;
            struct endpoint *ep = slot ? slot->endpoints[dci] : NULL;
            if (ep) {
                bool polled = ep->callback != NULL || ep->iso_fill != NULL;
                endpoint_event(hc, ep, event);
                if (polled && (ep->callback || ep->iso_fill)) {
                    hc->doorbells[slot_id] = dci;
                }
            }
            break;
        }
        case TRB_PORT_STATUS_CHANGE: {
            int port = (int)(event->parameter >> 24);
            if (port >= 1 && port <= hc->ports) {
                /* Clear what changed (not the reset's change: reset_port waits for it). */
                uint32_t portsc = read32(hc->op, OP_PORTSC(port));
                write32(hc->op, OP_PORTSC(port),
                        (portsc & ~(PORT_ENABLED | PORT_CHANGES)) |
                            (portsc & PORT_CHANGES & ~(PORT_RESET_CHANGE | PORT_WARM_RESET_CHANGE)));
                usb_port_changed(&hc->usb, port);
            }
            break;
        }
        default:
            break;
        }
        if (++hc->event_dequeue == RING_TRBS) {
            hc->event_dequeue = 0;
            hc->event_cycle ^= 1;
        }
    }
    if (any) {
        write64(hc->rt, RT_ERDP,
                (virt_to_phys(hc->events) + hc->event_dequeue * sizeof(struct trb)) | ERDP_BUSY);
    }
}

static void xhci_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    for (int i = 0; i < controller_count; i++) {
        controllers[i]->interrupted = true;
        wait_queue_wake_all(&controllers[i]->interrupts);
    }
}

static bool interrupted(void *arg) {
    return ((struct xhci *)arg)->interrupted;
}

static void event_thread(void *arg) {
    struct xhci *hc = arg;
    for (;;) {
        wait_queue_wait_timeout(&hc->interrupts, interrupted, hc, 10, false);
        hc->interrupted = false;
        write32(hc->rt, RT_IMAN, read32(hc->rt, RT_IMAN) | IMAN_PENDING);
        write32(hc->op, OP_USBSTS, STS_EVENT);
        process_events(hc);
    }
}

/* ---- Commands ---- */

static bool command_finished(void *arg) {
    return __atomic_load_n(&((struct xhci *)arg)->command_done, __ATOMIC_ACQUIRE);
}

/* Runs a command; returns its completion code (or 0 if it never finished).
 * `slot_out` gets the slot it names (Enable Slot). */
static uint32_t command(struct xhci *hc, uint64_t parameter, uint32_t control, uint32_t *slot_out) {
    mutex_lock(&hc->command_lock);
    hc->command_done = false;
    hc->command_trb = ring_push(&hc->commands, parameter, 0, control);
    hc->doorbells[0] = 0;
    wait_queue_wait_timeout(&hc->waiters, command_finished, hc, 2000, false);
    uint32_t code = hc->command_done ? hc->command_code : 0;
    if (slot_out) {
        *slot_out = hc->command_slot;
    }
    mutex_unlock(&hc->command_lock);
    if (code != CODE_SUCCESS) {
        kprintf("[xhci] command %u failed (code %u)\n", TRB_TYPE_OF(control), code);
    }
    return code;
}

/* ---- Devices (slots) ---- */

static struct endpoint *new_endpoint(void) {
    struct endpoint *ep = kzalloc(sizeof(*ep));
    if (ep && !ring_init(&ep->ring)) {
        kfree(ep);
        return NULL;
    }
    return ep;
}

static void free_endpoint(struct endpoint *ep) {
    if (ep) {
        free_page(ep->ring.trbs);
        free_page(ep->buffer);
        kfree(ep);
    }
}

static void free_slot(struct xhci *hc, struct slot *slot) {
    if (!slot) {
        return;
    }
    hc->slots[slot->id] = NULL;
    hc->dcbaa[slot->id] = 0;
    for (int i = 0; i < 32; i++) {
        free_endpoint(slot->endpoints[i]);
    }
    free_page(slot->output);
    free_page(slot->input);
    free_page(slot->bounce);
    kfree(slot);
}

static uint16_t default_max_packet0(enum usb_speed speed) {
    switch (speed) {
    case USB_SPEED_LOW:
    case USB_SPEED_FULL: return 8;
    case USB_SPEED_HIGH: return 64;
    default: return 512;
    }
}

static int xhci_address_device(struct usb_hc *usb, struct usb_device *device) {
    struct xhci *hc = usb->data;
    uint32_t id = 0;
    if (command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &id) != CODE_SUCCESS || id == 0 ||
        id > (uint32_t)hc->max_slots) {
        return -VX_EIO;
    }
    struct slot *slot = kzalloc(sizeof(*slot));
    if (!slot) {
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | id << 24, NULL);
        return -VX_ENOMEM;
    }
    slot->id = (int)id;
    slot->control_lock = (struct mutex)MUTEX_INIT;
    slot->output = dma_page();
    slot->input = dma_page();
    slot->bounce = dma_page();
    slot->endpoints[1] = new_endpoint();
    slot->device = device;
    hc->slots[id] = slot;
    if (!slot->output || !slot->input || !slot->bounce || !slot->endpoints[1]) {
        free_slot(hc, slot);
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | id << 24, NULL);
        return -VX_ENOMEM;
    }
    hc->dcbaa[id] = virt_to_phys(slot->output);
    device->hc_data = slot;

    struct endpoint *ep0 = slot->endpoints[1];
    ep0->max_packet = default_max_packet0(device->speed);
    uint32_t *control = input_context(hc, slot, 0);
    control[1] = 0x3; /* Add the slot context and endpoint 0's. */
    fill_slot_context(hc, slot, device, 1);
    uint32_t *ep = input_context(hc, slot, 2);
    ep[1] = 3U << 1 | 4U << 3 | (uint32_t)ep0->max_packet << 16; /* 3 retries, control. */
    ep[2] = (uint32_t)ring_dequeue_pointer(&ep0->ring);
    ep[3] = (uint32_t)(ring_dequeue_pointer(&ep0->ring) >> 32);
    ep[4] = 8; /* Average TRB length. */
    ep0->configured = true;
    if (command(hc, virt_to_phys(slot->input), TRB_TYPE(TRB_ADDRESS_DEVICE) | id << 24, NULL) !=
        CODE_SUCCESS) {
        device->hc_data = NULL;
        free_slot(hc, slot);
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | id << 24, NULL);
        return -VX_EIO;
    }
    return 0;
}

static int xhci_set_max_packet0(struct usb_hc *usb, struct usb_device *device, uint16_t size) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    if (slot->endpoints[1]->max_packet == size) {
        return 0;
    }
    memset(slot->input, 0, PAGE_SIZE);
    input_context(hc, slot, 0)[1] = 0x2; /* Endpoint 0 changes. */
    uint32_t *ep = input_context(hc, slot, 2);
    ep[1] = 3U << 1 | 4U << 3 | (uint32_t)size << 16;
    ep[2] = (uint32_t)ring_dequeue_pointer(&slot->endpoints[1]->ring);
    ep[3] = (uint32_t)(ring_dequeue_pointer(&slot->endpoints[1]->ring) >> 32);
    ep[4] = 8;
    if (command(hc, virt_to_phys(slot->input), TRB_TYPE(TRB_EVALUATE_CONTEXT) | slot->id << 24,
                NULL) != CODE_SUCCESS) {
        return -VX_EIO;
    }
    slot->endpoints[1]->max_packet = size;
    return 0;
}

/* The endpoint context's interval: 2^n * 125 microseconds. */
static uint32_t interval_of(struct usb_device *device, const struct usb_endpoint *e) {
    if (e->type != USB_ENDPOINT_INTERRUPT && e->type != USB_ENDPOINT_ISOCHRONOUS) {
        return 0;
    }
    if (device->speed == USB_SPEED_HIGH || device->speed == USB_SPEED_SUPER ||
        e->type == USB_ENDPOINT_ISOCHRONOUS) {
        uint32_t n = e->interval ? e->interval - 1U : 0;
        return n > 15 ? 15 : n;
    }
    /* Low and full speed: bInterval is in milliseconds (8 * 125 us). */
    uint32_t units = (uint32_t)(e->interval ? e->interval : 1) * 8, n = 0;
    while ((2U << n) <= units && n < 10) {
        n++;
    }
    return n < 3 ? 3 : n;
}

static int xhci_configure(struct usb_hc *usb, struct usb_device *device) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    memset(slot->input, 0, PAGE_SIZE);
    uint32_t *control = input_context(hc, slot, 0);
    control[1] = 1; /* The slot context... */
    int last = 1;
    for (int i = 0; i < device->interface_count; i++) {
        const struct usb_interface *interface = &device->interfaces[i];
        for (int j = 0; j < interface->endpoint_count; j++) {
            const struct usb_endpoint *e = &interface->endpoints[j];
            if (e->type == USB_ENDPOINT_ISOCHRONOUS || e->type == USB_ENDPOINT_CONTROL) {
                continue; /* (Not used by any driver here.) */
            }
            int dci = dci_of(e->address);
            if (!slot->endpoints[dci] && !(slot->endpoints[dci] = new_endpoint())) {
                return -VX_ENOMEM;
            }
            struct endpoint *ep = slot->endpoints[dci];
            ep->max_packet = e->max_packet & 0x7ff;
            bool in = e->address & USB_DIR_IN;
            uint32_t type = e->type == USB_ENDPOINT_BULK ? (in ? 6 : 2) : (in ? 7 : 3);
            uint32_t *ctx = input_context(hc, slot, dci + 1);
            uint32_t esit = (uint32_t)ep->max_packet * (e->max_burst + 1U);
            ctx[0] = interval_of(device, e) << 16 | (esit >> 16) << 24;
            ctx[1] = 3U << 1 | type << 3 | (uint32_t)e->max_burst << 8 |
                     (uint32_t)ep->max_packet << 16;
            ctx[2] = (uint32_t)ring_dequeue_pointer(&ep->ring);
            ctx[3] = (uint32_t)(ring_dequeue_pointer(&ep->ring) >> 32);
            ctx[4] = (e->type == USB_ENDPOINT_INTERRUPT ? esit : 3072) | (esit & 0xffff) << 16;
            control[1] |= 1U << dci;
            ep->configured = true;
            last = dci > last ? dci : last;
        }
    }
    fill_slot_context(hc, slot, device, last);
    if (command(hc, virt_to_phys(slot->input), TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | slot->id << 24,
                NULL) != CODE_SUCCESS) {
        return -VX_EIO;
    }
    return 0;
}

static void xhci_free_device(struct usb_hc *usb, struct usb_device *device) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    if (!slot) {
        return;
    }
    device->hc_data = NULL;
    command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot->id << 24, NULL);
    free_slot(hc, slot);
}

/* ---- Transfers ---- */

static bool transfer_finished(void *arg) {
    return __atomic_load_n(&((struct endpoint *)arg)->done, __ATOMIC_ACQUIRE);
}

/* After a stall, error or timeout: the endpoint goes again from where the
 * ring is now. */
static void restart_endpoint(struct xhci *hc, struct slot *slot, int dci, bool stalled) {
    struct endpoint *ep = slot->endpoints[dci];
    if (stalled) {
        command(hc, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | (uint32_t)dci << 16 | (uint32_t)slot->id << 24,
                NULL);
    } else {
        command(hc, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | (uint32_t)dci << 16 | (uint32_t)slot->id << 24,
                NULL);
    }
    command(hc, ring_dequeue_pointer(&ep->ring),
            TRB_TYPE(TRB_SET_DEQUEUE) | (uint32_t)dci << 16 | (uint32_t)slot->id << 24, NULL);
}

/* Waits for the queued transfer; returns the bytes moved or an error. */
static int finish_transfer(struct xhci *hc, struct slot *slot, int dci, uint32_t timeout_ms) {
    struct endpoint *ep = slot->endpoints[dci];
    hc->doorbells[slot->id] = (uint32_t)dci;
    wait_queue_wait_timeout(&hc->waiters, transfer_finished, ep, timeout_ms, false);
    if (!ep->done) {
        ep->trb_count = 0;
        restart_endpoint(hc, slot, dci, false);
        return -VX_ETIMEDOUT;
    }
    ep->trb_count = 0;
    switch (ep->code) {
    case CODE_SUCCESS:
    case CODE_SHORT_PACKET:
        return (int)ep->transferred;
    case CODE_STALL:
        restart_endpoint(hc, slot, dci, true);
        return -VX_EPIPE;
    default:
        restart_endpoint(hc, slot, dci, true);
        return -VX_EIO;
    }
}

static int xhci_control(struct usb_hc *usb, struct usb_device *device, const uint8_t setup[8],
                        void *data, uint16_t length) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    if (!slot || device->gone) {
        return -VX_ENODEV;
    }
    if (length > PAGE_SIZE) {
        return -VX_EINVAL;
    }
    bool in = setup[0] & USB_DIR_IN;
    mutex_lock(&slot->control_lock);
    struct endpoint *ep = slot->endpoints[1];
    if (!in && length) {
        memcpy(slot->bounce, data, length);
    }
    uint64_t packet;
    memcpy(&packet, setup, 8);
    ep->done = false;
    ep->partial = false;
    ep->transferred = 0;
    ep->trb_count = 0;
    uint32_t transfer_type = length == 0 ? 0 : in ? 3 : 2;
    ring_push(&ep->ring, packet, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | transfer_type << 16);
    if (length) {
        ep->lengths[ep->trb_count] = length;
        ep->trbs[ep->trb_count++] =
            ring_push(&ep->ring, virt_to_phys(slot->bounce), length,
                      TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN | TRB_ISP : 0));
    }
    ep->lengths[ep->trb_count] = 0;
    ep->trbs[ep->trb_count++] =
        ring_push(&ep->ring, 0, 0,
                  TRB_TYPE(TRB_STATUS) | TRB_IOC | ((length && in) ? 0 : TRB_DIR_IN));
    int result = finish_transfer(hc, slot, 1, 3000);
    if (in && result > 0) {
        memcpy(data, slot->bounce, (size_t)result);
    }
    mutex_unlock(&slot->control_lock);
    return result;
}

static int xhci_bulk(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint, void *data,
                     uint32_t length, uint32_t timeout_ms) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    int dci = dci_of(endpoint);
    struct endpoint *ep = slot ? slot->endpoints[dci] : NULL;
    if (!ep || !ep->configured || device->gone) {
        return -VX_ENODEV;
    }
    if (length > BULK_MAX || length == 0) {
        return -VX_EINVAL;
    }
    /* No TRB may cross a 64 KiB boundary: a transfer that would is two
     * (split at a whole number of packets, the buffer being sector-aligned). */
    uint64_t phys = virt_to_phys(data);
    uint32_t done = 0;
    while (done < length) {
        uint32_t piece = (uint32_t)(0x10000 - ((phys + done) & 0xffff));
        piece = piece < length - done ? piece : length - done;
        ep->done = false;
        ep->partial = false;
        ep->transferred = 0;
        ep->lengths[0] = piece;
        ep->trbs[0] = ring_push(&ep->ring, phys + done, piece,
                                TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
        ep->trb_count = 1;
        int result = finish_transfer(hc, slot, dci, timeout_ms);
        if (result < 0) {
            return result;
        }
        done += (uint32_t)result;
        if ((uint32_t)result < piece) {
            break; /* Short: that's all there is. */
        }
    }
    return (int)done;
}

static int xhci_interrupt_in(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint,
                             uint16_t size, usb_report_fn callback, void *arg) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    int dci = dci_of(endpoint | USB_DIR_IN);
    struct endpoint *ep = slot ? slot->endpoints[dci] : NULL;
    if (!ep || !ep->configured || size == 0 || size > PAGE_SIZE) {
        return -VX_EINVAL;
    }
    if (!ep->buffer && !(ep->buffer = dma_page())) {
        return -VX_ENOMEM;
    }
    ep->size = size;
    ep->arg = arg;
    ep->trbs[0] = ring_push(&ep->ring, virt_to_phys(ep->buffer), size,
                            TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ep->trb_count = 1;
    __atomic_store_n(&ep->callback, callback, __ATOMIC_RELEASE);
    hc->doorbells[slot->id] = (uint32_t)dci;
    return 0;
}

/* Starts an isochronous OUT endpoint of an alternate setting (which the
 * driver chose with SET_INTERFACE): adds it to the slot, then keeps
 * ISO_SLOTS packets queued, each filled by `fill` just before it's queued. */
static int xhci_iso_out(struct usb_hc *usb, struct usb_device *device,
                        const struct usb_endpoint *e, uint16_t packet, usb_iso_fill_fn fill,
                        void *arg) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    int dci = dci_of(e->address & ~USB_DIR_IN);
    if (!slot || !packet || packet > PAGE_SIZE / 2 || (e->address & USB_DIR_IN)) {
        return -VX_EINVAL;
    }
    if (!slot->endpoints[dci] && !(slot->endpoints[dci] = new_endpoint())) {
        return -VX_ENOMEM;
    }
    struct endpoint *ep = slot->endpoints[dci];
    if (!ep->buffer && !(ep->buffer = dma_page())) {
        return -VX_ENOMEM;
    }
    ep->max_packet = e->max_packet & 0x7ff;
    int last = dci;
    for (int i = 1; i < 32; i++) {
        if (slot->endpoints[i] && slot->endpoints[i]->configured && i > last) {
            last = i;
        }
    }
    memset(slot->input, 0, PAGE_SIZE);
    uint32_t *control = input_context(hc, slot, 0);
    control[1] = 1U | 1U << dci;
    /* bInterval: 2^(n-1) frames at full speed (8 microframes each), 2^(n-1)
     * microframes at high speed. */
    uint32_t n = e->interval ? e->interval - 1U : 0;
    uint32_t interval = device->speed == USB_SPEED_HIGH || device->speed == USB_SPEED_SUPER ? n : n + 3;
    interval = interval > 15 ? 15 : interval;
    uint32_t *ctx = input_context(hc, slot, dci + 1);
    ctx[0] = interval << 16;
    ctx[1] = 1U << 3 /* Isoch OUT, no error retries */ | (uint32_t)ep->max_packet << 16;
    ctx[2] = (uint32_t)ring_dequeue_pointer(&ep->ring);
    ctx[3] = (uint32_t)(ring_dequeue_pointer(&ep->ring) >> 32);
    ctx[4] = packet | (uint32_t)ep->max_packet << 16;
    fill_slot_context(hc, slot, device, last);
    if (command(hc, virt_to_phys(slot->input), TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | slot->id << 24,
                NULL) != CODE_SUCCESS) {
        return -VX_EIO;
    }
    ep->configured = true;
    ep->iso_packet = packet;
    ep->iso_slots = (int)(PAGE_SIZE / packet) < ISO_SLOTS ? (int)(PAGE_SIZE / packet) : ISO_SLOTS;
    ep->iso_next = 0;
    ep->arg = arg;
    ep->iso_fill = fill;
    for (int i = 0; i < ep->iso_slots; i++) {
        iso_queue(ep);
    }
    hc->doorbells[slot->id] = (uint32_t)dci;
    return 0;
}

static void xhci_iso_stop(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    int dci = dci_of(endpoint & ~USB_DIR_IN);
    struct endpoint *ep = slot ? slot->endpoints[dci] : NULL;
    if (!ep || !ep->iso_fill) {
        return;
    }
    __atomic_store_n(&ep->iso_fill, NULL, __ATOMIC_RELEASE);
    if (!device->gone) {
        command(hc, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | (uint32_t)dci << 16 | (uint32_t)slot->id << 24,
                NULL);
    }
}

static int xhci_reset_endpoint(struct usb_hc *usb, struct usb_device *device, uint8_t endpoint) {
    struct xhci *hc = usb->data;
    struct slot *slot = device->hc_data;
    int dci = dci_of(endpoint);
    if (!slot || !slot->endpoints[dci]) {
        return -VX_EINVAL;
    }
    restart_endpoint(hc, slot, dci, true);
    return 0;
}

/* ---- Root ports ---- */

static bool xhci_port_connected(struct usb_hc *usb, int port) {
    struct xhci *hc = usb->data;
    return read32(hc->op, OP_PORTSC(port)) & PORT_CONNECTED;
}

static int xhci_reset_port(struct usb_hc *usb, int port) {
    struct xhci *hc = usb->data;
    uint32_t portsc = read32(hc->op, OP_PORTSC(port));
    if (!(portsc & PORT_CONNECTED)) {
        return -VX_ENODEV;
    }
    uint32_t keep = portsc & ~(PORT_ENABLED | PORT_CHANGES);
    if (hc->usb3_port[port - 1]) {
        /* USB 3 ports enable themselves once the link is up; a warm reset if not. */
        if (!(portsc & PORT_ENABLED)) {
            write32(hc->op, OP_PORTSC(port), keep | PORT_WARM_RESET);
        }
    } else {
        write32(hc->op, OP_PORTSC(port), keep | PORT_RESET);
    }
    uint64_t deadline = timer_ms() + 500;
    while (timer_ms() < deadline) {
        portsc = read32(hc->op, OP_PORTSC(port));
        if ((portsc & PORT_ENABLED) && !(portsc & PORT_RESET)) {
            break;
        }
        thread_sleep_ms(5);
    }
    write32(hc->op, OP_PORTSC(port), (portsc & ~(PORT_ENABLED | PORT_CHANGES)) |
                                         (portsc & (PORT_RESET_CHANGE | PORT_WARM_RESET_CHANGE)));
    if (!(portsc & PORT_ENABLED)) {
        return -VX_EIO;
    }
    thread_sleep_ms(10); /* Reset recovery. */
    return speed_of((int)PORT_SPEED(portsc));
}

static const struct usb_hc_ops xhci_ops = {
    .address_device = xhci_address_device,
    .set_max_packet0 = xhci_set_max_packet0,
    .configure = xhci_configure,
    .free_device = xhci_free_device,
    .control = xhci_control,
    .bulk = xhci_bulk,
    .interrupt_in = xhci_interrupt_in,
    .iso_out = xhci_iso_out,
    .iso_stop = xhci_iso_stop,
    .reset_endpoint = xhci_reset_endpoint,
    .reset_port = xhci_reset_port,
    .port_connected = xhci_port_connected,
};

/* ---- Setting up a controller ---- */

/* Extended capabilities: taking it from the firmware (BIOS handoff), and
 * which ports are USB 3. */
static void extended_capabilities(struct xhci *hc) {
    uint32_t offset = (read32(hc->cap, CAP_HCCPARAMS1) >> 16) * 4;
    for (int guard = 0; offset && guard < 64; guard++) {
        uint32_t header = read32(hc->cap, offset);
        uint8_t id = header & 0xff;
        if (id == 1) { /* USB Legacy Support. */
            if (header & (1U << 16)) {
                write32(hc->cap, offset, header | (1U << 24));
                uint64_t deadline = timer_ms() + 1000;
                while ((read32(hc->cap, offset) & (1U << 16)) && timer_ms() < deadline) {
                    thread_sleep_ms(1);
                }
                if (read32(hc->cap, offset) & (1U << 16)) {
                    kprintf("[xhci] the firmware kept the controller; taking it anyway\n");
                    write32(hc->cap, offset, (read32(hc->cap, offset) & ~(1U << 16)) | (1U << 24));
                }
            }
            /* No more SMIs from it. */
            uint32_t legacy = read32(hc->cap, offset + 4);
            write32(hc->cap, offset + 4, (legacy & ~0xe011U) | 0xe0000000U);
        } else if (id == 2) { /* Supported Protocol: a range of ports. */
            uint8_t major = header >> 24;
            uint32_t ports = read32(hc->cap, offset + 8);
            int first = ports & 0xff, count = (ports >> 8) & 0xff;
            for (int p = first; p < first + count && p <= 64; p++) {
                if (p >= 1) {
                    hc->usb3_port[p - 1] = major >= 3;
                }
            }
        }
        uint32_t next = (header >> 8) & 0xff;
        offset = next ? offset + next * 4 : 0;
    }
}

static bool reset_controller(struct xhci *hc) {
    write32(hc->op, OP_USBCMD, read32(hc->op, OP_USBCMD) & ~CMD_RUN);
    if (!wait_register(hc->op, OP_USBSTS, STS_HALTED, STS_HALTED, 100)) {
        return false;
    }
    write32(hc->op, OP_USBCMD, CMD_RESET);
    return wait_register(hc->op, OP_USBCMD, CMD_RESET, 0, 1000) &&
           wait_register(hc->op, OP_USBSTS, STS_NOT_READY, 0, 1000);
}

static void probe(struct pci_device *pci) {
    if (controller_count == MAX_CONTROLLERS) {
        return;
    }
    pci_enable(pci);
    struct xhci *hc = kzalloc(sizeof(*hc));
    volatile uint8_t *base = pci_map_bar(pci, 0);
    if (!hc || !base) {
        kfree(hc);
        return;
    }
    hc->pci = pci;
    hc->cap = base;
    hc->op = base + (read32(base, CAP_LENGTH) & 0xff);
    hc->rt = base + (read32(base, CAP_RTSOFF) & ~0x1fU);
    hc->doorbells = (volatile uint32_t *)(base + (read32(base, CAP_DBOFF) & ~0x3U));
    uint32_t params1 = read32(base, CAP_HCSPARAMS1), params2 = read32(base, CAP_HCSPARAMS2);
    uint32_t cparams = read32(base, CAP_HCCPARAMS1);
    hc->max_slots = (int)(params1 & 0xff) < MAX_SLOTS ? (int)(params1 & 0xff) : MAX_SLOTS;
    hc->ports = (int)(params1 >> 24) < 64 ? (int)(params1 >> 24) : 64;
    hc->context_size = (cparams & (1U << 2)) ? 64 : 32;
    hc->command_lock = (struct mutex)MUTEX_INIT;
    hc->waiters = (struct wait_queue)WAIT_QUEUE_INIT;
    hc->interrupts = (struct wait_queue)WAIT_QUEUE_INIT;

    extended_capabilities(hc);
    if (!reset_controller(hc)) {
        kprintf("[xhci] the controller didn't reset\n");
        return;
    }

    hc->dcbaa = dma_page();
    hc->events = dma_page();
    uint64_t *erst = dma_page();
    if (!hc->dcbaa || !hc->events || !erst || !ring_init(&hc->commands)) {
        kprintf("[xhci] out of memory\n");
        return;
    }
    /* Scratchpad pages: memory the controller keeps for itself. */
    uint32_t scratchpads = ((params2 >> 21) & 0x1f) << 5 | ((params2 >> 27) & 0x1f);
    if (scratchpads) {
        uint64_t *array = dma_page();
        for (uint32_t i = 0; array && i < scratchpads && i < PAGE_SIZE / 8; i++) {
            void *page = dma_page();
            array[i] = page ? virt_to_phys(page) : 0;
        }
        hc->dcbaa[0] = array ? virt_to_phys(array) : 0;
    }
    write32(hc->op, OP_CONFIG, (read32(hc->op, OP_CONFIG) & ~0xffU) | (uint32_t)hc->max_slots);
    write64(hc->op, OP_DCBAAP, virt_to_phys(hc->dcbaa));
    write64(hc->op, OP_CRCR, hc->commands.phys | 1);

    erst[0] = virt_to_phys(hc->events);
    erst[1] = RING_TRBS;
    hc->event_cycle = 1;
    write32(hc->rt, RT_ERSTSZ, 1);
    write64(hc->rt, RT_ERDP, virt_to_phys(hc->events));
    write64(hc->rt, RT_ERSTBA, virt_to_phys(erst));
    write32(hc->rt, RT_IMOD, 1000); /* At most an interrupt each 250 us. */
    write32(hc->rt, RT_IMAN, IMAN_ENABLE | IMAN_PENDING);

    controllers[controller_count++] = hc;
    bool msi = pci_enable_msi(pci, xhci_interrupt);
    write32(hc->op, OP_USBCMD, CMD_RUN | CMD_INTERRUPTS);
    if (!wait_register(hc->op, OP_USBSTS, STS_HALTED, 0, 100)) {
        kprintf("[xhci] the controller didn't start\n");
        controller_count--;
        return;
    }
    /* Power on every port (where that's the driver's to do). */
    for (int port = 1; port <= hc->ports; port++) {
        uint32_t portsc = read32(hc->op, OP_PORTSC(port));
        if (!(portsc & PORT_POWER)) {
            write32(hc->op, OP_PORTSC(port), (portsc & ~(PORT_ENABLED | PORT_CHANGES)) | PORT_POWER);
        }
    }

    int usb3 = 0;
    for (int port = 0; port < hc->ports; port++) {
        usb3 += hc->usb3_port[port];
    }
    hc->usb.ops = &xhci_ops;
    hc->usb.data = hc;
    hc->usb.name = "xhci";
    hc->usb.ports = hc->ports;
    hc->usb.node = pci->node;
    pci_claim(pci, "xhci", NULL);
    device_set_details(pci->node, "%d ports (%d USB 3), %d slots%s", hc->ports, usb3,
                       hc->max_slots, msi ? "" : ", polled");
    kprintf("[xhci] %d ports (%d USB 3), %d slots, %d-byte contexts, %s\n", hc->ports, usb3,
            hc->max_slots, hc->context_size, msi ? "MSI interrupts" : "polling");
    thread_create("xhci", event_thread, hc);
    usb_add_controller(&hc->usb);
}

void xhci_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->class_code == 0x0c && pci->subclass == 0x03 && pci->prog_if == 0x30) {
            probe(pci);
        }
    }
}
