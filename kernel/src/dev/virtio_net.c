#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/pci.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/virtio.h>

/*
 * virtio network card (QEMU's virtio-net-pci), through the modern virtio 1.0
 * interface. Queue 0 receives: it always holds buffers for the card to fill.
 * Queue 1 sends: one descriptor per frame, taken back once the card is done.
 * Both queues have QUEUE_SIZE buffers of 2 KiB. The interrupt only wakes the
 * network thread, which does the work (see net_interface.poll).
 */

#define VIRTIO_NET_TRANSITIONAL 0x1000
#define VIRTIO_NET_MODERN 0x1041

#define NET_FEATURE_MAC 5

#define QUEUE_SIZE 64
#define BUFFER_SIZE 2048
#define BUFFER_ORDER 5 /* QUEUE_SIZE * BUFFER_SIZE = 128 KiB = 2^5 pages. */

/* Every frame starts with this header (all zero: no offloads). */
struct __attribute__((packed)) net_header {
    uint8_t flags, gso_type;
    uint16_t header_length, gso_size, csum_start, csum_offset, num_buffers;
};

struct __attribute__((packed)) avail_ring {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[QUEUE_SIZE];
};

struct __attribute__((packed)) used_ring {
    uint16_t flags;
    uint16_t index;
    struct {
        uint32_t id;
        uint32_t length;
    } ring[QUEUE_SIZE];
};

struct queue {
    struct virtio_descriptor *descriptors;
    volatile struct avail_ring *avail;
    volatile struct used_ring *used;
    volatile uint16_t *notify;
    uint16_t number, size, last_used;
    uint8_t *buffers; /* size * BUFFER_SIZE bytes. */
};

struct virtio_card {
    struct net_interface net;
    struct virtio_device device;
    struct queue rx, tx;
    bool tx_busy[QUEUE_SIZE];
    int tx_free;
    struct spinlock tx_lock;
};

static struct virtio_card *cards[4];
static int card_count;

static void card_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    net_wake();
}

static bool setup_queue(struct virtio_card *card, struct queue *q, uint16_t number) {
    volatile struct virtio_common_cfg *common = card->device.common;
    common->queue_select = number;
    if (common->queue_size == 0) {
        return false;
    }
    q->number = number;
    q->size = common->queue_size < QUEUE_SIZE ? common->queue_size : QUEUE_SIZE;
    q->descriptors = virtio_dma_page();
    q->avail = virtio_dma_page();
    q->used = virtio_dma_page();
    uint64_t buffers = pmm_alloc(BUFFER_ORDER);
    if (!q->descriptors || !q->avail || !q->used || !buffers) {
        return false;
    }
    q->buffers = phys_to_virt(buffers);
    for (uint16_t i = 0; i < q->size; i++) {
        q->descriptors[i].address = buffers + (uint64_t)i * BUFFER_SIZE;
        q->descriptors[i].length = BUFFER_SIZE;
        q->descriptors[i].flags = number == 0 ? VIRTIO_DESC_WRITE : 0;
    }
    common->queue_size = q->size;
    common->queue_desc = virt_to_phys(q->descriptors);
    common->queue_driver = virt_to_phys((void *)q->avail);
    common->queue_device = virt_to_phys((void *)q->used);
    q->notify = virtio_queue_notify(&card->device);
    return true;
}

/* Takes back the transmit buffers the card is done with. */
static void reclaim_tx(struct virtio_card *card) {
    struct queue *q = &card->tx;
    while (q->last_used != q->used->index) {
        uint32_t id = q->used->ring[q->last_used % q->size].id;
        if (id < q->size && card->tx_busy[id]) {
            card->tx_busy[id] = false;
            card->tx_free++;
        }
        q->last_used++;
    }
}

static void card_transmit(struct net_interface *net, const void *frame, size_t length) {
    struct virtio_card *card = net->driver;
    struct queue *q = &card->tx;
    if (length + sizeof(struct net_header) > BUFFER_SIZE) {
        net->tx_dropped++;
        return;
    }
    uint64_t flags = spin_lock_irqsave(&card->tx_lock);
    reclaim_tx(card);
    int slot = -1;
    for (int i = 0; i < q->size && card->tx_free; i++) {
        if (!card->tx_busy[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&card->tx_lock, flags);
        net->tx_dropped++; /* Card busy: dropped, as on a full wire. */
        return;
    }
    uint8_t *buffer = q->buffers + (size_t)slot * BUFFER_SIZE;
    memset(buffer, 0, sizeof(struct net_header));
    memcpy(buffer + sizeof(struct net_header), frame, length);
    q->descriptors[slot].length = (uint32_t)(sizeof(struct net_header) + length);
    card->tx_busy[slot] = true;
    card->tx_free--;
    q->avail->ring[q->avail->index % q->size] = (uint16_t)slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* The buffer before the index. */
    q->avail->index++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify = q->number;
    spin_unlock_irqrestore(&card->tx_lock, flags);
}

static void card_poll(struct net_interface *net) {
    struct virtio_card *card = net->driver;
    struct queue *q = &card->rx;
    bool returned = false;
    while (q->last_used != q->used->index) {
        uint16_t slot = q->last_used % q->size;
        uint32_t id = q->used->ring[slot].id;
        uint32_t length = q->used->ring[slot].length;
        q->last_used++;
        if (id >= q->size) {
            continue;
        }
        if (length > sizeof(struct net_header) && length <= BUFFER_SIZE) {
            net_receive(net, q->buffers + (size_t)id * BUFFER_SIZE + sizeof(struct net_header),
                        length - sizeof(struct net_header));
        }
        /* Give the buffer back. */
        q->descriptors[id].length = BUFFER_SIZE;
        q->avail->ring[q->avail->index % q->size] = (uint16_t)id;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        q->avail->index++;
        returned = true;
    }
    if (returned) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *q->notify = q->number;
    }
    uint64_t flags = spin_lock_irqsave(&card->tx_lock);
    reclaim_tx(card);
    spin_unlock_irqrestore(&card->tx_lock, flags);
}

static void probe(struct pci_device *pci) {
    struct virtio_card *card = kzalloc(sizeof(*card));
    if (!card || card_count == 4) {
        kfree(card);
        return;
    }
    uint32_t features;
    if (!virtio_find(pci, &card->device) ||
        !virtio_negotiate(&card->device, 1U << NET_FEATURE_MAC, &features)) {
        kprintf("[virtio] %x:%x.%u: network card not usable, skipping\n", pci->bus, pci->slot,
                pci->function);
        kfree(card);
        return;
    }
    volatile struct virtio_common_cfg *common = card->device.common;
    if (!setup_queue(card, &card->rx, 0) || !setup_queue(card, &card->tx, 1)) {
        kprintf("[virtio] network card: can't set up its queues\n");
        return;
    }
    bool interrupts = pci_enable_msi(pci, card_interrupt);
    if (interrupts) {
        common->msix_config = VIRTIO_MSIX_NO_VECTOR;
        for (uint16_t i = 0; i < 2; i++) {
            common->queue_select = i;
            common->queue_msix_vector = 0;
            interrupts = interrupts && common->queue_msix_vector == 0;
        }
    }
    for (uint16_t i = 0; i < 2; i++) {
        common->queue_select = i;
        common->queue_enable = 1;
    }
    common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;

    /* Every receive buffer goes to the card. */
    struct queue *rx = &card->rx;
    for (uint16_t i = 0; i < rx->size; i++) {
        rx->avail->ring[i] = i;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rx->avail->index = rx->size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *rx->notify = 0;
    card->tx_free = card->tx.size;

    struct net_interface *net = &card->net;
    int number = card_count;
    cards[card_count++] = card;
    memcpy(net->name, "eth0", 5);
    net->name[3] = (char)('0' + number);
    if (features & (1U << NET_FEATURE_MAC)) {
        volatile uint8_t *mac = card->device.device_config;
        for (int i = 0; i < ETH_ADDRESS; i++) {
            net->mac[i] = mac[i];
        }
    } else {
        uint8_t fallback[ETH_ADDRESS] = {0x52, 0x54, 0x00, 0x12, 0x34, (uint8_t)(0x56 + number)};
        memcpy(net->mac, fallback, ETH_ADDRESS);
    }
    net->mtu = NET_MTU;
    net->transmit = card_transmit;
    net->poll = card_poll;
    net->driver = card;
    kprintf("[virtio] %s: virtio network card, %s\n", net->name,
            interrupts ? "MSI-X interrupts" : "polling");
    net_register(net);
}

void virtio_net_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->vendor_id == VIRTIO_VENDOR &&
            (pci->device_id == VIRTIO_NET_MODERN || pci->device_id == VIRTIO_NET_TRANSITIONAL)) {
            probe(pci);
        }
    }
}
