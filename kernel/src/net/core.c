#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/sched.h>
#include <vexa/string.h>

/*
 * The core of the network stack: interfaces (and the loopback one), the
 * network thread, Ethernet, ARP, IPv4 and ICMP.
 *
 * IPv4 is kept simple: no fragments (received ones are dropped, sent packets
 * must fit the MTU), no options, and routing is "the interface's own subnet,
 * else its gateway", with the first configured interface as the default.
 */

struct mutex net_lock = MUTEX_INIT;

static struct net_interface *interfaces;
static int interface_count;
static struct wait_queue net_thread_queue = WAIT_QUEUE_INIT;
static volatile bool work_pending;

#define ETH_TYPE_IP 0x0800
#define ETH_TYPE_ARP 0x0806

struct __attribute__((packed)) eth_header {
    uint8_t destination[ETH_ADDRESS];
    uint8_t source[ETH_ADDRESS];
    uint16_t type;
};

struct __attribute__((packed)) ip_header {
    uint8_t version_length; /* 0x45: version 4, 5 32-bit words. */
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t fragment; /* Flags and fragment offset. */
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    ipv4_t source;
    ipv4_t destination;
};

#define IP_DONT_FRAGMENT 0x4000
#define IP_MORE_FRAGMENTS 0x2000
#define IP_OFFSET_MASK 0x1fff

static const uint8_t broadcast_mac[ETH_ADDRESS] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* ---- Checksums ---- */

uint16_t net_checksum(const void *data, size_t length, uint32_t initial) {
    uint64_t sum = initial;
    const uint8_t *p = data;
    while (length >= 2) {
        sum += (uint16_t)(p[0] | p[1] << 8);
        p += 2;
        length -= 2;
    }
    if (length) {
        sum += p[0];
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint16_t ip_transport_checksum(ipv4_t source, ipv4_t destination, uint8_t protocol,
                               const void *data, size_t length) {
    /* The pseudo-header, summed in network byte order like the data. */
    uint32_t sum = (source & 0xffff) + (source >> 16) + (destination & 0xffff) +
                   (destination >> 16) + net16(protocol) + net16((uint16_t)length);
    return net_checksum(data, length, sum);
}

uint8_t *net_frame(void) {
    return kmalloc(FRAME_MAX);
}

/* ---- Interfaces ---- */

struct net_interface *net_interfaces(void) {
    return interfaces;
}

void net_wake(void) {
    work_pending = true;
    wait_queue_wake_all(&net_thread_queue);
}

void net_register(struct net_interface *net) {
    mutex_lock(&net_lock);
    net->index = ++interface_count;
    if (!net->mtu) {
        net->mtu = NET_MTU;
    }
    struct net_interface **p = &interfaces;
    while (*p) {
        p = &(*p)->next;
    }
    *p = net;
    net->flags |= NET_FLAG_UP;
    if (!(net->flags & NET_FLAG_LOOPBACK)) {
        kprintf("[net] %s: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", net->name, net->mac[0],
                net->mac[1], net->mac[2], net->mac[3], net->mac[4], net->mac[5]);
        dhcp_start(net);
    }
    mutex_unlock(&net_lock);
    net_wake();
}

int net_snapshot(struct vx_net_interface *out, int max) {
    mutex_lock(&net_lock);
    int count = 0;
    for (struct net_interface *net = interfaces; net; net = net->next, count++) {
        if (count >= max) {
            continue;
        }
        struct vx_net_interface *o = &out[count];
        memset(o, 0, sizeof(*o));
        memcpy(o->name, net->name, sizeof(net->name));
        memcpy(o->mac, net->mac, ETH_ADDRESS);
        o->flags = net->flags;
        o->mtu = net->mtu;
        o->address = net->address;
        o->netmask = net->netmask;
        o->gateway = net->gateway;
        o->dns = net->dns;
        o->rx_packets = net->rx_packets;
        o->rx_bytes = net->rx_bytes;
        o->tx_packets = net->tx_packets;
        o->tx_bytes = net->tx_bytes;
        o->rx_dropped = net->rx_dropped;
        o->tx_dropped = net->tx_dropped;
    }
    mutex_unlock(&net_lock);
    return count;
}

static void transmit(struct net_interface *net, const void *frame, size_t length) {
    net->tx_packets++;
    net->tx_bytes += length;
    net->transmit(net, frame, length);
}

/* ---- Loopback: what is sent comes back in on the next round ---- */

struct loop_frame {
    struct loop_frame *next;
    size_t length;
    uint8_t data[];
};

#define LOOPBACK_QUEUE_MAX 512

static struct loop_frame *loop_head, *loop_tail;
static int loop_queued;

static void loop_transmit(struct net_interface *net, const void *frame, size_t length) {
    struct loop_frame *copy = loop_queued < LOOPBACK_QUEUE_MAX ? kmalloc(sizeof(*copy) + length)
                                                               : NULL;
    if (!copy) {
        net->tx_dropped++;
        return;
    }
    copy->next = NULL;
    copy->length = length;
    memcpy(copy->data, frame, length);
    if (loop_tail) {
        loop_tail->next = copy;
    } else {
        loop_head = copy;
    }
    loop_tail = copy;
    loop_queued++;
    net_wake();
}

static void loop_poll(struct net_interface *net) {
    /* Only what was queued before this round: replies wait for the next. */
    int count = loop_queued;
    while (count-- > 0 && loop_head) {
        struct loop_frame *frame = loop_head;
        loop_head = frame->next;
        if (!loop_head) {
            loop_tail = NULL;
        }
        loop_queued--;
        net_receive(net, frame->data, frame->length);
        kfree(frame);
    }
}

static struct net_interface loopback = {
    .name = "lo",
    .flags = NET_FLAG_LOOPBACK,
    .mtu = NET_MTU,
    .address = IPV4(127, 0, 0, 1),
    .netmask = IPV4(255, 0, 0, 0),
    .transmit = loop_transmit,
    .poll = loop_poll,
};

/* ---- ARP ---- */

struct __attribute__((packed)) arp_packet {
    uint16_t hardware_type;  /* 1: Ethernet */
    uint16_t protocol_type;  /* 0x0800: IPv4 */
    uint8_t hardware_length; /* 6 */
    uint8_t protocol_length; /* 4 */
    uint16_t operation;      /* 1: request, 2: reply */
    uint8_t sender_mac[ETH_ADDRESS];
    ipv4_t sender_ip;
    uint8_t target_mac[ETH_ADDRESS];
    ipv4_t target_ip;
};

#define ARP_REQUEST 1
#define ARP_REPLY 2
#define ARP_ENTRIES 64
#define ARP_LIFETIME_MS (5 * 60 * 1000)
#define ARP_RETRY_MS 1000
#define ARP_PENDING_MAX 8 /* Packets waiting for one address. */

struct arp_entry {
    struct net_interface *net;
    ipv4_t ip;
    uint8_t mac[ETH_ADDRESS];
    bool resolved;
    uint64_t updated;   /* When learned, or when the last request went out. */
    int tries;
    struct pending_frame *pending;
};

struct pending_frame {
    struct pending_frame *next;
    uint8_t *frame;
    size_t length;
};

static struct arp_entry arp_table[ARP_ENTRIES];

static void free_pending(struct arp_entry *entry) {
    while (entry->pending) {
        struct pending_frame *p = entry->pending;
        entry->pending = p->next;
        entry->net->tx_dropped++;
        kfree(p->frame);
        kfree(p);
    }
}

static struct arp_entry *arp_find(struct net_interface *net, ipv4_t ip) {
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_table[i].net == net && arp_table[i].ip == ip) {
            return &arp_table[i];
        }
    }
    return NULL;
}

static struct arp_entry *arp_new(struct net_interface *net, ipv4_t ip) {
    struct arp_entry *victim = &arp_table[0];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!arp_table[i].net) {
            victim = &arp_table[i];
            break;
        }
        if (arp_table[i].updated < victim->updated) {
            victim = &arp_table[i];
        }
    }
    if (victim->net) {
        free_pending(victim);
    }
    memset(victim, 0, sizeof(*victim));
    victim->net = net;
    victim->ip = ip;
    return victim;
}

static void arp_send(struct net_interface *net, uint16_t operation, const uint8_t *target_mac,
                     ipv4_t target_ip) {
    uint8_t frame[ETH_HEADER + sizeof(struct arp_packet)];
    struct eth_header *eth = (struct eth_header *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HEADER);
    memcpy(eth->destination, operation == ARP_REQUEST ? broadcast_mac : target_mac, ETH_ADDRESS);
    memcpy(eth->source, net->mac, ETH_ADDRESS);
    eth->type = net16(ETH_TYPE_ARP);
    arp->hardware_type = net16(1);
    arp->protocol_type = net16(ETH_TYPE_IP);
    arp->hardware_length = ETH_ADDRESS;
    arp->protocol_length = 4;
    arp->operation = net16(operation);
    memcpy(arp->sender_mac, net->mac, ETH_ADDRESS);
    arp->sender_ip = net->address;
    memset(arp->target_mac, 0, ETH_ADDRESS);
    if (operation == ARP_REPLY) {
        memcpy(arp->target_mac, target_mac, ETH_ADDRESS);
    }
    arp->target_ip = target_ip;
    transmit(net, frame, sizeof(frame));
}

static void arp_learned(struct arp_entry *entry, const uint8_t *mac) {
    memcpy(entry->mac, mac, ETH_ADDRESS);
    entry->resolved = true;
    entry->updated = timer_ms();
    entry->tries = 0;
    /* Send what was waiting for this address. */
    struct pending_frame *p = entry->pending;
    entry->pending = NULL;
    while (p) {
        struct pending_frame *next = p->next;
        memcpy(((struct eth_header *)p->frame)->destination, mac, ETH_ADDRESS);
        transmit(entry->net, p->frame, p->length);
        kfree(p->frame);
        kfree(p);
        p = next;
    }
}

static void arp_input(struct net_interface *net, const uint8_t *data, size_t length) {
    if (length < sizeof(struct arp_packet)) {
        return;
    }
    const struct arp_packet *arp = (const struct arp_packet *)data;
    if (arp->hardware_type != net16(1) || arp->protocol_type != net16(ETH_TYPE_IP)) {
        return;
    }
    bool for_us = net->address && arp->target_ip == net->address;
    struct arp_entry *entry = arp_find(net, arp->sender_ip);
    if (entry) {
        arp_learned(entry, arp->sender_mac);
    } else if (for_us && arp->sender_ip) {
        arp_learned(arp_new(net, arp->sender_ip), arp->sender_mac);
    }
    if (for_us && arp->operation == net16(ARP_REQUEST)) {
        arp_send(net, ARP_REPLY, arp->sender_mac, arp->sender_ip);
    }
}

static void arp_tick(uint64_t now) {
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *entry = &arp_table[i];
        if (!entry->net) {
            continue;
        }
        if (entry->resolved && now - entry->updated > ARP_LIFETIME_MS) {
            entry->net = NULL; /* Forget it; the next packet asks again. */
        } else if (!entry->resolved && now - entry->updated >= ARP_RETRY_MS) {
            if (++entry->tries > 3) {
                free_pending(entry); /* Nobody answers. */
                entry->net = NULL;
            } else {
                entry->updated = now;
                arp_send(entry->net, ARP_REQUEST, NULL, entry->ip);
            }
        }
    }
}

/* ---- IPv4 ---- */

static uint16_t next_id;

bool ip_is_local(ipv4_t address) {
    if ((address & 0xff) == 127) {
        return true;
    }
    for (struct net_interface *net = interfaces; net; net = net->next) {
        if (net->address && net->address == address) {
            return true;
        }
    }
    return false;
}

struct net_interface *ip_route(ipv4_t destination, ipv4_t *next_hop) {
    *next_hop = destination;
    if (ip_is_local(destination)) {
        return &loopback;
    }
    struct net_interface *fallback = NULL;
    for (struct net_interface *net = interfaces; net; net = net->next) {
        if (net->flags & NET_FLAG_LOOPBACK || !(net->flags & NET_FLAG_UP)) {
            continue;
        }
        if (destination == IPV4_BROADCAST) {
            return net; /* DHCP: the first card. */
        }
        if (net->address && ((destination ^ net->address) & net->netmask) == 0) {
            return net;
        }
        if (!fallback && net->address && net->gateway) {
            fallback = net;
        }
    }
    if (fallback) {
        *next_hop = fallback->gateway;
    }
    return fallback;
}

ipv4_t ip_source_for(ipv4_t destination) {
    ipv4_t hop;
    struct net_interface *net = ip_route(destination, &hop);
    if (!net) {
        return 0;
    }
    return net == &loopback && destination != IPV4_LOOPBACK && ip_is_local(destination)
               ? destination
               : net->address;
}

int ip_send(uint8_t *frame, ipv4_t source, ipv4_t destination, uint8_t protocol,
            size_t length) {
    ipv4_t hop;
    struct net_interface *net = ip_route(destination, &hop);
    if (!net) {
        kfree(frame);
        return -VX_ENETUNREACH;
    }
    return ip_send_on(net, hop, frame, source, destination, protocol, length);
}

int ip_send_on(struct net_interface *net, ipv4_t hop, uint8_t *frame, ipv4_t source,
               ipv4_t destination, uint8_t protocol, size_t length) {
    if (IP_HEADER + length > net->mtu) {
        kfree(frame);
        return -VX_EMSGSIZE;
    }
    struct ip_header *ip = (struct ip_header *)(frame + ETH_HEADER);
    ip->version_length = 0x45;
    ip->tos = 0;
    ip->total_length = net16((uint16_t)(IP_HEADER + length));
    ip->id = net16(next_id++);
    ip->fragment = net16(IP_DONT_FRAGMENT);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->source = source ? source : net->address;
    ip->destination = destination;
    ip->checksum = net_checksum(ip, IP_HEADER, 0);

    struct eth_header *eth = (struct eth_header *)frame;
    memcpy(eth->source, net->mac, ETH_ADDRESS);
    eth->type = net16(ETH_TYPE_IP);
    size_t frame_length = ETH_HEADER + IP_HEADER + length;

    bool subnet_broadcast = net->netmask && (destination | net->netmask) == IPV4_BROADCAST &&
                            ((destination ^ net->address) & net->netmask) == 0;
    if (net->flags & NET_FLAG_LOOPBACK) {
        memset(eth->destination, 0, ETH_ADDRESS);
    } else if (destination == IPV4_BROADCAST || subnet_broadcast) {
        memcpy(eth->destination, broadcast_mac, ETH_ADDRESS);
    } else {
        struct arp_entry *entry = arp_find(net, hop);
        if (!entry || !entry->resolved) {
            /* Ask, and keep the packet until the answer comes. */
            if (!entry) {
                entry = arp_new(net, hop);
                entry->updated = timer_ms();
                arp_send(net, ARP_REQUEST, NULL, hop);
            }
            int waiting = 0;
            struct pending_frame **p = &entry->pending;
            while (*p) {
                p = &(*p)->next;
                waiting++;
            }
            struct pending_frame *pending = waiting < ARP_PENDING_MAX ? kmalloc(sizeof(*pending))
                                                                      : NULL;
            if (!pending) {
                net->tx_dropped++;
                kfree(frame);
                return 0; /* Lost, like on a busy wire; TCP sends it again. */
            }
            pending->next = NULL;
            pending->frame = frame;
            pending->length = frame_length;
            *p = pending;
            return 0;
        }
        memcpy(eth->destination, entry->mac, ETH_ADDRESS);
    }
    transmit(net, frame, frame_length);
    kfree(frame);
    return 0;
}

/* ---- ICMP ---- */

#define ICMP_ECHO_REPLY 0
#define ICMP_UNREACHABLE 3
#define ICMP_ECHO_REQUEST 8

static void icmp_input(const struct ip_packet *packet) {
    if (packet->length < 8 || net_checksum(packet->data, packet->length, 0) != 0) {
        return;
    }
    if (packet->data[0] == ICMP_ECHO_REQUEST && !packet->broadcast &&
        packet->length <= NET_MTU - IP_HEADER) {
        uint8_t *frame = net_frame();
        if (!frame) {
            return;
        }
        uint8_t *icmp = frame + TRANSPORT_OFFSET;
        memcpy(icmp, packet->data, packet->length);
        icmp[0] = ICMP_ECHO_REPLY;
        icmp[2] = icmp[3] = 0;
        uint16_t sum = net_checksum(icmp, packet->length, 0);
        memcpy(icmp + 2, &sum, 2);
        ip_send(frame, packet->destination, packet->source, IP_PROTOCOL_ICMP, packet->length);
    }
}

void icmp_send_unreachable(const struct ip_packet *packet, uint8_t code) {
    if (packet->broadcast) {
        return;
    }
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    /* The type, code, checksum and 4 unused bytes; then the start of the
     * packet that couldn't be delivered. */
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    size_t quoted = packet->length < 8 ? packet->length : 8;
    memset(icmp, 0, 8);
    icmp[0] = ICMP_UNREACHABLE;
    icmp[1] = code;
    memcpy(icmp + 8, packet->header, packet->header_length);
    memcpy(icmp + 8 + packet->header_length, packet->data, quoted);
    size_t length = 8 + packet->header_length + quoted;
    uint16_t sum = net_checksum(icmp, length, 0);
    memcpy(icmp + 2, &sum, 2);
    ip_send(frame, packet->destination, packet->source, IP_PROTOCOL_ICMP, length);
}

static void ip_input(struct net_interface *net, const uint8_t *data, size_t length) {
    if (length < IP_HEADER) {
        return;
    }
    const struct ip_header *ip = (const struct ip_header *)data;
    size_t header_length = (size_t)(ip->version_length & 0xf) * 4;
    size_t total = net16(ip->total_length);
    if ((ip->version_length >> 4) != 4 || header_length < IP_HEADER || total < header_length ||
        total > length || net_checksum(ip, header_length, 0) != 0) {
        return;
    }
    if (net16(ip->fragment) & (IP_MORE_FRAGMENTS | IP_OFFSET_MASK)) {
        net->rx_dropped++; /* No reassembly. */
        return;
    }
    bool broadcast = ip->destination == IPV4_BROADCAST ||
                     (net->netmask && (ip->destination | net->netmask) == IPV4_BROADCAST);
    bool ours = (net->address && ip->destination == net->address) || broadcast ||
                (net->flags & NET_FLAG_LOOPBACK) ||
                (!net->address && ip->protocol == IP_PROTOCOL_UDP); /* DHCP */
    if (!ours) {
        return;
    }
    struct ip_packet packet = {
        .net = net,
        .source = ip->source,
        .destination = ip->destination,
        .protocol = ip->protocol,
        .header = data,
        .header_length = header_length,
        .data = data + header_length,
        .length = total - header_length,
        .broadcast = broadcast,
    };
    switch (ip->protocol) {
    case IP_PROTOCOL_ICMP:
        raw_input(&packet);
        icmp_input(&packet);
        break;
    case IP_PROTOCOL_UDP:
        udp_input(&packet);
        break;
    case IP_PROTOCOL_TCP:
        tcp_input(&packet);
        break;
    default:
        icmp_send_unreachable(&packet, 2); /* Protocol unreachable. */
    }
}

void net_receive(struct net_interface *net, const uint8_t *frame, size_t length) {
    if (length < ETH_HEADER) {
        return;
    }
    net->rx_packets++;
    net->rx_bytes += length;
    const struct eth_header *eth = (const struct eth_header *)frame;
    switch (net16(eth->type)) {
    case ETH_TYPE_ARP:
        arp_input(net, frame + ETH_HEADER, length - ETH_HEADER);
        break;
    case ETH_TYPE_IP:
        ip_input(net, frame + ETH_HEADER, length - ETH_HEADER);
        break;
    }
}

/* ---- The network thread ---- */

#define TICK_MS 10

static bool has_work(void *arg) {
    (void)arg;
    return work_pending;
}

static void net_thread(void *arg) {
    (void)arg;
    uint64_t last_tick = 0;
    for (;;) {
        wait_queue_wait_timeout(&net_thread_queue, has_work, NULL, TICK_MS, false);
        mutex_lock(&net_lock);
        work_pending = false;
        for (struct net_interface *net = interfaces; net; net = net->next) {
            net->poll(net);
        }
        uint64_t now = timer_ms();
        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            arp_tick(now);
            tcp_tick(now);
            dhcp_tick(now);
        }
        mutex_unlock(&net_lock);
    }
}

void net_init(void) {
    net_register(&loopback);
    thread_create("network", net_thread, NULL);
}
