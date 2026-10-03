/*
 * Realtek Ethernet: the RTL8139 (100 Mbit/s; QEMU's "rtl8139") and the
 * RTL8111/8168/8101 family (gigabit, on a great many desktop boards).
 *
 * The 8139 receives into one ring buffer that the card writes packets into
 * one after another (each with a 4-byte header), and sends from four fixed
 * buffers in turn. The 8111/8168 uses rings of 16-byte descriptors that the
 * card owns while their OWN bit is set. Either way the interrupt only wakes
 * the network thread, which does the work.
 *
 * The 8111/8168 family has many revisions; this driver does the setup they
 * share (not the per-revision PHY fixes Linux's r8169 applies), so a few
 * may not get a link.
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

#define REALTEK_VENDOR 0x10ec
#define MAX_CARDS 4

/* Registers both share. */
#define REG_MAC 0x00
#define REG_COMMAND 0x37
#define REG_IMR 0x3c
#define REG_ISR 0x3e
#define REG_TX_CONFIG 0x40
#define REG_RX_CONFIG 0x44
#define REG_9346 0x50
#define REG_CONFIG1 0x52

#define COMMAND_RESET 0x10
#define COMMAND_RX 0x08
#define COMMAND_TX 0x04
#define COMMAND_RX_EMPTY 0x01

#define INT_ROK 0x0001
#define INT_RER 0x0002
#define INT_TOK 0x0004
#define INT_TER 0x0008
#define INT_RX_OVERFLOW 0x0010
#define INT_LINK 0x0020
#define INT_FIFO_OVERFLOW 0x0040

/* Receive: this card's address, broadcasts; no limit on DMA bursts. */
#define RX_ACCEPT (0x02 | 0x08) /* Physical match, broadcast. */

struct card {
    struct net_interface net;
    struct pci_device *pci;
    bool gigabit;
    volatile uint8_t *mmio; /* Or I/O ports at io_base. */
    uint16_t io_base;
    struct spinlock tx_lock;
    /* RTL8139. */
    uint8_t *rx_ring;
    uint32_t rx_offset;
    uint8_t *tx_buffers;
    int tx_next;
    /* RTL8111/8168. */
    struct desc *rx, *tx;
    uint8_t *rx_buffers, *gtx_buffers;
    uint32_t rx_next, gtx_next, gtx_clean;
};

static struct card *cards[MAX_CARDS];
static int card_count;

static uint8_t r8(struct card *c, uint32_t reg) {
    return c->mmio ? c->mmio[reg] : inb((uint16_t)(c->io_base + reg));
}
static uint16_t r16(struct card *c, uint32_t reg) {
    return c->mmio ? *(volatile uint16_t *)(c->mmio + reg) : inw((uint16_t)(c->io_base + reg));
}
static uint32_t r32(struct card *c, uint32_t reg) {
    return c->mmio ? *(volatile uint32_t *)(c->mmio + reg) : inl((uint16_t)(c->io_base + reg));
}
static void w8(struct card *c, uint32_t reg, uint8_t v) {
    if (c->mmio) c->mmio[reg] = v;
    else outb((uint16_t)(c->io_base + reg), v);
}
static void w16(struct card *c, uint32_t reg, uint16_t v) {
    if (c->mmio) *(volatile uint16_t *)(c->mmio + reg) = v;
    else outw((uint16_t)(c->io_base + reg), v);
}
static void w32(struct card *c, uint32_t reg, uint32_t v) {
    if (c->mmio) *(volatile uint32_t *)(c->mmio + reg) = v;
    else outl((uint16_t)(c->io_base + reg), v);
}

/* Zeroed pages below 4 GiB (the 8139 takes 32-bit addresses), or NULL. */
static void *dma_pages(unsigned order, bool low) {
    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return NULL;
    }
    if (low && phys + (PAGE_SIZE << order) > 0x100000000ULL) {
        pmm_free(phys, order);
        return NULL;
    }
    void *p = phys_to_virt(phys);
    memset(p, 0, PAGE_SIZE << order);
    return p;
}

static void realtek_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    net_wake();
}

static bool reset(struct card *c) {
    w8(c, REG_COMMAND, COMMAND_RESET);
    for (int tries = 0; tries < 100; tries++) {
        if (!(r8(c, REG_COMMAND) & COMMAND_RESET)) {
            return true;
        }
        thread_sleep_ms(1);
    }
    return false;
}

/* ---- RTL8139 ---- */

#define RX_RING 8192
#define RX_RING_ALLOC (RX_RING + 16 + 1536) /* (The card may write past the end.) */
#define REG_TSD0 0x10
#define REG_TSAD0 0x20
#define REG_RBSTART 0x30
#define REG_CAPR 0x38
#define TSD_OWN (1U << 13)
#define TSD_TOK (1U << 15)
#define TX_SLOTS 4
#define TX_SIZE 2048

static void rtl8139_transmit(struct net_interface *net, const void *frame, size_t length) {
    struct card *c = net->driver;
    if (length > TX_SIZE - 4) {
        net->tx_dropped++;
        return;
    }
    uint64_t flags = spin_lock_irqsave(&c->tx_lock);
    uint32_t reg = REG_TSD0 + 4 * (uint32_t)c->tx_next;
    uint32_t status = r32(c, reg);
    if (!(status & TSD_OWN) && status != 0 && !(status & TSD_TOK)) {
        spin_unlock_irqrestore(&c->tx_lock, flags);
        net->tx_dropped++; /* Still sending what was there. */
        return;
    }
    uint8_t *buffer = c->tx_buffers + (size_t)c->tx_next * TX_SIZE;
    memcpy(buffer, frame, length);
    if (length < 60) {
        memset(buffer + length, 0, 60 - length);
        length = 60;
    }
    w32(c, REG_TSAD0 + 4 * (uint32_t)c->tx_next, (uint32_t)virt_to_phys(buffer));
    w32(c, reg, (uint32_t)length); /* OWN clear: the card sends it. */
    c->tx_next = (c->tx_next + 1) % TX_SLOTS;
    spin_unlock_irqrestore(&c->tx_lock, flags);
}

static void rtl8139_poll(struct net_interface *net) {
    struct card *c = net->driver;
    uint16_t status = r16(c, REG_ISR);
    w16(c, REG_ISR, status); /* (Writing 1s clears them.) */
    for (int budget = 0; budget < 64 && !(r8(c, REG_COMMAND) & COMMAND_RX_EMPTY); budget++) {
        uint8_t *p = c->rx_ring + c->rx_offset;
        uint16_t header = (uint16_t)(p[0] | p[1] << 8);
        uint16_t length = (uint16_t)(p[2] | p[3] << 8); /* With the 4-byte CRC. */
        if (!(header & 1) || length < 64 || length > 1522) {
            /* Garbage: start the receiver over. */
            net->rx_dropped++;
            w8(c, REG_COMMAND, COMMAND_TX);
            w8(c, REG_COMMAND, COMMAND_RX | COMMAND_TX);
            c->rx_offset = 0;
            break;
        }
        net_receive(net, p + 4, length - 4);
        c->rx_offset = (c->rx_offset + length + 4 + 3) & ~3U;
        if (c->rx_offset >= RX_RING) {
            c->rx_offset -= RX_RING;
        }
        w16(c, REG_CAPR, (uint16_t)(c->rx_offset - 16));
    }
}

static bool rtl8139_setup(struct card *c) {
    w8(c, REG_CONFIG1, 0x00); /* Power on. */
    if (!reset(c)) {
        return false;
    }
    c->rx_ring = dma_pages(2, true);   /* 16 KiB >= RX_RING_ALLOC */
    c->tx_buffers = dma_pages(1, true); /* 4 x 2 KiB */
    if (!c->rx_ring || !c->tx_buffers) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        c->net.mac[i] = r8(c, REG_MAC + (uint32_t)i);
    }
    w32(c, REG_RBSTART, (uint32_t)virt_to_phys(c->rx_ring));
    w8(c, REG_COMMAND, COMMAND_RX | COMMAND_TX);
    /* An 8 KiB ring (bits 11-12: 0); bit 7: a packet at the end runs on past it (into
     * the extra room) instead of wrapping; unlimited DMA bursts; ours and broadcasts. */
    w32(c, REG_RX_CONFIG, RX_ACCEPT | 1U << 7 | 7U << 8 | 7U << 13);
    w32(c, REG_TX_CONFIG, 6U << 8 | 3U << 24); /* DMA burst 1024, standard gap. */
    w16(c, REG_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RX_OVERFLOW | INT_LINK |
                        INT_FIFO_OVERFLOW);
    c->net.transmit = rtl8139_transmit;
    c->net.poll = rtl8139_poll;
    return true;
}

/* ---- RTL8111/8168/8101 ---- */

#define REG_TX_DESC 0x20  /* Normal priority transmit ring. */
#define REG_TX_POLL 0x38
#define REG_RX_MAX 0xda
#define REG_CPLUS 0xe0
#define REG_RX_DESC 0xe4
#define REG_TX_MAX 0xec
#define POLL_NORMAL 0x40

#define DESC_OWN (1U << 31)
#define DESC_EOR (1U << 30) /* The last in the ring. */
#define DESC_FS (1U << 29)
#define DESC_LS (1U << 28)
#define DESC_RX_ERROR (1U << 21)

#define G_RX_COUNT 128
#define G_TX_COUNT 64
#define G_BUFFER 2048

struct desc {
    uint32_t options1, options2;
    uint64_t address;
};

static void gigabit_transmit(struct net_interface *net, const void *frame, size_t length) {
    struct card *c = net->driver;
    if (length > G_BUFFER) {
        net->tx_dropped++;
        return;
    }
    uint64_t flags = spin_lock_irqsave(&c->tx_lock);
    while (c->gtx_clean != c->gtx_next && !(c->tx[c->gtx_clean].options1 & DESC_OWN)) {
        c->gtx_clean = (c->gtx_clean + 1) % G_TX_COUNT;
    }
    uint32_t next = (c->gtx_next + 1) % G_TX_COUNT;
    if (next == c->gtx_clean) {
        spin_unlock_irqrestore(&c->tx_lock, flags);
        net->tx_dropped++;
        return;
    }
    uint8_t *buffer = c->gtx_buffers + (size_t)c->gtx_next * G_BUFFER;
    memcpy(buffer, frame, length);
    if (length < 60) {
        memset(buffer + length, 0, 60 - length);
        length = 60;
    }
    struct desc *d = &c->tx[c->gtx_next];
    d->options2 = 0;
    d->address = virt_to_phys(buffer);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->options1 = DESC_OWN | DESC_FS | DESC_LS | (uint32_t)length |
                  (c->gtx_next == G_TX_COUNT - 1 ? DESC_EOR : 0);
    c->gtx_next = next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w8(c, REG_TX_POLL, POLL_NORMAL);
    spin_unlock_irqrestore(&c->tx_lock, flags);
}

static void gigabit_poll(struct net_interface *net) {
    struct card *c = net->driver;
    uint16_t status = r16(c, REG_ISR);
    w16(c, REG_ISR, status);
    for (int budget = 0; budget < G_RX_COUNT; budget++) {
        struct desc *d = &c->rx[c->rx_next];
        uint32_t options = d->options1;
        if (options & DESC_OWN) {
            break;
        }
        uint32_t length = options & 0x3fff; /* With the CRC. */
        if ((options & DESC_FS) && (options & DESC_LS) && !(options & DESC_RX_ERROR) &&
            length > 4 && length <= G_BUFFER) {
            net_receive(net, c->rx_buffers + (size_t)c->rx_next * G_BUFFER, length - 4);
        } else {
            net->rx_dropped++;
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        d->options1 = DESC_OWN | G_BUFFER | (c->rx_next == G_RX_COUNT - 1 ? DESC_EOR : 0);
        c->rx_next = (c->rx_next + 1) % G_RX_COUNT;
    }
}

static bool gigabit_setup(struct card *c) {
    if (!reset(c)) {
        return false;
    }
    c->rx = dma_pages(0, false);
    c->tx = dma_pages(0, false);
    c->rx_buffers = dma_pages(6, false);  /* 128 x 2 KiB */
    c->gtx_buffers = dma_pages(5, false); /* 64 x 2 KiB */
    if (!c->rx || !c->tx || !c->rx_buffers || !c->gtx_buffers) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        c->net.mac[i] = r8(c, REG_MAC + (uint32_t)i);
    }
    for (int i = 0; i < G_RX_COUNT; i++) {
        c->rx[i].address = virt_to_phys(c->rx_buffers + (size_t)i * G_BUFFER);
        c->rx[i].options1 = DESC_OWN | G_BUFFER | (i == G_RX_COUNT - 1 ? DESC_EOR : 0);
    }
    c->tx[G_TX_COUNT - 1].options1 = DESC_EOR;
    w8(c, REG_9346, 0xc0); /* Unlock the configuration registers. */
    w16(c, REG_CPLUS, r16(c, REG_CPLUS) | 0x0008); /* PCI multiple read/write. */
    w16(c, REG_RX_MAX, G_BUFFER - 1);
    w8(c, REG_TX_MAX, 0x3b); /* 8 KiB units of 128: about 7.5 KiB. */
    uint64_t rx = virt_to_phys(c->rx), tx = virt_to_phys(c->tx);
    w32(c, REG_TX_DESC, (uint32_t)tx);
    w32(c, REG_TX_DESC + 4, (uint32_t)(tx >> 32));
    w32(c, REG_RX_DESC, (uint32_t)rx);
    w32(c, REG_RX_DESC + 4, (uint32_t)(rx >> 32));
    w8(c, REG_COMMAND, COMMAND_RX | COMMAND_TX);
    /* No receive threshold, unlimited DMA bursts, ours and broadcasts. */
    w32(c, REG_RX_CONFIG, (r32(c, REG_RX_CONFIG) & 0xff7e1880U) | RX_ACCEPT | 7U << 8 | 7U << 13);
    w32(c, REG_TX_CONFIG, 3U << 24 | 7U << 8); /* Standard gap, unlimited bursts. */
    w8(c, REG_9346, 0x00);
    w16(c, REG_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RX_OVERFLOW | INT_LINK |
                        INT_FIFO_OVERFLOW);
    c->net.transmit = gigabit_transmit;
    c->net.poll = gigabit_poll;
    return true;
}

/* ---- Finding them ---- */

static void probe(struct pci_device *pci, bool gigabit) {
    if (card_count == MAX_CARDS) {
        return;
    }
    pci_enable(pci);
    struct card *c = kzalloc(sizeof(*c));
    if (!c) {
        return;
    }
    c->pci = pci;
    c->gigabit = gigabit;
    c->tx_lock = (struct spinlock)SPINLOCK_INIT;
    /* The registers: a memory BAR if there is one, else the I/O ports in BAR 0. */
    for (int bar = 1; bar < 6 && !c->mmio; bar++) {
        if (pci->bar[bar] && !pci->bar_is_io[bar] && pci->bar_size[bar] >= 256) {
            c->mmio = pci_map_bar(pci, bar);
        }
    }
    if (!c->mmio && pci->bar_is_io[0]) {
        c->io_base = (uint16_t)pci->bar[0];
        pci_write16(pci, 0x04, pci_read16(pci, 0x04) | 1); /* Command: I/O space on. */
    }
    if (!c->mmio && !c->io_base) {
        kfree(c);
        return;
    }
    bool ok = gigabit ? gigabit_setup(c) : rtl8139_setup(c);
    if (!ok) {
        kprintf("[realtek] %04x: the card didn't start\n", pci->device_id);
        return;
    }
    bool interrupts = pci_enable_msi(pci, realtek_interrupt);
    struct net_interface *net = &c->net;
    net_name(net);
    net->mtu = NET_MTU;
    net->driver = c;
    cards[card_count++] = c;
    pci_claim(pci, gigabit ? "r8169" : "rtl8139", NULL);
    device_set_details(pci->node, "%s, MAC %02x:%02x:%02x:%02x:%02x:%02x", net->name, net->mac[0],
                       net->mac[1], net->mac[2], net->mac[3], net->mac[4], net->mac[5]);
    kprintf("[realtek] %s: RTL%04x, %s\n", net->name, pci->device_id,
            interrupts ? "MSI interrupts" : "polling");
    net_register(net);
}

void realtek_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->vendor_id != REALTEK_VENDOR && pci->vendor_id != 0x1186 /* D-Link */) {
            continue;
        }
        switch (pci->device_id) {
        case 0x8139: /* RTL8139 */
        case 0x8138:
            probe(pci, false);
            break;
        case 0x8161: /* RTL8111/8168 and the 8101 (100 Mbit/s) and kin */
        case 0x8167:
        case 0x8168:
        case 0x8169:
        case 0x8136:
        case 0x4300:
        case 0x4302:
            probe(pci, true);
            break;
        }
    }
}
