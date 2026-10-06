/*
 * AMD PCnet: the PCnet-PCI II (Am79C970A, QEMU's "pcnet") and PCnet-FAST III
 * (Am79C973, VirtualBox's card for many guests), both 1022:2000.
 *
 * Its registers are reached through two ports: one says which register
 * (RAP), the other reads or writes it (RDP for the CSRs, BDP for the BCRs).
 * After a reset they're 16-bit; a 32-bit write of 0 to RDP makes them 32-bit
 * for good. With software style 2 the card takes 32-bit addresses and
 * 16-byte descriptors in rings that it owns while their OWN bit is set; it
 * reads its setup (the rings, this card's address) from an init block in
 * memory. The interrupt (or the network thread's polling, without one) does
 * the work.
 */
#include <vexa/arch.h>
#include <vexa/device.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

#define MAX_CARDS 4

/* The ports, 32-bit (DWIO) once it's switched; the 16-bit reset port too. */
#define PORT_APROM 0x00 /* This card's address, in its first 6 bytes. */
#define PORT_RDP 0x10
#define PORT_RAP 0x14
#define PORT_RESET 0x18
#define PORT_BDP 0x1c
#define PORT_RESET16 0x14

#define CSR0_INIT (1U << 0)
#define CSR0_STRT (1U << 1)
#define CSR0_STOP (1U << 2)
#define CSR0_TDMD (1U << 3) /* Look at the transmit ring now. */
#define CSR0_IENA (1U << 6)
#define CSR0_IDON (1U << 8)
#define CSR0_TINT (1U << 9)
#define CSR0_RINT (1U << 10)
#define CSR0_EVENTS 0x7f00 /* The status bits (written as 1 to clear them). */
#define CSR4_APAD_XMT (1U << 11) /* Short frames padded by the card. */
#define CSR15_PROM (1U << 15)
#define BCR20_SWSTYLE_2 0x0102 /* 32-bit descriptors and addresses. */

#define DESC_OWN (1U << 31)
#define DESC_ERR (1U << 30)
#define DESC_STP (1U << 25) /* Start of packet. */
#define DESC_ENP (1U << 24) /* End of packet. */
#define DESC_ONES 0xf000U   /* (Must be set in the byte count's top bits.) */

#define RX_LOG 5 /* 32 */
#define TX_LOG 4 /* 16 */
#define RX_COUNT (1 << RX_LOG)
#define TX_COUNT (1 << TX_LOG)
#define BUFFER 1536

struct desc {
    uint32_t address;
    uint32_t status; /* OWN, errors, STP/ENP, then the buffer's size, negated. */
    uint32_t misc;   /* Receive: the frame's length. Transmit: more errors. */
    uint32_t reserved;
};

struct init_block {
    uint16_t mode;
    uint8_t rx_log; /* (In the top 4 bits.) */
    uint8_t tx_log;
    uint8_t mac[6];
    uint16_t reserved;
    uint8_t multicast[8]; /* The logical address filter. */
    uint32_t rx_ring, tx_ring;
} __attribute__((packed));

struct card {
    struct net_interface net;
    struct pci_device *pci;
    uint16_t io;
    struct spinlock lock; /* The register ports, and the transmit ring. */
    struct desc *rx, *tx;
    uint8_t *rx_buffers, *tx_buffers;
    struct init_block *init;
    uint32_t rx_next, tx_next, tx_clean;
};

static struct card *cards[MAX_CARDS];
static int card_count;

static uint32_t csr_read(struct card *c, uint32_t n) {
    outl((uint16_t)(c->io + PORT_RAP), n);
    return inl((uint16_t)(c->io + PORT_RDP));
}

static void csr_write(struct card *c, uint32_t n, uint32_t value) {
    outl((uint16_t)(c->io + PORT_RAP), n);
    outl((uint16_t)(c->io + PORT_RDP), value);
}

static void bcr_write(struct card *c, uint32_t n, uint32_t value) {
    outl((uint16_t)(c->io + PORT_RAP), n);
    outl((uint16_t)(c->io + PORT_BDP), value);
}

/* Zeroed pages below 4 GiB (the card takes 32-bit addresses), or NULL. */
static void *dma_pages(unsigned order) {
    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return NULL;
    }
    if (phys + (PAGE_SIZE << order) > 0x100000000ULL) {
        pmm_free(phys, order);
        return NULL;
    }
    void *p = phys_to_virt(phys);
    memset(p, 0, PAGE_SIZE << order);
    return p;
}

static uint32_t low32(const void *p) {
    return (uint32_t)virt_to_phys(p);
}

static void give_rx(struct card *c, uint32_t i) {
    c->rx[i].misc = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    c->rx[i].status = DESC_OWN | DESC_ONES | ((uint32_t)-BUFFER & 0x0fff);
}

static void pcnet_transmit(struct net_interface *net, const void *frame, size_t length) {
    struct card *c = net->driver;
    if (length > BUFFER) {
        net->tx_dropped++;
        return;
    }
    uint64_t flags = spin_lock_irqsave(&c->lock);
    while (c->tx_clean != c->tx_next && !(c->tx[c->tx_clean].status & DESC_OWN)) {
        c->tx_clean = (c->tx_clean + 1) % TX_COUNT;
    }
    uint32_t next = (c->tx_next + 1) % TX_COUNT;
    if (next == c->tx_clean) {
        spin_unlock_irqrestore(&c->lock, flags);
        net->tx_dropped++; /* The ring is full. */
        return;
    }
    uint8_t *buffer = c->tx_buffers + (size_t)c->tx_next * BUFFER;
    memcpy(buffer, frame, length);
    if (length < 60) {
        memset(buffer + length, 0, 60 - length);
        length = 60;
    }
    struct desc *d = &c->tx[c->tx_next];
    d->address = low32(buffer);
    d->misc = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->status = DESC_OWN | DESC_STP | DESC_ENP | DESC_ONES | ((uint32_t)-length & 0x0fff);
    c->tx_next = next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    csr_write(c, 0, CSR0_TDMD | CSR0_IENA);
    spin_unlock_irqrestore(&c->lock, flags);
}

static void pcnet_poll(struct net_interface *net) {
    struct card *c = net->driver;
    uint64_t flags = spin_lock_irqsave(&c->lock);
    uint32_t status = csr_read(c, 0);
    csr_write(c, 0, (status & CSR0_EVENTS) | CSR0_IENA); /* (Clears them.) */
    spin_unlock_irqrestore(&c->lock, flags);
    for (int budget = 0; budget < RX_COUNT; budget++) {
        struct desc *d = &c->rx[c->rx_next];
        uint32_t s = d->status;
        if (s & DESC_OWN) {
            break;
        }
        uint32_t length = d->misc & 0x0fff; /* With the 4-byte CRC. */
        if (!(s & DESC_ERR) && (s & DESC_STP) && (s & DESC_ENP) && length > 4 &&
            length <= BUFFER) {
            net_receive(net, c->rx_buffers + (size_t)c->rx_next * BUFFER, length - 4);
        } else {
            net->rx_dropped++;
        }
        give_rx(c, c->rx_next);
        c->rx_next = (c->rx_next + 1) % RX_COUNT;
    }
}

static bool pcnet_interrupt(void *arg) {
    struct card *c = arg;
    uint64_t flags = spin_lock_irqsave(&c->lock);
    uint32_t status = csr_read(c, 0);
    bool ours = status & (CSR0_RINT | CSR0_TINT | (1U << 15));
    if (ours) {
        /* Off until the network thread has looked (its poll turns them on). */
        csr_write(c, 0, status & CSR0_EVENTS);
    }
    spin_unlock_irqrestore(&c->lock, flags);
    if (ours) {
        net_wake();
    }
    return ours;
}

static bool setup(struct card *c) {
    /* Reset (either width of port), then 32-bit registers and descriptors. */
    inl((uint16_t)(c->io + PORT_RESET));
    inw((uint16_t)(c->io + PORT_RESET16));
    thread_sleep_ms(1);
    /* This card's address: a byte at a time while the ports are 16-bit (32-bit
     * ones take only 32-bit reads). */
    for (int i = 0; i < 6; i++) {
        c->net.mac[i] = inb((uint16_t)(c->io + PORT_APROM + i));
    }
    outl((uint16_t)(c->io + PORT_RDP), 0);
    if ((c->net.mac[0] & c->net.mac[1] & c->net.mac[2]) == 0xff) {
        uint32_t low = inl((uint16_t)(c->io + PORT_APROM)), high = inl((uint16_t)(c->io + 4));
        for (int i = 0; i < 6; i++) {
            c->net.mac[i] = (uint8_t)((i < 4 ? low >> (8 * i) : high >> (8 * (i - 4))) & 0xff);
        }
    }
    csr_write(c, 0, CSR0_STOP);
    bcr_write(c, 20, BCR20_SWSTYLE_2);
    c->rx = dma_pages(0);
    c->tx = dma_pages(0);
    c->init = dma_pages(0);
    c->rx_buffers = dma_pages(4); /* 32 x 1536 bytes */
    c->tx_buffers = dma_pages(3); /* 16 x 1536 bytes */
    if (!c->rx || !c->tx || !c->init || !c->rx_buffers || !c->tx_buffers) {
        return false;
    }
    for (uint32_t i = 0; i < RX_COUNT; i++) {
        c->rx[i].address = low32(c->rx_buffers + (size_t)i * BUFFER);
        give_rx(c, i);
    }
    struct init_block *b = c->init;
    b->mode = 0;
    b->rx_log = RX_LOG << 4;
    b->tx_log = TX_LOG << 4;
    memcpy(b->mac, c->net.mac, 6);
    memset(b->multicast, 0xff, sizeof(b->multicast)); /* Every multicast group (IPv6). */
    b->rx_ring = low32(c->rx);
    b->tx_ring = low32(c->tx);
    uint32_t at = low32(b);
    csr_write(c, 1, at & 0xffff);
    csr_write(c, 2, at >> 16);
    csr_write(c, 4, csr_read(c, 4) | CSR4_APAD_XMT);
    csr_write(c, 15, csr_read(c, 15) & ~CSR15_PROM);
    csr_write(c, 0, CSR0_INIT);
    for (int tries = 0; !(csr_read(c, 0) & CSR0_IDON); tries++) {
        if (tries == 100) {
            return false;
        }
        thread_sleep_ms(1);
    }
    csr_write(c, 0, CSR0_IDON | CSR0_STRT | CSR0_IENA);
    return true;
}

static void probe(struct pci_device *pci) {
    if (card_count == MAX_CARDS || !pci->bar_is_io[0] || !pci->bar[0]) {
        return;
    }
    pci_enable(pci);
    pci_write16(pci, 0x04, pci_read16(pci, 0x04) | 1); /* Command: I/O space on. */
    struct card *c = kzalloc(sizeof(*c));
    if (!c) {
        return;
    }
    c->pci = pci;
    c->io = (uint16_t)pci->bar[0];
    c->lock = (struct spinlock)SPINLOCK_INIT;
    if (!setup(c)) {
        kprintf("[pcnet] %04x: the card didn't start\n", pci->device_id);
        return;
    }
    struct net_interface *net = &c->net;
    net_name(net);
    net->mtu = NET_MTU;
    net->driver = c;
    net->transmit = pcnet_transmit;
    net->poll = pcnet_poll;
    cards[card_count++] = c;
    bool interrupts = pci_attach_interrupt(pci, pcnet_interrupt, c);
    pci_claim(pci, "pcnet", NULL);
    device_set_details(pci->node, "%s, MAC %02x:%02x:%02x:%02x:%02x:%02x", net->name, net->mac[0],
                       net->mac[1], net->mac[2], net->mac[3], net->mac[4], net->mac[5]);
    kprintf("[pcnet] %s: AMD PCnet, %s\n", net->name, interrupts ? "interrupts" : "polling");
    net_register(net);
}

void pcnet_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->vendor_id == 0x1022 && pci->device_id == 0x2000) {
            probe(pci);
        }
    }
}
