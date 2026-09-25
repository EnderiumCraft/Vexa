#include <vexa/abi.h>
#include <vexa/mm.h>
#include <vexa/random.h>
#include <vexa/string.h>

#include "inet.h"

/*
 * IPv4 sockets: the common part (addresses, ports), UDP, and raw ICMP sockets
 * (what ping uses). TCP is in tcp.c.
 */

#define DATAGRAM_QUEUE_MAX (256 * 1024) /* Bytes waiting in one socket. */
#define UDP_HEADER 8

int inet_parse_address(const struct vx_socket_address *address, size_t length, ipv4_t *ip,
                       uint16_t *port) {
    if (!address || length < 8) {
        return -VX_EINVAL;
    }
    if (address->family != VX_AF_INET) {
        return -VX_EAFNOSUPPORT;
    }
    *ip = address->inet.address;
    *port = address->inet.port;
    return 0;
}

void inet_make_address(struct vx_socket_address *address, size_t *length, ipv4_t ip,
                       uint16_t port) {
    memset(&address->inet, 0, sizeof(address->inet));
    address->inet.family = VX_AF_INET;
    address->inet.port = port;
    address->inet.address = ip;
    *length = sizeof(struct vx_inet_address);
}

#define EPHEMERAL_FIRST 49152
#define EPHEMERAL_COUNT 16384

uint16_t inet_ephemeral_port(bool (*in_use)(ipv4_t ip, uint16_t port, bool reuse)) {
    uint16_t start;
    random_bytes(&start, sizeof(start));
    for (uint32_t i = 0; i < EPHEMERAL_COUNT; i++) {
        uint16_t port = net16((uint16_t)(EPHEMERAL_FIRST + (start + i) % EPHEMERAL_COUNT));
        if (!in_use(IPV4_ANY, port, false)) {
            return port;
        }
    }
    return 0;
}

/* ---- UDP and raw sockets ---- */

struct datagram {
    struct datagram *next;
    ipv4_t from;
    uint16_t from_port;
    size_t length;
    uint8_t data[];
};

struct dgram_socket {
    struct dgram_socket *next;
    struct socket *socket;
    bool raw;
    ipv4_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    bool bound, connected, shut_read, shut_write;
    struct datagram *head, *tail;
    size_t queued;
    struct wait_queue wait;
};

static struct dgram_socket *udp_sockets, *raw_sockets;

static struct dgram_socket *dgram_of(struct socket *socket) {
    return socket->data;
}

static bool udp_port_in_use(ipv4_t ip, uint16_t port, bool reuse) {
    for (struct dgram_socket *d = udp_sockets; d; d = d->next) {
        if (d->bound && d->local_port == port &&
            (!ip || !d->local_ip || d->local_ip == ip) && !(reuse && d->socket->reuse_address)) {
            return true;
        }
    }
    return false;
}

/* Gives an unbound socket a port. */
static int autobind(struct dgram_socket *d) {
    if (d->bound) {
        return 0;
    }
    if (!d->raw) {
        d->local_port = inet_ephemeral_port(udp_port_in_use);
        if (!d->local_port) {
            return -VX_EADDRINUSE;
        }
    }
    d->bound = true;
    return 0;
}

static void enqueue(struct dgram_socket *d, ipv4_t from, uint16_t from_port, const void *data,
                    size_t length) {
    if (d->shut_read || d->queued + length > DATAGRAM_QUEUE_MAX) {
        return;
    }
    struct datagram *datagram = kmalloc(sizeof(*datagram) + length);
    if (!datagram) {
        return;
    }
    datagram->next = NULL;
    datagram->from = from;
    datagram->from_port = from_port;
    datagram->length = length;
    memcpy(datagram->data, data, length);
    if (d->tail) {
        d->tail->next = datagram;
    } else {
        d->head = datagram;
    }
    d->tail = datagram;
    d->queued += length;
    wait_queue_wake_all(&d->wait);
}

void udp_input(const struct ip_packet *packet) {
    if (packet->length < UDP_HEADER) {
        return;
    }
    const uint8_t *udp = packet->data;
    uint16_t source_port, destination_port, length, checksum;
    memcpy(&source_port, udp, 2);
    memcpy(&destination_port, udp + 2, 2);
    memcpy(&length, udp + 4, 2);
    memcpy(&checksum, udp + 6, 2);
    length = net16(length);
    if (length < UDP_HEADER || length > packet->length) {
        return;
    }
    if (checksum && ip_transport_checksum(packet->source, packet->destination, IP_PROTOCOL_UDP,
                                          udp, length) != 0) {
        return;
    }
    if (destination_port == net16(68) && !(packet->net->flags & NET_FLAG_LOOPBACK)) {
        dhcp_input(packet->net, udp + UDP_HEADER, length - UDP_HEADER);
        return;
    }
    if (!packet->net->address && !(packet->net->flags & NET_FLAG_LOOPBACK)) {
        return; /* Not configured yet: only DHCP. */
    }
    /* A connected socket that matches exactly, else a bound one. */
    struct dgram_socket *best = NULL;
    for (struct dgram_socket *d = udp_sockets; d; d = d->next) {
        if (!d->bound || d->local_port != destination_port ||
            (d->local_ip && d->local_ip != packet->destination && !packet->broadcast)) {
            continue;
        }
        if (d->connected) {
            if (d->remote_ip == packet->source && d->remote_port == source_port) {
                best = d;
                break;
            }
        } else if (!best) {
            best = d;
        }
    }
    if (!best) {
        icmp_send_unreachable(packet, 3); /* Port unreachable. */
        return;
    }
    enqueue(best, packet->source, source_port, udp + UDP_HEADER, length - UDP_HEADER);
}

void raw_input(const struct ip_packet *packet) {
    for (struct dgram_socket *d = raw_sockets; d; d = d->next) {
        if (d->connected && d->remote_ip != packet->source) {
            continue;
        }
        /* Raw sockets get the IP header too. */
        size_t length = packet->header_length + packet->length;
        uint8_t *copy = kmalloc(length);
        if (!copy) {
            return;
        }
        memcpy(copy, packet->header, packet->header_length);
        memcpy(copy + packet->header_length, packet->data, packet->length);
        enqueue(d, packet->source, 0, copy, length);
        kfree(copy);
    }
}

static int dgram_bind(struct socket *socket, const struct vx_socket_address *address,
                      size_t length) {
    ipv4_t ip;
    uint16_t port;
    int error = inet_parse_address(address, length, &ip, &port);
    if (error) {
        return error;
    }
    mutex_lock(&net_lock);
    struct dgram_socket *d = dgram_of(socket);
    if (d->bound) {
        error = -VX_EINVAL;
    } else if (ip && ip != IPV4_BROADCAST && !ip_is_local(ip)) {
        error = -VX_EADDRNOTAVAIL;
    } else if (!d->raw && port && udp_port_in_use(ip, port, socket->reuse_address)) {
        error = -VX_EADDRINUSE;
    } else {
        d->local_ip = ip == IPV4_BROADCAST ? 0 : ip;
        d->local_port = port;
        error = port || d->raw ? 0 : autobind(d);
        d->bound = true;
    }
    mutex_unlock(&net_lock);
    return error;
}

static int dgram_connect(struct socket *socket, const struct vx_socket_address *address,
                         size_t length) {
    struct dgram_socket *d = dgram_of(socket);
    if (address && length >= 2 && address->family == 0) { /* AF_UNSPEC: disconnect. */
        mutex_lock(&net_lock);
        d->connected = false;
        mutex_unlock(&net_lock);
        return 0;
    }
    ipv4_t ip;
    uint16_t port;
    int error = inet_parse_address(address, length, &ip, &port);
    if (error) {
        return error;
    }
    mutex_lock(&net_lock);
    error = ip_source_for(ip) ? autobind(d) : -VX_ENETUNREACH;
    if (!error) {
        d->remote_ip = ip;
        d->remote_port = port;
        d->connected = true;
    }
    mutex_unlock(&net_lock);
    return error;
}

static int64_t dgram_send(struct socket *socket, struct socket_message *message) {
    struct dgram_socket *d = dgram_of(socket);
    ipv4_t ip = 0;
    uint16_t port = 0;
    if (message->address) {
        int error = inet_parse_address(message->address, message->address_length, &ip, &port);
        if (error) {
            return error;
        }
    }
    mutex_lock(&net_lock);
    int64_t result;
    if (d->shut_write) {
        result = -VX_EPIPE;
        goto out;
    }
    if (!message->address) {
        if (!d->connected) {
            result = -VX_EDESTADDRREQ;
            goto out;
        }
        ip = d->remote_ip;
        port = d->remote_port;
    }
    size_t header = d->raw ? 0 : UDP_HEADER;
    if (header + message->size > NET_MTU - IP_HEADER) {
        result = -VX_EMSGSIZE;
        goto out;
    }
    if ((result = autobind(d)) != 0) {
        goto out;
    }
    ipv4_t source = d->local_ip ? d->local_ip : ip_source_for(ip);
    if (!source && ip != IPV4_BROADCAST) {
        result = -VX_ENETUNREACH;
        goto out;
    }
    uint8_t *frame = net_frame();
    if (!frame) {
        result = -VX_ENOMEM;
        goto out;
    }
    uint8_t *p = frame + TRANSPORT_OFFSET;
    memcpy(p + header, message->data, message->size);
    size_t length = header + message->size;
    if (!d->raw) {
        uint16_t udp_length = net16((uint16_t)length), zero = 0;
        memcpy(p, &d->local_port, 2);
        memcpy(p + 2, &port, 2);
        memcpy(p + 4, &udp_length, 2);
        memcpy(p + 6, &zero, 2);
        uint16_t sum = ip_transport_checksum(source, ip, IP_PROTOCOL_UDP, p, length);
        if (sum == 0) {
            sum = 0xffff; /* 0 would mean "no checksum". */
        }
        memcpy(p + 6, &sum, 2);
    }
    result = ip_send(frame, source, ip, d->raw ? IP_PROTOCOL_ICMP : IP_PROTOCOL_UDP, length);
    if (result == 0) {
        result = (int64_t)message->size;
    }
out:
    mutex_unlock(&net_lock);
    return result;
}

static bool dgram_readable(void *arg) {
    struct dgram_socket *d = arg;
    return d->head || d->shut_read;
}

static int64_t dgram_receive(struct socket *socket, struct socket_message *message) {
    struct dgram_socket *d = dgram_of(socket);
    mutex_lock(&net_lock);
    int64_t result =
        socket_wait(socket, &net_lock, &d->wait, dgram_readable, d, message->flags, false);
    if (result == 0) {
        struct datagram *datagram = d->head;
        if (!datagram) {
            result = 0; /* Shut down for reading. */
        } else {
            size_t n = datagram->length < message->size ? datagram->length : message->size;
            memcpy(message->data, datagram->data, n);
            result = (int64_t)n;
            if (n < datagram->length) {
                if (message->flags & VX_MSG_TRUNC) {
                    result = (int64_t)datagram->length; /* Linux: the real length. */
                }
                message->flags |= VX_MSG_TRUNC;
            } else {
                message->flags &= ~VX_MSG_TRUNC;
            }
            if (message->address) {
                inet_make_address(message->address, &message->address_length, datagram->from,
                                  datagram->from_port);
            }
            if (!(message->flags & VX_MSG_PEEK)) {
                d->head = datagram->next;
                if (!d->head) {
                    d->tail = NULL;
                }
                d->queued -= datagram->length;
                kfree(datagram);
            }
        }
    }
    mutex_unlock(&net_lock);
    return result;
}

static int dgram_shutdown(struct socket *socket, int how) {
    struct dgram_socket *d = dgram_of(socket);
    mutex_lock(&net_lock);
    if (how != VX_SHUT_WRITE) {
        d->shut_read = true;
    }
    if (how != VX_SHUT_READ) {
        d->shut_write = true;
    }
    wait_queue_wake_all(&d->wait);
    mutex_unlock(&net_lock);
    return 0;
}

static int dgram_address(struct socket *socket, bool peer, struct vx_socket_address *address,
                         size_t *length) {
    struct dgram_socket *d = dgram_of(socket);
    mutex_lock(&net_lock);
    int error = 0;
    if (peer) {
        if (d->connected) {
            inet_make_address(address, length, d->remote_ip, d->remote_port);
        } else {
            error = -VX_ENOTCONN;
        }
    } else {
        ipv4_t ip = d->local_ip;
        if (!ip && d->connected) {
            ip = ip_source_for(d->remote_ip);
        }
        inet_make_address(address, length, ip, d->local_port);
    }
    mutex_unlock(&net_lock);
    return error;
}

static uint32_t dgram_poll(struct socket *socket) {
    struct dgram_socket *d = dgram_of(socket);
    mutex_lock(&net_lock);
    uint32_t ready = OBJECT_WRITABLE | (dgram_readable(d) ? OBJECT_READABLE : 0);
    mutex_unlock(&net_lock);
    return ready;
}

static int64_t dgram_pending(struct socket *socket) {
    struct dgram_socket *d = dgram_of(socket);
    mutex_lock(&net_lock);
    int64_t n = d->head ? (int64_t)d->head->length : 0; /* Linux: the next datagram's size. */
    mutex_unlock(&net_lock);
    return n;
}

static void dgram_release(struct socket *socket) {
    struct dgram_socket *d = dgram_of(socket);
    mutex_lock(&net_lock);
    struct dgram_socket **p = d->raw ? &raw_sockets : &udp_sockets;
    while (*p && *p != d) {
        p = &(*p)->next;
    }
    if (*p) {
        *p = d->next;
    }
    mutex_unlock(&net_lock);
    while (d->head) {
        struct datagram *next = d->head->next;
        kfree(d->head);
        d->head = next;
    }
    kfree(d);
}

static const struct socket_ops dgram_ops = {
    .bind = dgram_bind,
    .connect = dgram_connect,
    .send = dgram_send,
    .receive = dgram_receive,
    .shutdown = dgram_shutdown,
    .address = dgram_address,
    .poll = dgram_poll,
    .pending = dgram_pending,
    .release = dgram_release,
};

static int dgram_create(struct socket *socket, bool raw) {
    struct dgram_socket *d = kzalloc(sizeof(*d));
    if (!d) {
        return -VX_ENOMEM;
    }
    d->socket = socket;
    d->raw = raw;
    socket->data = d;
    socket->ops = &dgram_ops;
    mutex_lock(&net_lock);
    struct dgram_socket **list = raw ? &raw_sockets : &udp_sockets;
    d->next = *list;
    *list = d;
    mutex_unlock(&net_lock);
    return 0;
}

int inet_create(struct socket *socket) {
    switch (socket->type) {
    case VX_SOCK_STREAM:
        if (socket->protocol && socket->protocol != VX_IPPROTO_TCP) {
            return -VX_EPROTONOSUPPORT;
        }
        socket->protocol = VX_IPPROTO_TCP;
        return tcp_create(socket);
    case VX_SOCK_DGRAM:
        if (socket->protocol && socket->protocol != VX_IPPROTO_UDP) {
            return -VX_EPROTONOSUPPORT;
        }
        socket->protocol = VX_IPPROTO_UDP;
        return dgram_create(socket, false);
    case VX_SOCK_RAW:
        if (socket->protocol != VX_IPPROTO_ICMP) {
            return -VX_EPROTONOSUPPORT;
        }
        return dgram_create(socket, true);
    default:
        return -VX_EPROTONOSUPPORT;
    }
}
