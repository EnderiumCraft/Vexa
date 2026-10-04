#ifndef LIBVEXA_NET_H
#define LIBVEXA_NET_H

#include <stdint.h>
#include <vexa/syscall.h>

/* Networking helpers on top of the socket system calls. IPv4 addresses are
 * in network byte order, as in struct vx_inet_address; IPv6 ones are 16
 * bytes, as in struct vx_inet6_address. */

static inline uint16_t vx_net16(uint16_t value) {
    return __builtin_bswap16(value);
}

/* An IPv4 socket address. `port` is in host byte order. */
struct vx_socket_address vx_inet_address(uint32_t address, uint16_t port);
/* "10.0.2.2" -> address; 0 on success, -VX_EINVAL if it isn't one. */
int vx_parse_ipv4(const char *text, uint32_t *address);
/* Formats an address as "a.b.c.d" (16 bytes is always enough). */
char *vx_format_ipv4(uint32_t address, char text[16]);
/* Finds a host's IPv4 address: a numeric address, "localhost", a name in
 * /etc/hosts, or a DNS lookup through the name server DHCP gave us. Returns
 * 0, or a negative VX_E* error (-VX_ENOENT: no such name). */
long vx_resolve(const char *name, uint32_t *address);
/* An IPv6 socket address. `port` is in host byte order. */
struct vx_socket_address vx_inet6_address(const uint8_t address[16], uint16_t port);
/* "2001:db8::1" (or "::ffff:10.0.2.2") -> address; 0 or -VX_EINVAL. */
int vx_parse_ipv6(const char *text, uint8_t address[16]);
/* Formats an address the short way ("fe80::1"); 46 bytes is always enough. */
char *vx_format_ipv6(const uint8_t address[16], char text[46]);
/* Finds a host's IPv6 address, like vx_resolve (a numeric address,
 * "localhost", /etc/hosts, or a DNS AAAA lookup). */
long vx_resolve6(const char *name, uint8_t address[16]);
/* Resolves `host` and opens a TCP connection to it, by IPv4 if the name has
 * an IPv4 address, else by IPv6; returns the socket handle or a negative
 * VX_E* error. */
int vx_connect_to(const char *host, uint16_t port);

#endif
