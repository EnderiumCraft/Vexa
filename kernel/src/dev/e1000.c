/*
 * Intel gigabit Ethernet: the e1000 family (82540 and friends: QEMU's
 * "e1000", VirtualBox's and VMware's cards) and the e1000e family (82574,
 * 82579, and the I217/I218/I219 in many desktops and laptops; QEMU's
 * "e1000e").
 *
 * Both families take the same legacy descriptors: a ring of 16-byte receive
 * descriptors, each with a 2 KiB buffer the card fills, and a ring of
 * transmit descriptors, each pointing at a frame to send. The card says a
 * descriptor is done by setting its DD bit; the head and tail registers say
 * which part of each ring belongs to whom. The interrupt only wakes the
 * network thread, which does the work.
 */
#include <vexa/arch.h>
#include <vexa/device.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/pci.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

#define INTEL_VENDOR 0x8086

/* Registers. */
#define CTRL 0x0000
#define STATUS 0x0008
#define EERD 0x0014
#define CTRL_EXT 0x0018
#define ICR 0x00c0
#define IMS 0x00d0
#define IMC 0x00d8
#define RCTL 0x0100
#define TCTL 0x0400
#define TIPG 0x0410
#define RDBAL 0x2800
#define RDBAH 0x2804
#define RDLEN 0x2808
#define RDH 0x2810
#define RDT 0x2818
#define TDBAL 0x3800
#define TDBAH 0x3804
#define TDLEN 0x3808
#define TDH 0x3810
#define TDT 0x3818
#define MTA 0x5200
#define RAL0 0x5400
#define RAH0 0x5404

#define CTRL_ASDE (1U << 5)
#define CTRL_SLU (1U << 6)
#define CTRL_RST (1U << 26)
#define CTRL_PHY_RST (1U << 31)
#define STATUS_LINK_UP (1U << 1)
#define RCTL_EN (1U << 1)
#define RCTL_BAM (1U << 15) /* Broadcasts. */
#define RCTL_SECRC (1U << 26) /* Strip the CRC. */
#define TCTL_EN (1U << 1)
#define TCTL_PSP (1U << 3)
#define RAH_VALID (1U << 31)
#define INT_TXDW (1U << 0)
#define INT_LSC (1U << 2)
#define INT_RXDMT0 (1U << 4)
#define INT_RXO (1U << 6)
#define INT_RXT0 (1U << 7)

#define DESC_DONE 0x01
#define DESC_EOP 0x02
#define TX_CMD_EOP 0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS 0x08

#define RX_COUNT 128
#define TX_COUNT 64
#define BUFFER_SIZE 2048
#define MAX_CARDS 4

struct rx_desc {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
};

struct tx_desc {
    uint64_t address;
    uint16_t length;
    uint8_t cso, command, status, css;
    uint16_t special;
};

struct card {
    struct net_interface net;
    struct pci_device *pci;
    volatile uint8_t *regs;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buffers, *tx_buffers;
    uint32_t rx_next;           /* The next descriptor the card will fill. */
    uint32_t tx_next, tx_clean; /* Ours to fill; the oldest not yet taken back. */
    struct spinlock tx_lock;
    bool link_up;
};

static struct card *cards[MAX_CARDS];
static int card_count;

/* Devices known to work with legacy descriptors and this setup. */
static const uint16_t e1000_ids[] = {
    0x100e, 0x100f, 0x1004, 0x1008, 0x1009, 0x100c, 0x100d, 0x1010, 0x1011, 0x1012,
    0x1013, 0x1015, 0x1016, 0x1017, 0x1018, 0x1019, 0x101a, 0x101d, 0x1026, 0x1027,
    0x1028, 0x1075, 0x1076, 0x1077, 0x1078, 0x1079, 0x107a, 0x107b, 0x107c, 0x108a,
    0x1099, 0x10b5,
};
static const uint16_t e1000e_ids[] = {
    0x105e, 0x105f, 0x1060, 0x107d, 0x107e, 0x107f, 0x108b, 0x108c, 0x109a, 0x10a4,
    0x10a5, 0x10bc, 0x10b9, 0x10ba, 0x10bb, 0x10bd, 0x10d3, 0x10d5, 0x10d9, 0x10da,
    0x10de, 0x10df, 0x10e5, 0x10ea, 0x10eb, 0x10ef, 0x10f0, 0x10f5, 0x10f6, 0x1501,
    0x1502, 0x1503, 0x150c, 0x153a, 0x153b, 0x155a, 0x1559, 0x15a0, 0x15a1, 0x15a2,
    0x15a3, 0x156f, 0x1570, 0x15b7, 0x15b8, 0x15b9, 0x15bb, 0x15bc, 0x15bd, 0x15be,
    0x15d6, 0x15d7, 0x15d8, 0x15e3, 0x15df, 0x15e0, 0x15e1, 0x15e2, 0x0d4c, 0x0d4d,
    0x0d4e, 0x0d4f, 0x0d53, 0x0d55, 0x15f9, 0x15fa, 0x15fb, 0x15fc, 0x1a1c, 0x1a1d,
    0x1a1e, 0x1a1f, 0x550a, 0x550b, 0x550c, 0x550d,
};

static uint32_t r32(struct card *card, uint32_t reg) {
    return *(volatile uint32_t *)(card->regs + reg);
}

static void w32(struct card *card, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(card->regs + reg) = value;
}

static bool listed(uint16_t id, const uint16_t *ids, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

/* The MAC address: in RAL0/RAH0 once the card has loaded its EEPROM;
 * otherwise read from the EEPROM. */
static void read_mac(struct card *card, bool newer) {
    uint32_t low = r32(card, RAL0), high = r32(card, RAH0);
    if ((high & RAH_VALID) && (low || (high & 0xffff))) {
        for (int i = 0; i < 4; i++) {
            card->net.mac[i] = (uint8_t)(low >> (8 * i));
        }
        card->net.mac[4] = (uint8_t)high;
        card->net.mac[5] = (uint8_t)(high >> 8);
        return;
    }
    for (int word = 0; word < 3; word++) {
        /* (Newer cards: the address at bit 2, done at bit 1; older: 8 and 4.) */
        w32(card, EERD, newer ? (uint32_t)word << 2 | 1 : (uint32_t)word << 8 | 1);
        uint32_t value = 0;
        for (int tries = 0; tries < 10000; tries++) {
            value = r32(card, EERD);
            if (value & (newer ? 0x2U : 0x10U)) {
                break;
            }
        }
        card->net.mac[word * 2] = (uint8_t)(value >> 16);
        card->net.mac[word * 2 + 1] = (uint8_t)(value >> 24);
    }
}

static void card_transmit(struct net_interface *net, const void *frame, size_t length) {
    struct card *card = net->driver;
    if (length > BUFFER_SIZE) {
        net->tx_dropped++;
        return;
    }
    uint64_t flags = spin_lock_irqsave(&card->tx_lock);
    /* Take back what the card has sent. */
    while (card->tx_clean != card->tx_next && (card->tx[card->tx_clean].status & DESC_DONE)) {
        card->tx_clean = (card->tx_clean + 1) % TX_COUNT;
    }
    uint32_t next = (card->tx_next + 1) % TX_COUNT;
    if (next == card->tx_clean) {
        spin_unlock_irqrestore(&card->tx_lock, flags);
        net->tx_dropped++; /* Full: dropped, as on a busy wire. */
        return;
    }
    struct tx_desc *d = &card->tx[card->tx_next];
    memcpy(card->tx_buffers + (size_t)card->tx_next * BUFFER_SIZE, frame, length);
    d->length = (uint16_t)(length < 60 ? 60 : length); /* (The card pads short frames.) */
    d->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    if (length < 60) {
        memset(card->tx_buffers + (size_t)card->tx_next * BUFFER_SIZE + length, 0, 60 - length);
    }
    card->tx_next = next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w32(card, TDT, card->tx_next);
    spin_unlock_irqrestore(&card->tx_lock, flags);
}

static void card_poll(struct net_interface *net) {
    struct card *card = net->driver;
    uint32_t cause = r32(card, ICR); /* (Reading clears it.) */
    if (cause & INT_LSC) {
        bool up = r32(card, STATUS) & STATUS_LINK_UP;
        if (up != card->link_up) {
            card->link_up = up;
            kprintf("[e1000] %s: link %s\n", net->name, up ? "up" : "down");
        }
    }
    uint32_t last = card->rx_next;
    bool returned = false;
    for (int budget = 0; budget < RX_COUNT; budget++) {
        struct rx_desc *d = &card->rx[card->rx_next];
        if (!(d->status & DESC_DONE)) {
            break;
        }
        if ((d->status & DESC_EOP) && !d->errors && d->length >= ETH_HEADER &&
            d->length <= BUFFER_SIZE) {
            net_receive(net, card->rx_buffers + (size_t)card->rx_next * BUFFER_SIZE, d->length);
        } else {
            net->rx_dropped++;
        }
        d->status = 0;
        last = card->rx_next;
        card->rx_next = (card->rx_next + 1) % RX_COUNT;
        returned = true;
    }
    if (returned) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        w32(card, RDT, last); /* The ones just read are the card's again. */
    }
}

static void e1000_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    net_wake();
}

static void *dma_pages(unsigned order) {
    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return NULL;
    }
    void *p = phys_to_virt(phys);
    memset(p, 0, PAGE_SIZE << order);
    return p;
}

static void probe(struct pci_device *pci, bool newer) {
    if (card_count == MAX_CARDS) {
        return;
    }
    pci_enable(pci);
    struct card *card = kzalloc(sizeof(*card));
    if (!card || !(card->regs = pci_map_bar(pci, 0))) {
        kfree(card);
        return;
    }
    card->pci = pci;
    card->tx_lock = (struct spinlock)SPINLOCK_INIT;

    /* Reset, with interrupts off before and after. */
    w32(card, IMC, 0xffffffff);
    w32(card, CTRL, r32(card, CTRL) | CTRL_RST);
    thread_sleep_ms(10);
    for (int tries = 0; tries < 100 && (r32(card, CTRL) & CTRL_RST); tries++) {
        thread_sleep_ms(1);
    }
    w32(card, IMC, 0xffffffff);
    r32(card, ICR);
    /* Link up, speed found by itself. */
    w32(card, CTRL, (r32(card, CTRL) | CTRL_SLU | CTRL_ASDE) & ~CTRL_PHY_RST);
    read_mac(card, newer);
    for (int i = 0; i < 128; i++) {
        w32(card, MTA + 4 * (uint32_t)i, 0); /* No multicast. */
    }

    /* Rings: 128 x 16 bytes, 64 x 16 bytes; buffers 2 KiB each. */
    card->rx = dma_pages(0);
    card->tx = dma_pages(0);
    card->rx_buffers = dma_pages(6); /* 128 * 2 KiB = 256 KiB */
    card->tx_buffers = dma_pages(5); /* 64 * 2 KiB = 128 KiB */
    if (!card->rx || !card->tx || !card->rx_buffers || !card->tx_buffers) {
        kprintf("[e1000] out of memory\n");
        return;
    }
    for (int i = 0; i < RX_COUNT; i++) {
        card->rx[i].address = virt_to_phys(card->rx_buffers + (size_t)i * BUFFER_SIZE);
    }
    for (int i = 0; i < TX_COUNT; i++) {
        card->tx[i].address = virt_to_phys(card->tx_buffers + (size_t)i * BUFFER_SIZE);
        card->tx[i].status = DESC_DONE;
    }
    uint64_t rx = virt_to_phys(card->rx), tx = virt_to_phys(card->tx);
    w32(card, RDBAL, (uint32_t)rx);
    w32(card, RDBAH, (uint32_t)(rx >> 32));
    w32(card, RDLEN, RX_COUNT * sizeof(struct rx_desc));
    w32(card, RDH, 0);
    w32(card, RDT, RX_COUNT - 1);
    w32(card, TDBAL, (uint32_t)tx);
    w32(card, TDBAH, (uint32_t)(tx >> 32));
    w32(card, TDLEN, TX_COUNT * sizeof(struct tx_desc));
    w32(card, TDH, 0);
    w32(card, TDT, 0);
    w32(card, RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC); /* 2 KiB buffers (size bits 0). */
    w32(card, TCTL, TCTL_EN | TCTL_PSP | 0x0fU << 4 | 0x3fU << 12);
    w32(card, TIPG, 10 | 8 << 10 | 6 << 20);

    bool interrupts = pci_enable_msi(pci, e1000_interrupt);
    w32(card, IMS, INT_RXT0 | INT_RXO | INT_RXDMT0 | INT_LSC | INT_TXDW);
    card->link_up = r32(card, STATUS) & STATUS_LINK_UP;

    struct net_interface *net = &card->net;
    net_name(net);
    net->mtu = NET_MTU;
    net->transmit = card_transmit;
    net->poll = card_poll;
    net->driver = card;
    cards[card_count++] = card;
    pci_claim(pci, newer ? "e1000e" : "e1000", NULL);
    device_set_details(pci->node, "%s, MAC %02x:%02x:%02x:%02x:%02x:%02x", net->name, net->mac[0],
                       net->mac[1], net->mac[2], net->mac[3], net->mac[4], net->mac[5]);
    kprintf("[e1000] %s: Intel %04x, link %s, %s\n", net->name, pci->device_id,
            card->link_up ? "up" : "down", interrupts ? "MSI interrupts" : "polling");
    net_register(net);
}

void e1000_init(void) {
    for (struct pci_device *pci = pci_first(); pci; pci = pci->next) {
        if (pci->vendor_id != INTEL_VENDOR || pci->class_code != 0x02) {
            continue;
        }
        if (listed(pci->device_id, e1000_ids, sizeof(e1000_ids) / sizeof(e1000_ids[0]))) {
            probe(pci, false);
        } else if (listed(pci->device_id, e1000e_ids, sizeof(e1000e_ids) / sizeof(e1000e_ids[0]))) {
            probe(pci, true);
        }
    }
}
