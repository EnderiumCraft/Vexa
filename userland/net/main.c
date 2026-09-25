/* net: the network interfaces, their addresses and traffic. */
#include <stdio.h>
#include <vexa/net.h>

int main(void) {
    struct vx_net_interface list[8];
    long count = vx_net_info(list, 8);
    if (count < 0) {
        fprintf(stderr, "net: %s\n", vx_strerror(count));
        return 1;
    }
    for (long i = 0; i < count && i < 8; i++) {
        struct vx_net_interface *n = &list[i];
        char address[16], netmask[16], gateway[16], dns[16];
        printf("%s: %s%s%s mtu %u\n", n->name, n->flags & VX_NET_UP ? "up" : "down",
               n->flags & VX_NET_LOOPBACK ? " loopback" : "",
               n->flags & VX_NET_DHCP ? " dhcp" : "", n->mtu);
        if (!(n->flags & VX_NET_LOOPBACK)) {
            printf("  mac %02x:%02x:%02x:%02x:%02x:%02x\n", n->mac[0], n->mac[1], n->mac[2],
                   n->mac[3], n->mac[4], n->mac[5]);
        }
        if (n->address) {
            printf("  address %s netmask %s\n", vx_format_ipv4(n->address, address),
                   vx_format_ipv4(n->netmask, netmask));
        } else {
            printf("  no address yet\n");
        }
        if (n->gateway) {
            printf("  router %s", vx_format_ipv4(n->gateway, gateway));
            if (n->dns) {
                printf(" name server %s", vx_format_ipv4(n->dns, dns));
            }
            printf("\n");
        }
        printf("  received %llu packets (%llu bytes), sent %llu (%llu bytes)\n", n->rx_packets,
               n->rx_bytes, n->tx_packets, n->tx_bytes);
    }
    return 0;
}
