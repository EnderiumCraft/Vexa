#ifndef LIBVEXA_NET_IF_H
#define LIBVEXA_NET_IF_H

/* Network interfaces: their names and addresses (ioctl SIOCGIFCONF), from
 * vx_net_info. */
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_netmask;
        struct sockaddr ifr_broadaddr;
        short ifr_flags;
        int ifr_ifindex;
        int ifr_mtu;
        char __size[24];
    };
};

struct ifconf {
    int ifc_len;
    union {
        char *ifc_buf;
        struct ifreq *ifc_req;
    };
};

#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCGIFADDR 0x8915
#define SIOCGIFNETMASK 0x891b

unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int index, char *name);

#ifdef __cplusplus
}
#endif

#endif
