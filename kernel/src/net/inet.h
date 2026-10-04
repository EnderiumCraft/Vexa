#ifndef VEXA_NET_INET_H
#define VEXA_NET_INET_H

/* Shared by the IP socket protocols (inet.c, tcp.c), for VX_AF_INET and
 * VX_AF_INET6 sockets alike. Addresses are ip6_t (IPv4 ones mapped; "any" is
 * all zeros). */

#include <vexa/net.h>
#include <vexa/socket.h>

/* Checks and converts an address for `socket` (VX_AF_INET or VX_AF_INET6). */
int inet_parse_address(const struct socket *socket, const struct vx_socket_address *address,
                       size_t length, ip6_t *ip, uint16_t *port);
/* The socket's kind of address: sockaddr_in or sockaddr_in6. */
void inet_make_address(const struct socket *socket, struct vx_socket_address *address,
                       size_t *length, ip6_t ip, uint16_t port);

/* Which IP versions a socket bound to `ip` takes packets of. */
#define INET_V4 1
#define INET_V6 2
int inet_domains(const struct socket *socket, ip6_t ip);
/* Does a socket bound to (`ip`, `domains`) take a packet sent to `destination`? */
bool inet_takes(ip6_t ip, int domains, const struct ip_packet *packet);
/* Do two bindings of the same port overlap? */
bool inet_overlap(ip6_t a, int a_domains, ip6_t b, int b_domains);

typedef bool (*inet_in_use_fn)(ip6_t ip, int domains, uint16_t port, bool reuse);
/* A free port for a new connection or socket (network byte order), or 0. */
uint16_t inet_ephemeral_port(inet_in_use_fn in_use);

int tcp_create(struct socket *socket);

#endif
