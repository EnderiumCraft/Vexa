#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/sched.h>
#include <vexa/string.h>

/*
 * IPv6 (RFC 8200), with neighbour discovery (RFC 4861), stateless address
 * autoconfiguration (RFC 4862) and ICMPv6 (RFC 4443).
 *
 * Each card gets a link-local address (fe80:: and its MAC address made into
 * an EUI-64), then asks for routers; a router's advertisement gives it a
 * global address in each /64 prefix marked for autoconfiguration, its default
 * route, and a DNS server (RFC 8106). Duplicate address detection sends one
 * solicitation and waits a second.
 *
 * Kept simple: no fragments (received ones are dropped), no multicast
 * listener reports (switches that snoop MLD may not forward our solicited-node
 * multicast; most send it everywhere), one default router per card, and
 * link-local destinations go out of the first card (there are no zone ids).
 */

struct __attribute__((packed)) ip6_header {
    uint32_t version_class_flow; /* Version 6 in the top 4 bits. */
    uint16_t payload_length;
    uint8_t next_header;
    uint8_t hop_limit;
    ip6_t source;
    ip6_t destination;
};

#define ETH_TYPE_IP6 0x86dd

#define NEXT_HOP_BY_HOP 0
#define NEXT_ROUTING 43
#define NEXT_FRAGMENT 44
#define NEXT_NONE 59
#define NEXT_DESTINATION 60

#define ICMP6_UNREACHABLE 1
#define ICMP6_PARAMETER 4
#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY 129
#define ICMP6_ROUTER_SOLICIT 133
#define ICMP6_ROUTER_ADVERT 134
#define ICMP6_NEIGHBOR_SOLICIT 135
#define ICMP6_NEIGHBOR_ADVERT 136

#define OPTION_SOURCE_LINK 1
#define OPTION_TARGET_LINK 2
#define OPTION_PREFIX 3
#define OPTION_DNS 25

#define NA_ROUTER 0x80
#define NA_SOLICITED 0x40
#define NA_OVERRIDE 0x20

#define PREFIX_ON_LINK 0x80
#define PREFIX_AUTONOMOUS 0x40

#define DAD_MS 1000
#define SOLICIT_INTERVAL_MS 4000
#define SOLICITATIONS 3
#define HOP_LIMIT 64
#define ERROR_QUOTE_MAX (1280 - IP6_HEADER - 8) /* An error fits the minimum MTU. */

static const ip6_t all_nodes = {{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
static const ip6_t all_routers = {{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};

/* ---- Addresses ---- */

void ip6_format(ip6_t a, char *out, size_t size) {
    if (ip6_is_mapped(a)) {
        ksnprintf(out, size, "::ffff:%u.%u.%u.%u", a.b[12], a.b[13], a.b[14], a.b[15]);
        return;
    }
    uint16_t words[8];
    for (int i = 0; i < 8; i++) {
        words[i] = (uint16_t)(a.b[2 * i] << 8 | a.b[2 * i + 1]);
    }
    /* The longest run of zero words (two or more) becomes "::". */
    int best = -1, best_length = 1;
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && words[j] == 0) {
            j++;
        }
        if (j - i > best_length) {
            best = i;
            best_length = j - i;
        }
        i = j > i ? j : i + 1;
    }
    size_t n = 0;
    for (int i = 0; i < 8 && n < size; i++) {
        if (i == best) {
            n += (size_t)ksnprintf(out + n, size - n, "::");
            i += best_length - 1;
            continue;
        }
        bool colon = i > 0 && i != best + best_length;
        n += (size_t)ksnprintf(out + n, size - n, "%s%x", colon ? ":" : "", words[i]);
    }
    if (n == 0 && size) {
        out[0] = 0;
    }
}

static ip6_t solicited_node(ip6_t a) {
    ip6_t m = {{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff}};
    memcpy(&m.b[13], &a.b[13], 3);
    return m;
}

static void multicast_mac(ip6_t a, uint8_t *mac) {
    mac[0] = mac[1] = 0x33;
    memcpy(mac + 2, &a.b[12], 4);
}

/* The interface identifier from the MAC address (modified EUI-64). */
static void interface_id(const struct net_interface *net, uint8_t *id) {
    id[0] = net->mac[0] ^ 0x02;
    id[1] = net->mac[1];
    id[2] = net->mac[2];
    id[3] = 0xff;
    id[4] = 0xfe;
    id[5] = net->mac[3];
    id[6] = net->mac[4];
    id[7] = net->mac[5];
}

static bool same_prefix(ip6_t a, ip6_t b, int length) {
    int bytes = length / 8, bits = length % 8;
    if (memcmp(a.b, b.b, (size_t)bytes) != 0) {
        return false;
    }
    if (bits) {
        uint8_t mask = (uint8_t)(0xff << (8 - bits));
        return ((a.b[bytes] ^ b.b[bytes]) & mask) == 0;
    }
    return true;
}

static struct net_ip6_address *find_address(struct net_interface *net, ip6_t a) {
    for (int i = 0; i < NET_IP6_ADDRESSES; i++) {
        if (net->ip6[i].prefix_length && ip6_equal(net->ip6[i].address, a)) {
            return &net->ip6[i];
        }
    }
    return NULL;
}

static void log_address(struct net_interface *net, const char *what, ip6_t a, int length) {
    char text[48];
    ip6_format(a, text, sizeof(text));
    kprintf("[net] %s: IPv6 %s %s/%d\n", net->name, what, text, length);
}

static void forget_address(struct net_interface *net, struct net_ip6_address *slot) {
    memset(slot, 0, sizeof(*slot));
    (void)net;
}

static void send_dad(struct net_interface *net, ip6_t target);

/* Adds (or renews) an address; new ones start tentative. */
static void add_address(struct net_interface *net, ip6_t a, int prefix_length, uint64_t expires) {
    struct net_ip6_address *slot = find_address(net, a);
    if (slot) {
        slot->expires = expires;
        return;
    }
    for (int i = 0; i < NET_IP6_ADDRESSES && !slot; i++) {
        if (!net->ip6[i].prefix_length) {
            slot = &net->ip6[i];
        }
    }
    if (!slot) {
        return;
    }
    slot->address = a;
    slot->prefix_length = (uint8_t)prefix_length;
    slot->expires = expires;
    if (net->flags & NET_FLAG_LOOPBACK) {
        return;
    }
    slot->tentative = true;
    slot->ready_at = timer_ms() + DAD_MS;
    send_dad(net, a);
}

/* A usable (not tentative) address on `net`: link-local or not. */
static bool usable_address(struct net_interface *net, bool link_local, ip6_t *out) {
    for (int i = 0; i < NET_IP6_ADDRESSES; i++) {
        struct net_ip6_address *slot = &net->ip6[i];
        if (slot->prefix_length && !slot->tentative &&
            ip6_is_link_local(slot->address) == link_local) {
            *out = slot->address;
            return true;
        }
    }
    return false;
}

bool ip6_is_local(ip6_t address) {
    if (ip6_equal(address, IP6_LOOPBACK)) {
        return true;
    }
    for (struct net_interface *net = net_interfaces(); net; net = net->next) {
        struct net_ip6_address *slot = find_address(net, address);
        if (slot && !slot->tentative) {
            return true;
        }
    }
    return false;
}

/* ---- Routing ---- */

static bool router_alive(struct net_interface *net) {
    return !ip6_is_any(net->router6) &&
           (!net->router6_expires || timer_ms() < net->router6_expires);
}

static struct net_interface *route(ip6_t destination, ip6_t *hop) {
    *hop = destination;
    if (ip6_is_local(destination)) {
        return net_loopback();
    }
    struct net_interface *first = NULL;
    for (struct net_interface *net = net_interfaces(); net; net = net->next) {
        if (net->flags & NET_FLAG_LOOPBACK || !(net->flags & NET_FLAG_UP)) {
            continue;
        }
        if (!first) {
            first = net;
        }
        for (int i = 0; i < NET_IP6_ADDRESSES; i++) {
            struct net_ip6_address *slot = &net->ip6[i];
            if (slot->prefix_length && !ip6_is_link_local(slot->address) &&
                same_prefix(slot->address, destination, slot->prefix_length)) {
                return net; /* On the link. */
            }
        }
    }
    if (ip6_is_link_local(destination) || ip6_is_multicast(destination)) {
        return first;
    }
    for (struct net_interface *net = net_interfaces(); net; net = net->next) {
        if (!(net->flags & NET_FLAG_LOOPBACK) && (net->flags & NET_FLAG_UP) &&
            router_alive(net)) {
            *hop = net->router6;
            return net;
        }
    }
    return NULL;
}

bool ip6_source_for(ip6_t destination, ip6_t *source) {
    ip6_t hop;
    struct net_interface *net = route(destination, &hop);
    if (!net) {
        return false;
    }
    if (net->flags & NET_FLAG_LOOPBACK) {
        *source = destination;
        return true;
    }
    bool link_local = ip6_is_link_local(destination) ||
                      (ip6_is_multicast(destination) && (destination.b[1] & 0xf) <= 2);
    return usable_address(net, link_local, source); /* No global address: unreachable. */
}

uint16_t ip6_transport_checksum(ip6_t source, ip6_t destination, uint8_t protocol,
                                const void *data, size_t length) {
    /* The pseudo-header, summed in network byte order like the data. */
    uint32_t sum = net16((uint16_t)(length >> 16)) + net16((uint16_t)length) + net16(protocol);
    for (int i = 0; i < 16; i += 2) {
        sum += (uint32_t)(source.b[i] | source.b[i + 1] << 8);
        sum += (uint32_t)(destination.b[i] | destination.b[i + 1] << 8);
    }
    return net_checksum(data, length, sum);
}

/* Adds the IPv6 and Ethernet headers to the frame and sends it. */
static int send_on(struct net_interface *net, ip6_t hop, uint8_t *frame, ip6_t source,
                   ip6_t destination, uint8_t protocol, size_t length, uint8_t hop_limit) {
    if (IP6_HEADER + length > net->mtu) {
        kfree(frame);
        return -VX_EMSGSIZE;
    }
    struct ip6_header *ip = (struct ip6_header *)(frame + ETH_HEADER);
    ip->version_class_flow = net32(6U << 28);
    ip->payload_length = net16((uint16_t)length);
    ip->next_header = protocol;
    ip->hop_limit = hop_limit;
    ip->source = source;
    ip->destination = destination;
    uint8_t *eth = frame;
    memcpy(eth + ETH_ADDRESS, net->mac, ETH_ADDRESS);
    uint16_t type = net16(ETH_TYPE_IP6);
    memcpy(eth + 2 * ETH_ADDRESS, &type, 2);
    size_t frame_length = ETH_HEADER + IP6_HEADER + length;
    if (net->flags & NET_FLAG_LOOPBACK) {
        memset(eth, 0, ETH_ADDRESS);
    } else if (ip6_is_multicast(destination)) {
        multicast_mac(destination, eth);
    } else {
        net_send_to_neighbour(net, hop, frame, 0, frame_length);
        return 0;
    }
    net_transmit(net, frame, frame_length);
    kfree(frame);
    return 0;
}

int ip6_send(uint8_t *frame, ip6_t source, ip6_t destination, uint8_t protocol,
             size_t length) {
    ip6_t hop;
    struct net_interface *net = route(destination, &hop);
    if (!net || (ip6_is_any(source) && !ip6_source_for(destination, &source))) {
        kfree(frame);
        return -VX_ENETUNREACH;
    }
    return send_on(net, hop, frame, source, destination, protocol, length, HOP_LIMIT);
}

/* ---- ICMPv6 ---- */

/* Fills in the checksum of an ICMPv6 message and sends it. */
static void icmp6_send(struct net_interface *net, uint8_t *frame, ip6_t source,
                       ip6_t destination, size_t length, uint8_t hop_limit) {
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    icmp[2] = icmp[3] = 0;
    uint16_t sum = ip6_transport_checksum(source, destination, IP_PROTOCOL_ICMPV6, icmp, length);
    memcpy(icmp + 2, &sum, 2);
    if (net) {
        send_on(net, destination, frame, source, destination, IP_PROTOCOL_ICMPV6, length,
                hop_limit);
    } else {
        ip6_send(frame, source, destination, IP_PROTOCOL_ICMPV6, length);
    }
}

void ndp_solicit(struct net_interface *net, ip6_t target) {
    ip6_t source;
    if (!usable_address(net, true, &source) && !usable_address(net, false, &source)) {
        return; /* Still tentative; the retry will ask. */
    }
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    memset(icmp, 0, 32);
    icmp[0] = ICMP6_NEIGHBOR_SOLICIT;
    memcpy(icmp + 8, target.b, 16);
    icmp[24] = OPTION_SOURCE_LINK;
    icmp[25] = 1;
    memcpy(icmp + 26, net->mac, ETH_ADDRESS);
    icmp6_send(net, frame, source, solicited_node(target), 32, 255);
}

/* Duplicate address detection: who else has `target`? From ::. */
static void send_dad(struct net_interface *net, ip6_t target) {
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    memset(icmp, 0, 24);
    icmp[0] = ICMP6_NEIGHBOR_SOLICIT;
    memcpy(icmp + 8, target.b, 16);
    icmp6_send(net, frame, IP6_ANY, solicited_node(target), 24, 255);
}

static void send_advert(struct net_interface *net, ip6_t target, ip6_t destination,
                        uint8_t flags) {
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    memset(icmp, 0, 32);
    icmp[0] = ICMP6_NEIGHBOR_ADVERT;
    icmp[4] = flags;
    memcpy(icmp + 8, target.b, 16);
    icmp[24] = OPTION_TARGET_LINK;
    icmp[25] = 1;
    memcpy(icmp + 26, net->mac, ETH_ADDRESS);
    icmp6_send(net, frame, target, destination, 32, 255);
}

static void send_router_solicit(struct net_interface *net) {
    ip6_t source;
    if (!usable_address(net, true, &source)) {
        return;
    }
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    memset(icmp, 0, 16);
    icmp[0] = ICMP6_ROUTER_SOLICIT;
    icmp[8] = OPTION_SOURCE_LINK;
    icmp[9] = 1;
    memcpy(icmp + 10, net->mac, ETH_ADDRESS);
    icmp6_send(net, frame, source, all_routers, 16, 255);
}

void icmp6_send_error(const struct ip_packet *packet, uint8_t type, uint8_t code,
                      uint32_t pointer) {
    if (ip6_is_multicast(packet->destination) || ip6_is_any(packet->source)) {
        return;
    }
    if (packet->protocol == IP_PROTOCOL_ICMPV6 && packet->length > 0 && packet->data[0] < 128) {
        return; /* Never about an error. */
    }
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    uint8_t *icmp = frame + TRANSPORT_OFFSET;
    icmp[0] = type;
    icmp[1] = code;
    uint32_t p = net32(pointer);
    memcpy(icmp + 4, &p, 4);
    size_t quoted = packet->header_length + packet->length;
    if (quoted > ERROR_QUOTE_MAX) {
        quoted = ERROR_QUOTE_MAX;
    }
    size_t header = quoted < packet->header_length ? quoted : packet->header_length;
    memcpy(icmp + 8, packet->header, header);
    memcpy(icmp + 8 + header, packet->data, quoted - header);
    icmp6_send(NULL, frame, packet->destination, packet->source, 8 + quoted, HOP_LIMIT);
}

/* The next option of a neighbour discovery message (NULL at the end), and
 * its length in bytes (the type and length bytes included). */
static const uint8_t *next_option(const uint8_t **p, const uint8_t *end, size_t *length) {
    if (*p + 2 > end || (*p)[1] == 0 || *p + (*p)[1] * 8 > end) {
        return NULL;
    }
    const uint8_t *option = *p;
    *length = (size_t)option[1] * 8;
    *p += *length;
    return option;
}

static const uint8_t *link_option(const uint8_t *options, const uint8_t *end, uint8_t type) {
    size_t length;
    const uint8_t *o;
    while ((o = next_option(&options, end, &length))) {
        if (o[0] == type && length >= 8) {
            return o + 2;
        }
    }
    return NULL;
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return net32(v);
}

static uint64_t lifetime_end(uint32_t seconds) {
    return seconds == 0xffffffffU ? 0 : timer_ms() + (uint64_t)seconds * 1000;
}

static void router_advert(const struct ip_packet *packet) {
    struct net_interface *net = packet->net;
    const uint8_t *icmp = packet->data, *end = packet->data + packet->length;
    if (packet->length < 16 || !ip6_is_link_local(packet->source)) {
        return;
    }
    uint16_t router_lifetime = (uint16_t)(icmp[6] << 8 | icmp[7]);
    const uint8_t *mac = link_option(icmp + 16, end, OPTION_SOURCE_LINK);
    if (mac) {
        net_neighbour_learned(net, packet->source, mac, true);
    }
    if (router_lifetime) {
        if (ip6_is_any(net->router6)) {
            char text[48];
            ip6_format(packet->source, text, sizeof(text));
            kprintf("[net] %s: IPv6 router %s\n", net->name, text);
        }
        net->router6 = packet->source;
        net->router6_expires = timer_ms() + (uint64_t)router_lifetime * 1000;
    } else if (ip6_equal(net->router6, packet->source)) {
        net->router6 = IP6_ANY;
    }
    net->solicitations = SOLICITATIONS; /* Heard one: stop asking. */
    const uint8_t *p = icmp + 16, *o;
    size_t length;
    while ((o = next_option(&p, end, &length))) {
        if (o[0] == OPTION_PREFIX && length == 32) {
            uint8_t prefix_length = o[2], flags = o[3];
            uint32_t valid = get32(o + 4);
            ip6_t prefix;
            memcpy(prefix.b, o + 16, 16);
            if ((flags & PREFIX_AUTONOMOUS) && prefix_length == 64 && valid &&
                !ip6_is_link_local(prefix) && !ip6_is_multicast(prefix)) {
                ip6_t address = prefix;
                interface_id(net, &address.b[8]);
                add_address(net, address, 64, lifetime_end(valid));
            }
        } else if (o[0] == OPTION_DNS && length >= 24 && get32(o + 4)) {
            memcpy(net->dns6.b, o + 8, 16);
        }
    }
}

static void neighbor_solicit(const struct ip_packet *packet) {
    struct net_interface *net = packet->net;
    if (packet->length < 24) {
        return;
    }
    ip6_t target;
    memcpy(target.b, packet->data + 8, 16);
    struct net_ip6_address *slot = find_address(net, target);
    if (!slot) {
        return;
    }
    if (ip6_is_any(packet->source)) {
        /* Someone else's duplicate address detection. */
        if (slot->tentative) {
            log_address(net, "address in use elsewhere:", target, slot->prefix_length);
            forget_address(net, slot);
        } else {
            send_advert(net, target, all_nodes, NA_OVERRIDE);
        }
        return;
    }
    if (slot->tentative) {
        return;
    }
    const uint8_t *mac =
        link_option(packet->data + 24, packet->data + packet->length, OPTION_SOURCE_LINK);
    if (mac) {
        net_neighbour_learned(net, packet->source, mac, true);
    }
    send_advert(net, target, packet->source, NA_SOLICITED | NA_OVERRIDE);
}

static void neighbor_advert(const struct ip_packet *packet) {
    struct net_interface *net = packet->net;
    if (packet->length < 24) {
        return;
    }
    ip6_t target;
    memcpy(target.b, packet->data + 8, 16);
    struct net_ip6_address *slot = find_address(net, target);
    if (slot) {
        if (slot->tentative) {
            log_address(net, "address in use elsewhere:", target, slot->prefix_length);
            forget_address(net, slot);
        }
        return;
    }
    const uint8_t *mac =
        link_option(packet->data + 24, packet->data + packet->length, OPTION_TARGET_LINK);
    if (mac) {
        net_neighbour_learned(net, target, mac, false);
    }
}

static void icmp6_input(const struct ip_packet *packet, uint8_t hop_limit) {
    if (packet->length < 8 || ip6_transport_checksum(packet->source, packet->destination,
                                                     IP_PROTOCOL_ICMPV6, packet->data,
                                                     packet->length) != 0) {
        return;
    }
    uint8_t type = packet->data[0];
    if (type == ICMP6_ECHO_REQUEST) {
        if (packet->length > NET_MTU - IP6_HEADER) {
            return;
        }
        ip6_t source = packet->destination;
        if (ip6_is_multicast(source) && !ip6_source_for(packet->source, &source)) {
            return;
        }
        uint8_t *frame = net_frame();
        if (!frame) {
            return;
        }
        uint8_t *icmp = frame + TRANSPORT_OFFSET;
        memcpy(icmp, packet->data, packet->length);
        icmp[0] = ICMP6_ECHO_REPLY;
        icmp6_send(NULL, frame, source, packet->source, packet->length, HOP_LIMIT);
        return;
    }
    /* Neighbour discovery only from the link itself. */
    if (hop_limit != 255 || packet->data[1] != 0 || (packet->net->flags & NET_FLAG_LOOPBACK)) {
        return;
    }
    switch (type) {
    case ICMP6_ROUTER_ADVERT:
        router_advert(packet);
        break;
    case ICMP6_NEIGHBOR_SOLICIT:
        neighbor_solicit(packet);
        break;
    case ICMP6_NEIGHBOR_ADVERT:
        neighbor_advert(packet);
        break;
    }
}

/* ---- Receiving ---- */

static bool for_us(struct net_interface *net, ip6_t destination) {
    if (net->flags & NET_FLAG_LOOPBACK) {
        return true;
    }
    if (ip6_equal(destination, all_nodes)) {
        return true;
    }
    for (int i = 0; i < NET_IP6_ADDRESSES; i++) {
        struct net_ip6_address *slot = &net->ip6[i];
        if (!slot->prefix_length) {
            continue;
        }
        if (ip6_equal(destination, slot->address) ||
            ip6_equal(destination, solicited_node(slot->address))) {
            return true;
        }
    }
    return false;
}

void ip6_input(struct net_interface *net, const uint8_t *data, size_t length) {
    if (length < IP6_HEADER) {
        return;
    }
    const struct ip6_header *ip = (const struct ip6_header *)data;
    size_t payload = net16(ip->payload_length);
    if ((data[0] >> 4) != 6 || IP6_HEADER + payload > length ||
        !for_us(net, ip->destination)) {
        return;
    }
    size_t end = IP6_HEADER + payload, offset = IP6_HEADER;
    uint8_t next = ip->next_header;
    /* Skip the extension headers we can. */
    while (next == NEXT_HOP_BY_HOP || next == NEXT_DESTINATION || next == NEXT_ROUTING) {
        if (offset + 8 > end) {
            return;
        }
        size_t header_length = ((size_t)data[offset + 1] + 1) * 8;
        if (next == NEXT_ROUTING && data[offset + 3] != 0) {
            return; /* Segments left: we're no router. */
        }
        next = data[offset];
        offset += header_length;
        if (offset > end) {
            return;
        }
    }
    if (next == NEXT_FRAGMENT) {
        net->rx_dropped++; /* No reassembly. */
        return;
    }
    struct ip_packet packet = {
        .net = net,
        .version = 6,
        .source = ip->source,
        .destination = ip->destination,
        .protocol = next,
        .header = data,
        .header_length = offset,
        .data = data + offset,
        .length = end - offset,
        .broadcast = ip6_is_multicast(ip->destination),
    };
    switch (next) {
    case IP_PROTOCOL_ICMPV6:
        raw_input(&packet);
        icmp6_input(&packet, ip->hop_limit);
        break;
    case IP_PROTOCOL_UDP:
        udp_input(&packet);
        break;
    case IP_PROTOCOL_TCP:
        tcp_input(&packet);
        break;
    case NEXT_NONE:
        break;
    default:
        icmp_send_unreachable(&packet, 2);
    }
}

/* ---- Interfaces and timers ---- */

void ip6_start(struct net_interface *net) {
    if (net->flags & NET_FLAG_LOOPBACK) {
        add_address(net, IP6_LOOPBACK, 128, 0);
        return;
    }
    ip6_t link_local = {{0xfe, 0x80}};
    interface_id(net, &link_local.b[8]);
    add_address(net, link_local, 64, 0);
}

void ip6_tick(uint64_t now) {
    for (struct net_interface *net = net_interfaces(); net; net = net->next) {
        if (net->flags & NET_FLAG_LOOPBACK) {
            continue;
        }
        for (int i = 0; i < NET_IP6_ADDRESSES; i++) {
            struct net_ip6_address *slot = &net->ip6[i];
            if (!slot->prefix_length) {
                continue;
            }
            if (slot->tentative && now >= slot->ready_at) {
                slot->tentative = false;
                log_address(net, "address", slot->address, slot->prefix_length);
                if (ip6_is_link_local(slot->address)) {
                    net->solicit_at = now; /* Now ask for routers. */
                }
            } else if (slot->expires && now >= slot->expires) {
                log_address(net, "address expired:", slot->address, slot->prefix_length);
                forget_address(net, slot);
            }
        }
        if (net->solicit_at && now >= net->solicit_at) {
            if (net->solicitations < SOLICITATIONS) {
                net->solicitations++;
                send_router_solicit(net);
                net->solicit_at = now + SOLICIT_INTERVAL_MS;
            } else {
                net->solicit_at = 0;
            }
        }
        if (!ip6_is_any(net->router6) && net->router6_expires && now >= net->router6_expires) {
            net->router6 = IP6_ANY;
        }
    }
}
