#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vexa/net.h>

/*
 * Networking helpers: addresses, a DNS resolver (A records over UDP, with
 * /etc/hosts first), and connecting by name.
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

/* Looks `name` up in /etc/hosts ("address name alias..." lines). */
static int from_hosts(const char *name, uint32_t *address) {
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
        uint32_t candidate;
        bool numeric = vx_parse_ipv4(first, &candidate) == 0;
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
                *address = candidate;
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

static long dns_query(uint32_t server, const char *name, uint32_t *address) {
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
    query[at++] = 1; /* Type A */
    query[at++] = 0;
    query[at++] = 1; /* Class IN */

    int handle = vx_socket(VX_AF_INET, VX_SOCK_DGRAM, 0);
    if (handle < 0) {
        return handle;
    }
    struct vx_socket_address to = vx_inet_address(server, 53);
    long result = -VX_ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && result == -VX_ETIMEDOUT; attempt++) {
        struct vx_message message = {query, (unsigned long)at, 0, sizeof(to.inet), &to};
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
            if (n < 12 || answer[0] != query[0] || answer[1] != query[1] ||
                from.inet.address != server) {
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
                if (type == 1 && size == 4) {
                    memcpy(address, answer + p, 4);
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

long vx_resolve(const char *name, uint32_t *address) {
    if (vx_parse_ipv4(name, address) == 0) {
        return 0;
    }
    if (strcmp(name, "localhost") == 0) {
        *address = 0x0100007f; /* 127.0.0.1 */
        return 0;
    }
    if (from_hosts(name, address) == 0) {
        return 0;
    }
    struct vx_net_interface interfaces[8];
    long count = vx_net_info(interfaces, 8);
    for (long i = 0; i < count && i < 8; i++) {
        if (interfaces[i].dns) {
            return dns_query(interfaces[i].dns, name, address);
        }
    }
    return -VX_ENETUNREACH; /* No name server. */
}

int vx_connect_to(const char *host, uint16_t port) {
    uint32_t address;
    long error = vx_resolve(host, &address);
    if (error) {
        return (int)error;
    }
    int handle = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    if (handle < 0) {
        return handle;
    }
    struct vx_socket_address to = vx_inet_address(address, port);
    error = vx_connect(handle, &to, sizeof(to.inet));
    if (error) {
        vx_close(handle);
        return (int)error;
    }
    return handle;
}
