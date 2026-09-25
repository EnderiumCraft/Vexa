#include <vexa/arch.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/random.h>
#include <vexa/string.h>

/*
 * DHCP client: asks the network for an address, a netmask, a router and a
 * name server (DISCOVER, OFFER, REQUEST, ACK), for each network card, and
 * renews the lease halfway through it.
 */

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC 0x63538263 /* 99.130.83.99, as it reads in memory. */

#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6

#define OPTION_PAD 0
#define OPTION_NETMASK 1
#define OPTION_ROUTER 3
#define OPTION_DNS 6
#define OPTION_REQUESTED_IP 50
#define OPTION_LEASE_TIME 51
#define OPTION_MESSAGE_TYPE 53
#define OPTION_SERVER_ID 54
#define OPTION_PARAMETERS 55
#define OPTION_END 255

#define RETRY_MS 2000

struct __attribute__((packed)) dhcp_message {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    ipv4_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
};

struct __attribute__((packed)) udp_header {
    uint16_t source_port, destination_port, length, checksum;
};

enum dhcp_state { DHCP_IDLE, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };

#define MAX_CLIENTS 4

struct dhcp_client {
    struct net_interface *net;
    enum dhcp_state state;
    uint32_t xid;
    ipv4_t offered, server;
    uint64_t next_send; /* timer_ms() for the next (re)transmission. */
    uint64_t renew_at;
    int tries;
};

static struct dhcp_client clients[MAX_CLIENTS];

static void send_message(struct dhcp_client *client, uint8_t type) {
    uint8_t *frame = net_frame();
    if (!frame) {
        return;
    }
    struct udp_header *udp = (struct udp_header *)(frame + TRANSPORT_OFFSET);
    struct dhcp_message *m = (struct dhcp_message *)(udp + 1);
    memset(m, 0, sizeof(*m));
    m->op = 1; /* A request. */
    m->htype = 1;
    m->hlen = ETH_ADDRESS;
    m->xid = client->xid;
    m->flags = net16(0x8000); /* Please broadcast the answer: we have no address yet. */
    memcpy(m->chaddr, client->net->mac, ETH_ADDRESS);
    m->magic = DHCP_MAGIC;
    uint8_t *o = m->options;
    *o++ = OPTION_MESSAGE_TYPE;
    *o++ = 1;
    *o++ = type;
    if (type == DHCP_REQUEST) {
        *o++ = OPTION_REQUESTED_IP;
        *o++ = 4;
        memcpy(o, &client->offered, 4);
        o += 4;
        *o++ = OPTION_SERVER_ID;
        *o++ = 4;
        memcpy(o, &client->server, 4);
        o += 4;
    }
    *o++ = OPTION_PARAMETERS;
    *o++ = 3;
    *o++ = OPTION_NETMASK;
    *o++ = OPTION_ROUTER;
    *o++ = OPTION_DNS;
    *o++ = OPTION_END;
    size_t length = sizeof(struct udp_header) + sizeof(*m);
    udp->source_port = net16(DHCP_CLIENT_PORT);
    udp->destination_port = net16(DHCP_SERVER_PORT);
    udp->length = net16((uint16_t)length);
    udp->checksum = 0;
    udp->checksum = ip_transport_checksum(0, IPV4_BROADCAST, IP_PROTOCOL_UDP, udp, length);
    ip_send_on(client->net, IPV4_BROADCAST, frame, 0, IPV4_BROADCAST, IP_PROTOCOL_UDP, length);
}

static void restart(struct dhcp_client *client, uint64_t now) {
    random_bytes(&client->xid, sizeof(client->xid));
    client->state = DHCP_SELECTING;
    client->tries = 0;
    client->next_send = now + RETRY_MS;
    send_message(client, DHCP_DISCOVER);
}

void dhcp_start(struct net_interface *net) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].net) {
            clients[i].net = net;
            restart(&clients[i], timer_ms());
            return;
        }
    }
}

void dhcp_tick(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct dhcp_client *client = &clients[i];
        if (!client->net) {
            continue;
        }
        if (client->state == DHCP_BOUND) {
            if (now >= client->renew_at) {
                /* Ask again for the same address. */
                client->state = DHCP_REQUESTING;
                client->tries = 0;
                client->next_send = now + RETRY_MS;
                send_message(client, DHCP_REQUEST);
            }
        } else if (now >= client->next_send) {
            client->tries++;
            /* Back off to every 16 seconds while nobody answers. */
            uint64_t delay = RETRY_MS << (client->tries < 3 ? client->tries : 3);
            client->next_send = now + delay;
            if (client->state == DHCP_REQUESTING && client->tries > 3) {
                restart(client, now);
            } else {
                send_message(client, client->state == DHCP_REQUESTING ? DHCP_REQUEST
                                                                       : DHCP_DISCOVER);
            }
        }
    }
}

static void print_ip(const char *label, ipv4_t ip) {
    const uint8_t *b = (const uint8_t *)&ip;
    kprintf("%s%u.%u.%u.%u", label, b[0], b[1], b[2], b[3]);
}

void dhcp_input(struct net_interface *net, const uint8_t *data, size_t length) {
    struct dhcp_client *client = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].net == net) {
            client = &clients[i];
        }
    }
    const struct dhcp_message *m = (const struct dhcp_message *)data;
    size_t fixed = offsetof(struct dhcp_message, options);
    if (!client || length < fixed || m->op != 2 || m->xid != client->xid ||
        m->magic != DHCP_MAGIC || memcmp(m->chaddr, net->mac, ETH_ADDRESS) != 0) {
        return;
    }
    uint8_t type = 0;
    ipv4_t netmask = 0, router = 0, dns = 0, server = 0;
    uint32_t lease = 0;
    const uint8_t *o = m->options, *end = data + length;
    while (o < end && *o != OPTION_END) {
        if (*o == OPTION_PAD) {
            o++;
            continue;
        }
        if (o + 2 > end || o + 2 + o[1] > end) {
            break;
        }
        uint8_t code = o[0], size = o[1];
        const uint8_t *value = o + 2;
        if (code == OPTION_MESSAGE_TYPE && size >= 1) {
            type = value[0];
        } else if (size >= 4) {
            switch (code) {
            case OPTION_NETMASK: memcpy(&netmask, value, 4); break;
            case OPTION_ROUTER: memcpy(&router, value, 4); break;
            case OPTION_DNS: memcpy(&dns, value, 4); break;
            case OPTION_SERVER_ID: memcpy(&server, value, 4); break;
            case OPTION_LEASE_TIME:
                memcpy(&lease, value, 4);
                lease = net32(lease);
                break;
            }
        }
        o += 2 + size;
    }
    uint64_t now = timer_ms();
    if (type == DHCP_OFFER && client->state == DHCP_SELECTING) {
        client->offered = m->yiaddr;
        client->server = server;
        client->state = DHCP_REQUESTING;
        client->tries = 0;
        client->next_send = now + RETRY_MS;
        send_message(client, DHCP_REQUEST);
    } else if (type == DHCP_ACK && client->state == DHCP_REQUESTING) {
        bool changed = net->address != m->yiaddr;
        net->address = m->yiaddr;
        net->netmask = netmask ? netmask : IPV4(255, 255, 255, 0);
        net->gateway = router;
        net->dns = dns;
        net->flags |= NET_FLAG_DHCP;
        client->state = DHCP_BOUND;
        /* Renew halfway through the lease (at most once a day). */
        uint64_t seconds = lease && lease < 2 * 86400 ? lease / 2 : 86400;
        client->renew_at = now + (seconds ? seconds : 1) * 1000;
        if (changed) {
            kprintf("[net] %s: ", net->name);
            print_ip("address ", net->address);
            print_ip(", netmask ", net->netmask);
            print_ip(", router ", net->gateway);
            print_ip(", name server ", net->dns);
            kprintf(" (DHCP)\n");
        }
    } else if (type == DHCP_NAK) {
        net->address = 0;
        net->flags &= ~(uint32_t)NET_FLAG_DHCP;
        restart(client, now);
    }
}
