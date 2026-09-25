#ifndef VEXA_NET_INET_H
#define VEXA_NET_INET_H

/* Shared by the IPv4 socket protocols (inet.c, tcp.c). */

#include <vexa/net.h>
#include <vexa/socket.h>

/* Checks and converts a VX_AF_INET address. */
int inet_parse_address(const struct vx_socket_address *address, size_t length, ipv4_t *ip,
                       uint16_t *port);
void inet_make_address(struct vx_socket_address *address, size_t *length, ipv4_t ip,
                       uint16_t port);
/* A free port for a new connection or socket (network byte order), or 0. */
uint16_t inet_ephemeral_port(bool (*in_use)(ipv4_t ip, uint16_t port, bool reuse));

int tcp_create(struct socket *socket);
bool tcp_port_in_use(ipv4_t ip, uint16_t port, bool reuse);

#endif
