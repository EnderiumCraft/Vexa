#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vexa/net.h>

/*
 * Networking helpers: addresses, a DNS resolver (A and AAAA records over UDP,
 * with /etc/hosts first), and connecting by name.
 */

struct vx_socket_address vx_inet_address(uint32_t address, uint16_t port) {
    struct vx_socket_address a;
    memset(&a, 0, sizeof(a));
    a.inet.family = VX_AF_INET;
    a.inet.port = vx_net16(port);
    a.inet.address = address;
    return a;
}

int vx_parse_ipv4(const char *text, uint32_t *address) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) {
        if (*text < '0' || *text > '9') {
            return -VX_EINVAL;
        }
        unsigned value = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9' && digits < 4) {
            value = value * 10 + (unsigned)(*text++ - '0');
            digits++;
        }
        if (value > 255 || (i < 3 && *text++ != '.')) {
            return -VX_EINVAL;
        }
        bytes[i] = (uint8_t)value;
    }
    if (*text) {
        return -VX_EINVAL;
    }
    memcpy(address, bytes, 4);
    return 0;
}

char *vx_format_ipv4(uint32_t address, char text[16]) {
    const uint8_t *b = (const uint8_t *)&address;
    snprintf(text, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

struct vx_socket_address vx_inet6_address(const uint8_t address[16], uint16_t port) {
    struct vx_socket_address a;
    memset(&a, 0, sizeof(a));
    a.inet6.family = VX_AF_INET6;
    a.inet6.port = vx_net16(port);
    memcpy(a.inet6.address, address, 16);
    return a;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int vx_parse_ipv6(const char *text, uint8_t address[16]) {
    uint16_t words[8];
    int count = 0, gap = -1; /* gap: where "::" was. */
    const char *p = text;
    if (p[0] == ':') {
        if (p[1] != ':') {
            return -VX_EINVAL;
        }
        gap = 0;
        p += 2;
    }
    while (*p) {
        if (count == 8) {
            return -VX_EINVAL;
        }
        /* An IPv4 address at the end: two words. */
        const char *dot = strchr(p, '.');
        const char *colon = strchr(p, ':');
        if (dot && (!colon || dot < colon)) {
            uint32_t v4;
            if (count > 6 || vx_parse_ipv4(p, &v4) != 0) {
                return -VX_EINVAL;
            }
            const uint8_t *b = (const uint8_t *)&v4;
            words[count++] = (uint16_t)(b[0] << 8 | b[1]);
            words[count++] = (uint16_t)(b[2] << 8 | b[3]);
            p += strlen(p);
            break;
        }
        unsigned value = 0;
        int digits = 0, d;
        while ((d = hex_digit(*p)) >= 0) {
            if (++digits > 4) {
                return -VX_EINVAL;
            }
            value = value << 4 | (unsigned)d;
            p++;
        }
        if (digits == 0) {
            return -VX_EINVAL;
        }
        words[count++] = (uint16_t)value;
        if (*p == ':') {
            p++;
            if (*p == ':') {
                if (gap >= 0) {
                    return -VX_EINVAL; /* Only one "::". */
                }
                gap = count;
                p++;
            } else if (!*p) {
                return -VX_EINVAL; /* A trailing single colon. */
            }
        } else if (*p) {
            return -VX_EINVAL;
        }
    }
    if (gap < 0 ? count != 8 : count > 7) {
        return -VX_EINVAL;
    }
    uint16_t full[8] = {0};
    int tail = gap < 0 ? 0 : count - gap;
    for (int i = 0; i < count - tail; i++) {
        full[i] = words[i];
    }
    for (int i = 0; i < tail; i++) {
        full[8 - tail + i] = words[gap + i];
    }
    for (int i = 0; i < 8; i++) {
        address[2 * i] = (uint8_t)(full[i] >> 8);
        address[2 * i + 1] = (uint8_t)full[i];
    }
    return 0;
}

char *vx_format_ipv6(const uint8_t a[16], char text[46]) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(a, mapped, 12) == 0) {
        snprintf(text, 46, "::ffff:%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
        return text;
    }
    uint16_t words[8];
    for (int i = 0; i < 8; i++) {
        words[i] = (uint16_t)(a[2 * i] << 8 | a[2 * i + 1]);
    }
    /* The longest run of two or more zero words becomes "::". */
    int best = -1, best_length = 1;
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && words[j] == 0) {
            j++;
        }
        if (j - i > best_length) {
            best = i;
            best_length = j - i;
        }
        i = j > i ? j : i + 1;
    }
    size_t n = 0;
    text[0] = '\0';
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            n += (size_t)snprintf(text + n, 46 - n, "::");
            i += best_length - 1;
            continue;
        }
        bool colon = i > 0 && i != best + best_length;
        n += (size_t)snprintf(text + n, 46 - n, "%s%x", colon ? ":" : "", words[i]);
    }
    return text;
}

/* Looks `name` up in /etc/hosts ("address name alias..." lines): an IPv4
 * address (4 bytes) or an IPv6 one (16), as `size` says. */
static int from_hosts(const char *name, void *address, int size) {
    int handle = vx_open("/etc/hosts", VX_OPEN_READ);
    if (handle < 0) {
        return -VX_ENOENT;
    }
    char text[4096];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    if (n <= 0) {
        return -VX_ENOENT;
    }
    text[n] = '\0';
    size_t name_length = strlen(name);
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        /* The address, then the names. */
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        char *first = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        char saved = *p;
        *p = '\0';
        uint8_t candidate[16];
        bool numeric = size == 4 ? vx_parse_ipv4(first, (uint32_t *)candidate) == 0
                                 : vx_parse_ipv6(first, candidate) == 0;
        *p = saved;
        while (numeric && *p) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            char *word = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if ((size_t)(p - word) == name_length && strncmp(word, name, name_length) == 0) {
                memcpy(address, candidate, (size_t)size);
                return 0;
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
    return -VX_ENOENT;
}

/* Skips a (possibly compressed) name in a DNS message; returns the offset
 * after it, or -1. */
static long skip_name(const uint8_t *m, long length, long at) {
    while (at < length) {
        uint8_t label = m[at];
        if (label == 0) {
            return at + 1;
        }
        if ((label & 0xc0) == 0xc0) {
            return at + 2 <= length ? at + 2 : -1;
        }
        at += 1 + label;
    }
    return -1;
}

/* Asks `server` for `name`'s address: type 1 (A, 4 bytes) or 28 (AAAA, 16). */
static long dns_query(const struct vx_socket_address *server, size_t server_length,
                      const char *name, int qtype, void *address) {
    int want = qtype == 1 ? 4 : 16; /* The address size. */
    uint8_t query[512];
    uint16_t id = (uint16_t)vx_uptime() ^ (uint16_t)(vx_process_id() << 8);
    memset(query, 0, 12);
    query[0] = (uint8_t)(id >> 8);
    query[1] = (uint8_t)id;
    query[2] = 0x01; /* Recursion desired. */
    query[5] = 1;    /* One question. */
    long at = 12;
    for (const char *label = name; *label;) {
        const char *dot = strchr(label, '.');
        size_t n = dot ? (size_t)(dot - label) : strlen(label);
        if (n == 0 || n > 63 || at + (long)n + 6 > (long)sizeof(query)) {
            return -VX_EINVAL;
        }
        query[at++] = (uint8_t)n;
        memcpy(query + at, label, n);
        at += (long)n;
        label += n + (dot ? 1 : 0);
    }
    query[at++] = 0;
    query[at++] = 0;
    query[at++] = (uint8_t)qtype;
    query[at++] = 0;
    query[at++] = 1; /* Class IN */

    int handle = vx_socket(server->family, VX_SOCK_DGRAM, 0);
    if (handle < 0) {
        return handle;
    }
    struct vx_socket_address to = *server;
    long result = -VX_ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && result == -VX_ETIMEDOUT; attempt++) {
        struct vx_message message = {query, (unsigned long)at, 0, (unsigned)server_length, &to};
        long sent = vx_send(handle, &message);
        if (sent < 0) {
            result = sent;
            break;
        }
        for (;;) {
            struct vx_poll poll = {handle, VX_POLL_READ, 0};
            if (vx_poll(&poll, 1, 2000) <= 0) {
                break; /* Ask again. */
            }
            uint8_t answer[1500];
            struct vx_socket_address from;
            struct vx_message in = {answer, sizeof(answer), 0, 0, &from};
            long n = vx_receive(handle, &in);
            bool same = server->family == VX_AF_INET
                            ? from.inet.address == server->inet.address
                            : memcmp(from.inet6.address, server->inet6.address, 16) == 0;
            if (n < 12 || answer[0] != query[0] || answer[1] != query[1] || !same) {
                continue; /* Not the answer to this question. */
            }
            int rcode = answer[3] & 0xf;
            int answers = answer[6] << 8 | answer[7];
            if (rcode == 3) {
                result = -VX_ENOENT; /* No such name. */
                break;
            }
            long p = skip_name(answer, n, 12);
            p = p < 0 ? -1 : p + 4; /* Type and class of the question. */
            result = -VX_ENOENT;
            for (int i = 0; i < answers && p > 0; i++) {
                p = skip_name(answer, n, p);
                if (p < 0 || p + 10 > n) {
                    break;
                }
                int type = answer[p] << 8 | answer[p + 1];
                int size = answer[p + 8] << 8 | answer[p + 9];
                p += 10;
                if (p + size > n) {
                    break;
                }
                if (type == qtype && size == want) {
                    memcpy(address, answer + p, (size_t)size);
                    result = 0;
                    break;
                }
                p += size; /* E.g. a CNAME before the address. */
            }
            break;
        }
    }
    vx_close(handle);
    return result;
}

/* Asks the name server DHCP (or, failing that, an IPv6 router) gave us. */
static long dns_lookup(const char *name, int qtype, void *address) {
    struct vx_net_interface interfaces[8];
    long count = vx_net_info(interfaces, 8);
    static const uint8_t zero[16];
    for (long i = 0; i < count && i < 8; i++) {
        if (interfaces[i].dns) {
            struct vx_socket_address server = vx_inet_address(interfaces[i].dns, 53);
            return dns_query(&server, sizeof(server.inet), name, qtype, address);
        }
    }
    for (long i = 0; i < count && i < 8; i++) {
        if (memcmp(interfaces[i].dns6, zero, 16) != 0) {
            struct vx_socket_address server = vx_inet6_address(interfaces[i].dns6, 53);
            return dns_query(&server, sizeof(server.inet6), name, qtype, address);
        }
    }
    return -VX_ENETUNREACH; /* No name server. */
}

long vx_resolve(const char *name, uint32_t *address) {
    if (vx_parse_ipv4(name, address) == 0) {
        return 0;
    }
    uint8_t v6[16];
    if (vx_parse_ipv6(name, v6) == 0) {
        return -VX_ENOENT; /* An IPv6 address has no IPv4 one. */
    }
    if (strcmp(name, "localhost") == 0) {
        *address = 0x0100007f; /* 127.0.0.1 */
        return 0;
    }
    if (from_hosts(name, address, 4) == 0) {
        return 0;
    }
    return dns_lookup(name, 1, address);
}

long vx_resolve6(const char *name, uint8_t address[16]) {
    if (vx_parse_ipv6(name, address) == 0) {
        return 0;
    }
    uint32_t v4;
    if (vx_parse_ipv4(name, &v4) == 0) {
        return -VX_ENOENT;
    }
    if (strcmp(name, "localhost") == 0 || strcmp(name, "ip6-localhost") == 0) {
        memset(address, 0, 16);
        address[15] = 1; /* ::1 */
        return 0;
    }
    if (from_hosts(name, address, 16) == 0) {
        return 0;
    }
    return dns_lookup(name, 28, address);
}

int vx_connect_to(const char *host, uint16_t port) {
    struct vx_socket_address to;
    size_t length;
    uint32_t address;
    uint8_t address6[16];
    long error = vx_resolve(host, &address);
    if (error == 0) {
        to = vx_inet_address(address, port);
        length = sizeof(to.inet);
    } else if (vx_resolve6(host, address6) == 0) {
        to = vx_inet6_address(address6, port);
        length = sizeof(to.inet6);
    } else {
        return (int)error;
    }
    int handle = vx_socket(to.family, VX_SOCK_STREAM, 0);
    if (handle < 0) {
        return handle;
    }
    error = vx_connect(handle, &to, length);
    if (error) {
        vx_close(handle);
        return (int)error;
    }
    return handle;
}
