#ifndef LIBVEXA_NET_H
#define LIBVEXA_NET_H

#include <stdint.h>
#include <vexa/syscall.h>

/* Networking helpers on top of the socket system calls. IPv4 addresses are
 * in network byte order, as in struct vx_inet_address. */

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
/* Resolves `host` and opens a TCP connection to it; returns the socket
 * handle or a negative VX_E* error. */
int vx_connect_to(const char *host, uint16_t port);

#endif
