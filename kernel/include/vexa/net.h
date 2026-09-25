#ifndef VEXA_NET_H
#define VEXA_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>
#include <vexa/mutex.h>

/*
 * The network stack: Ethernet, ARP, IPv4, ICMP, UDP and TCP, over network
 * interfaces (network cards and the loopback interface).
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

#define ETH_ADDRESS 6
#define ETH_HEADER 14
#define IP_HEADER 20
#define NET_MTU 1500
#define FRAME_MAX (ETH_HEADER + NET_MTU)
/* Where a transport header starts in a frame being built. */
#define TRANSPORT_OFFSET (ETH_HEADER + IP_HEADER)

#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_TCP 6
#define IP_PROTOCOL_UDP 17

#define NET_FLAG_UP 0x1
#define NET_FLAG_LOOPBACK 0x2
#define NET_FLAG_DHCP 0x4 /* Configured by DHCP. */

struct net_interface {
    char name[8];
    uint8_t mac[ETH_ADDRESS];
    uint32_t flags; /* NET_FLAG_* */
    uint32_t mtu;
    ipv4_t address, netmask, gateway, dns;
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

/* ---- Protocols: called for received packets, with net_lock held ---- */

struct ip_packet {
    struct net_interface *net;
    ipv4_t source, destination;
    uint8_t protocol;
    const uint8_t *header;  /* The IP header... */
    const uint8_t *data;    /* ...and what follows it. */
    size_t header_length, length;
    bool broadcast;
};

void udp_input(const struct ip_packet *packet);
void tcp_input(const struct ip_packet *packet);
void raw_input(const struct ip_packet *packet); /* Every ICMP packet, for raw sockets. */
void dhcp_input(struct net_interface *net, const uint8_t *data, size_t length);
/* Timers, every few milliseconds from the network thread. */
void tcp_tick(uint64_t now);
void dhcp_tick(uint64_t now);
void dhcp_start(struct net_interface *net);

/* Sends an ICMP error (like "port unreachable") about a received packet. */
void icmp_send_unreachable(const struct ip_packet *packet, uint8_t code);

/* A copy of each interface's settings and counters, for vx_net_info and
 * /proc/net. Returns how many interfaces there are (fills at most `max`). */
struct vx_net_interface;
int net_snapshot(struct vx_net_interface *out, int max);

#endif
