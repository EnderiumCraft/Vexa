#ifndef VEXA_NET_H
#define VEXA_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>
#include <vexa/mutex.h>

/*
 * The network stack: Ethernet, ARP, IPv4, ICMP, IPv6 (with neighbour
 * discovery and address autoconfiguration), ICMPv6, UDP and TCP, over network
 * interfaces (network cards and the loopback interface).
 *
 * UDP, TCP and the sockets keep every address as 16 bytes (ip6_t): IPv6 ones
 * as they are, IPv4 ones mapped (::ffff:a.b.c.d), so one socket can talk to
 * both; net_send() and friends pick the IP version from the address.
 *
 * One lock, net_lock (a sleeping mutex), covers all of it. Received frames are
 * handled by the network thread, which also runs the timers (retransmissions,
 * DHCP); system calls take the lock for their part and never wait while
 * holding it.
 *
 * Addresses and ports are kept in network byte order, as on the wire.
 */

typedef uint32_t ipv4_t;

static inline uint16_t net16(uint16_t value) {
    return __builtin_bswap16(value);
}

static inline uint32_t net32(uint32_t value) {
    return __builtin_bswap32(value);
}

/* a.b.c.d in network byte order. */
#define IPV4(a, b, c, d) \
    ((ipv4_t)(a) | (ipv4_t)(b) << 8 | (ipv4_t)(c) << 16 | (ipv4_t)(d) << 24)
#define IPV4_ANY 0
#define IPV4_BROADCAST 0xffffffffU
#define IPV4_LOOPBACK IPV4(127, 0, 0, 1)

/* An IPv6 address, or an IPv4 one mapped into IPv6 (::ffff:a.b.c.d). */
typedef struct {
    uint8_t b[16];
} ip6_t;

static inline ip6_t ip6_mapped(ipv4_t v4) {
    ip6_t a = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}};
    __builtin_memcpy(&a.b[12], &v4, 4);
    return a;
}

static inline bool ip6_is_mapped(ip6_t a) {
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return __builtin_memcmp(a.b, prefix, 12) == 0;
}

static inline ipv4_t ip6_v4(ip6_t a) {
    ipv4_t v4;
    __builtin_memcpy(&v4, &a.b[12], 4);
    return v4;
}

static inline bool ip6_equal(ip6_t a, ip6_t b) {
    return __builtin_memcmp(a.b, b.b, 16) == 0;
}

/* :: (and, for IPv4, ::ffff:0.0.0.0): "any address". */
static inline bool ip6_is_any(ip6_t a) {
    static const uint8_t zero[16];
    return __builtin_memcmp(a.b, zero, 16) == 0 || (ip6_is_mapped(a) && ip6_v4(a) == 0);
}

static inline bool ip6_is_link_local(ip6_t a) {
    return a.b[0] == 0xfe && (a.b[1] & 0xc0) == 0x80;
}

static inline bool ip6_is_multicast(ip6_t a) {
    return a.b[0] == 0xff;
}

#define IP6_ANY ((ip6_t){{0}})
#define IP6_LOOPBACK ((ip6_t){{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}})

#define ETH_ADDRESS 6
#define ETH_HEADER 14
#define IP_HEADER 20
#define IP6_HEADER 40
#define NET_MTU 1500
/* Where a transport header starts in a frame being built: after the longer
 * (IPv6) header; IPv4 frames start 20 bytes into the buffer. */
#define TRANSPORT_OFFSET (ETH_HEADER + IP6_HEADER)
#define FRAME_MAX (TRANSPORT_OFFSET + NET_MTU)

#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_TCP 6
#define IP_PROTOCOL_UDP 17
#define IP_PROTOCOL_ICMPV6 58

#define NET_FLAG_UP 0x1
#define NET_FLAG_LOOPBACK 0x2
#define NET_FLAG_DHCP 0x4 /* Configured by DHCP. */

/* An IPv6 address of an interface. */
struct net_ip6_address {
    ip6_t address;
    uint8_t prefix_length;
    bool tentative;   /* Duplicate address detection is still running. */
    uint64_t ready_at; /* When it stops being tentative (timer_ms()). */
    uint64_t expires;  /* 0: never. */
};

#define NET_IP6_ADDRESSES 3 /* The link-local one, then autoconfigured ones. */

struct net_interface {
    char name[8];
    uint8_t mac[ETH_ADDRESS];
    uint32_t flags; /* NET_FLAG_* */
    uint32_t mtu;
    ipv4_t address, netmask, gateway, dns;
    /* IPv6: addresses (unused ones are all zero), the default router (a
     * link-local address; zero if none), and a DNS server from the router. */
    struct net_ip6_address ip6[NET_IP6_ADDRESSES];
    ip6_t router6, dns6;
    uint64_t router6_expires;
    int solicitations; /* Router solicitations sent. */
    uint64_t solicit_at;
    /* Sends one Ethernet frame. Called with net_lock held; must not sleep
     * (drop the frame if the card is busy). */
    void (*transmit)(struct net_interface *net, const void *frame, size_t length);
    /* Passes each received frame to net_receive(). The network thread calls
     * it with net_lock held. */
    void (*poll)(struct net_interface *net);
    void *driver;
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes, rx_dropped, tx_dropped;
    int index;
    struct net_interface *next;
};

extern struct mutex net_lock;

/* Starts the stack: the loopback interface and the network thread. */
void net_init(void);
/* Finds the network cards (dev/virtio_net.c). After net_init(). */
void virtio_net_init(void);
/* For drivers: adds an interface (and starts DHCP on it). */
void net_register(struct net_interface *net);
/* Names a network card: eth0, eth1... in the order they're found. */
void net_name(struct net_interface *net);
/* Card drivers (dev/e1000.c, dev/realtek.c). */
void e1000_init(void);
void realtek_init(void);
/* For drivers' interrupt handlers: there is work for the network thread. */
void net_wake(void);
struct net_interface *net_interfaces(void);
/* A received frame (with net_lock held). */
void net_receive(struct net_interface *net, const uint8_t *frame, size_t length);

/* ---- IPv4 (with net_lock held) ---- */

/* The interface that reaches `destination`, and the next hop on it; NULL if
 * none does. */
struct net_interface *ip_route(ipv4_t destination, ipv4_t *next_hop);
/* The address to send from when talking to `destination` (0 if unreachable). */
ipv4_t ip_source_for(ipv4_t destination);
/* True if `address` is one of ours (or loopback). */
bool ip_is_local(ipv4_t address);
/* Sends `frame` (kmalloc'ed, FRAME_MAX bytes, taken over), whose transport
 * part (`length` bytes at TRANSPORT_OFFSET) is filled in: adds the IP and
 * Ethernet headers and sends it, or queues it until ARP answers. Returns 0 or
 * a negative VX_E* error. `source` 0 means the interface's address. */
int ip_send(uint8_t *frame, ipv4_t source, ipv4_t destination, uint8_t protocol,
            size_t length);
/* The same, through a given interface and next hop (e.g. DHCP broadcasts). */
int ip_send_on(struct net_interface *net, ipv4_t hop, uint8_t *frame, ipv4_t source,
               ipv4_t destination, uint8_t protocol, size_t length);
/* The checksum of the transport header and data, with the IPv4 pseudo-header. */
uint16_t ip_transport_checksum(ipv4_t source, ipv4_t destination, uint8_t protocol,
                               const void *data, size_t length);
uint16_t net_checksum(const void *data, size_t length, uint32_t initial);
/* A new frame buffer for ip_send (NULL when out of memory). */
uint8_t *net_frame(void);

/* ---- Either IP version, by the address (with net_lock held) ---- */

/* Sends `frame` like ip_send(), by IPv4 or IPv6. `source` may be any (::). */
int net_send(uint8_t *frame, ip6_t source, ip6_t destination, uint8_t protocol, size_t length);
/* The address to send from to reach `destination`; returns false if it can't
 * be reached. */
bool net_source_for(ip6_t destination, ip6_t *source);
bool net_is_local(ip6_t address);
uint16_t net_transport_checksum(ip6_t source, ip6_t destination, uint8_t protocol,
                                const void *data, size_t length);
/* The most transport bytes one packet to `destination` can carry. */
size_t net_payload_max(ip6_t destination);

/* ---- IPv6 (ipv6.c, with net_lock held) ---- */

int ip6_send(uint8_t *frame, ip6_t source, ip6_t destination, uint8_t protocol,
             size_t length);
bool ip6_source_for(ip6_t destination, ip6_t *source);
bool ip6_is_local(ip6_t address);
uint16_t ip6_transport_checksum(ip6_t source, ip6_t destination, uint8_t protocol,
                                const void *data, size_t length);
/* An Ethernet frame of type 0x86dd. */
void ip6_input(struct net_interface *net, const uint8_t *data, size_t length);
/* A new interface: its link-local address, then router solicitations. */
void ip6_start(struct net_interface *net);
void ip6_tick(uint64_t now);
/* Writes `a` as text (2001:db8::1), for messages and /proc. */
void ip6_format(ip6_t a, char *out, size_t size);
/* For the loopback interface (core.c). */
struct net_interface *net_loopback(void);
/* Sends a whole Ethernet frame (counted in the interface's statistics). */
void net_transmit(struct net_interface *net, const void *frame, size_t length);
/* The neighbour table (core.c), for IPv4 and IPv6 next hops alike. Sends the
 * Ethernet frame at frame + start (kmalloc'ed `frame`, taken over) to `hop`,
 * filling in its destination, or keeps it until the address is resolved. */
void net_send_to_neighbour(struct net_interface *net, ip6_t hop, uint8_t *frame, size_t start,
                           size_t length);
/* `ip` is at `mac`: updates its entry (or makes one if `create`). */
void net_neighbour_learned(struct net_interface *net, ip6_t ip, const uint8_t *mac, bool create);
/* Asks who has `target` (a neighbour solicitation; ipv6.c). */
void ndp_solicit(struct net_interface *net, ip6_t target);

/* ---- Protocols: called for received packets, with net_lock held ---- */

struct ip_packet {
    struct net_interface *net;
    int version; /* 4 or 6 */
    ip6_t source, destination; /* IPv4 addresses mapped. */
    uint8_t protocol;
    const uint8_t *header;  /* The IP header... */
    const uint8_t *data;    /* ...and what follows it. */
    size_t header_length, length;
    bool broadcast;
};

void udp_input(const struct ip_packet *packet);
void tcp_input(const struct ip_packet *packet);
void raw_input(const struct ip_packet *packet); /* Every ICMP(v6) packet, for raw sockets. */
void dhcp_input(struct net_interface *net, const uint8_t *data, size_t length);
/* Timers, every few milliseconds from the network thread. */
void tcp_tick(uint64_t now);
void dhcp_tick(uint64_t now);
void dhcp_start(struct net_interface *net);

/* Sends an ICMP error about a received packet: IPv4's destination
 * unreachable `code` (2: protocol, 3: port), or the ICMPv6 equivalent. */
void icmp_send_unreachable(const struct ip_packet *packet, uint8_t code);
void icmp6_send_error(const struct ip_packet *packet, uint8_t type, uint8_t code,
                      uint32_t pointer);

/* A copy of each interface's settings and counters, for vx_net_info and
 * /proc/net. Returns how many interfaces there are (fills at most `max`). */
struct vx_net_interface;
int net_snapshot(struct vx_net_interface *out, int max);

#endif
