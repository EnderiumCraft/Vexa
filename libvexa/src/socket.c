/* BSD sockets over Vexa's (the address layouts are the same: they pass
 * straight through), select, and names to addresses (getaddrinfo and the
 * older calls) through vx_resolve. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <vexa/net.h>
#include <vexa/syscall.h>

#include "internal.h"

/* ---- Non-blocking descriptors (kept here; each call says so) ---- */

#define TRACKED 1024
static unsigned char nonblocking[TRACKED / 8];
static unsigned char socket_types[TRACKED]; /* SOCK_* of a socket, else 0. */

bool __vx_nonblocking(int fd) {
    return fd >= 0 && fd < TRACKED && (nonblocking[fd / 8] & (1 << (fd % 8)));
}

void __vx_set_nonblocking(int fd, bool on) {
    if (fd < 0 || fd >= TRACKED) {
        return;
    }
    if (on) {
        nonblocking[fd / 8] |= (unsigned char)(1 << (fd % 8));
    } else {
        nonblocking[fd / 8] &= (unsigned char)~(1 << (fd % 8));
    }
}

bool __vx_is_socket(int fd) {
    return fd >= 0 && fd < TRACKED && socket_types[fd];
}

void __vx_forget_fd(int fd) {
    if (fd >= 0 && fd < TRACKED) {
        __vx_set_nonblocking(fd, false);
        socket_types[fd] = 0;
    }
}

static int new_socket(int handle, int type, bool nonblock) {
    if (handle < 0) {
        return (int)__vx_errno_result(handle);
    }
    if (handle < TRACKED) {
        socket_types[handle] = (unsigned char)(type & VX_SOCK_TYPE_MASK);
        __vx_set_nonblocking(handle, nonblock);
    }
    return handle;
}

/* ---- Sockets ---- */

int socket(int family, int type, int protocol) {
    if (family == AF_INET6) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    bool nonblock = type & SOCK_NONBLOCK;
    /* (Vexa's non-blocking flag is libvexa's to keep: see send and recv.) */
    return new_socket(vx_socket(family, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), protocol), type,
                      nonblock);
}

int socketpair(int family, int type, int protocol, int fds[2]) {
    (void)protocol;
    long result = vx_socket_pair(family, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), fds);
    if (result < 0) {
        return (int)__vx_errno_result(result);
    }
    new_socket(fds[0], type, type & SOCK_NONBLOCK);
    new_socket(fds[1], type, type & SOCK_NONBLOCK);
    return 0;
}

int bind(int fd, const struct sockaddr *address, socklen_t length) {
    return (int)__vx_errno_result(vx_bind(fd, (const struct vx_socket_address *)address, length));
}

int listen(int fd, int backlog) {
    return (int)__vx_errno_result(vx_listen(fd, backlog));
}

int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    struct vx_socket_address peer;
    int handle = vx_accept(fd, &peer, __vx_nonblocking(fd) ? VX_SOCK_NONBLOCK : 0);
    if (handle >= 0 && address && length) {
        long size = vx_socket_address(handle, 1, &peer);
        socklen_t n = size > 0 && (socklen_t)size < *length ? (socklen_t)size : *length;
        memcpy(address, &peer, n);
        *length = size > 0 ? (socklen_t)size : 0;
    }
    return new_socket(handle, fd >= 0 && fd < TRACKED ? socket_types[fd] : SOCK_STREAM,
                      flags & SOCK_NONBLOCK);
}

int accept(int fd, struct sockaddr *address, socklen_t *length) {
    return accept4(fd, address, length, 0);
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
    /* (Even a non-blocking socket connects at once: Vexa's connect waits.) */
    return (int)__vx_errno_result(
        vx_connect(fd, (const struct vx_socket_address *)address, length));
}

static unsigned flags_of(int fd, int flags) {
    unsigned vx = (unsigned)flags & (MSG_PEEK | MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL);
    return __vx_nonblocking(fd) ? vx | VX_MSG_DONTWAIT : vx;
}

ssize_t sendto(int fd, const void *buffer, size_t size, int flags, const struct sockaddr *to,
               socklen_t length) {
    struct vx_message m = {
        .data = (void *)buffer, .size = size, .flags = flags_of(fd, flags),
        .address_length = to ? length : 0, .address = (struct vx_socket_address *)to,
    };
    return __vx_errno_result(vx_send(fd, &m));
}

ssize_t send(int fd, const void *buffer, size_t size, int flags) {
    return sendto(fd, buffer, size, flags, NULL, 0);
}

ssize_t recvfrom(int fd, void *buffer, size_t size, int flags, struct sockaddr *from,
                 socklen_t *length) {
    struct vx_socket_address sender;
    struct vx_message m = {
        .data = buffer, .size = size, .flags = flags_of(fd, flags),
        .address_length = sizeof(sender), .address = from ? &sender : NULL,
    };
    long n = vx_receive(fd, &m);
    if (n >= 0 && from && length) {
        socklen_t copy = m.address_length < *length ? m.address_length : *length;
        memcpy(from, &sender, copy);
        *length = m.address_length;
    }
    return __vx_errno_result(n);
}

ssize_t recv(int fd, void *buffer, size_t size, int flags) {
    return recvfrom(fd, buffer, size, flags, NULL, NULL);
}

/* (One piece at a time: the first buffer, then the rest if it all went.) */
ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    ssize_t total = 0;
    for (size_t i = 0; i < message->msg_iovlen; i++) {
        ssize_t n = sendto(fd, message->msg_iov[i].iov_base, message->msg_iov[i].iov_len, flags,
                           message->msg_name, message->msg_namelen);
        if (n < 0) {
            return total ? total : n;
        }
        total += n;
        if ((size_t)n < message->msg_iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    if (message->msg_iovlen == 0) {
        return 0;
    }
    socklen_t length = message->msg_namelen;
    ssize_t n = recvfrom(fd, message->msg_iov[0].iov_base, message->msg_iov[0].iov_len, flags,
                         message->msg_name, message->msg_name ? &length : NULL);
    message->msg_namelen = length;
    message->msg_controllen = 0;
    message->msg_flags = 0;
    return n;
}

int shutdown(int fd, int how) {
    return (int)__vx_errno_result(vx_shutdown(fd, how));
}

static int socket_name(int fd, int peer, struct sockaddr *address, socklen_t *length) {
    struct vx_socket_address a;
    long size = vx_socket_address(fd, peer, &a);
    if (size < 0) {
        return (int)__vx_errno_result(size);
    }
    socklen_t copy = (socklen_t)size < *length ? (socklen_t)size : *length;
    memcpy(address, &a, copy);
    *length = (socklen_t)size;
    return 0;
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    return socket_name(fd, 0, address, length);
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    return socket_name(fd, 1, address, length);
}

/* Options: the common ones are accepted (Vexa's sockets behave as if they
 * were set the usual way); unknown ones aren't. */
int setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    (void)name, (void)value, (void)length;
    if (!__vx_is_socket(fd)) {
        errno = ENOTSOCK;
        return -1;
    }
    if (level == SOL_SOCKET || level == IPPROTO_TCP || level == IPPROTO_IP ||
        level == IPPROTO_IPV6) {
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *length) {
    if (!__vx_is_socket(fd)) {
        errno = ENOTSOCK;
        return -1;
    }
    int answer = 0;
    if (level == SOL_SOCKET && name == SO_TYPE) {
        answer = socket_types[fd];
    } else if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF)) {
        answer = 65536;
    }
    if (value && length && *length >= sizeof(int)) {
        memcpy(value, &answer, sizeof(int));
        *length = sizeof(int);
    }
    return 0;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    int *argument = va_arg(args, int *);
    va_end(args);
    if (request == FIONBIO && argument) {
        __vx_set_nonblocking(fd, *argument != 0);
        return 0;
    }
    if (request == FIONREAD && argument) {
        *argument = 0;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* ---- select ---- */

int select(int count, fd_set *read, fd_set *write, fd_set *except, struct timeval *timeout) {
    struct pollfd fds[64];
    int n = 0;
    for (int fd = 0; fd < count && fd < FD_SETSIZE; fd++) {
        short events = (short)((read && FD_ISSET(fd, read) ? POLLIN : 0) |
                               (write && FD_ISSET(fd, write) ? POLLOUT : 0));
        if (!events && !(except && FD_ISSET(fd, except))) {
            continue;
        }
        if (n == 64) {
            errno = EINVAL;
            return -1;
        }
        fds[n++] = (struct pollfd){fd, events, 0};
    }
    int ms = timeout ? (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;
    int ready = poll(fds, (nfds_t)n, ms);
    if (ready < 0) {
        return -1;
    }
    if (read) {
        FD_ZERO(read);
    }
    if (write) {
        FD_ZERO(write);
    }
    if (except) {
        FD_ZERO(except);
    }
    int total = 0;
    for (int i = 0; i < n; i++) {
        bool any = false;
        if (read && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fds[i].fd, read);
            any = true;
        }
        if (write && (fds[i].revents & (POLLOUT | POLLERR))) {
            FD_SET(fds[i].fd, write);
            any = true;
        }
        total += any;
    }
    return total;
}

/* ---- Addresses as text ---- */

in_addr_t inet_addr(const char *text) {
    uint32_t address;
    return vx_parse_ipv4(text, &address) == 0 ? address : INADDR_NONE;
}

int inet_aton(const char *text, struct in_addr *address) {
    uint32_t a;
    if (vx_parse_ipv4(text, &a) != 0) {
        return 0;
    }
    address->s_addr = a;
    return 1;
}

char *inet_ntoa(struct in_addr address) {
    static char text[16];
    return vx_format_ipv4(address.s_addr, text);
}

int inet_pton(int family, const char *text, void *address) {
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    uint32_t a;
    if (vx_parse_ipv4(text, &a) != 0) {
        return 0;
    }
    memcpy(address, &a, 4);
    return 1;
}

const char *inet_ntop(int family, const void *address, char *text, socklen_t size) {
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    char buffer[16];
    uint32_t a;
    memcpy(&a, address, 4);
    vx_format_ipv4(a, buffer);
    if (strlen(buffer) + 1 > size) {
        errno = ENOSPC;
        return NULL;
    }
    strcpy(text, buffer);
    return text;
}

/* ---- Names ---- */

int h_errno;

struct hostent *gethostbyname(const char *name) {
    static struct hostent host;
    static char *addresses[2], *aliases[1];
    static uint32_t address;
    static char canonical[256];
    if (vx_resolve(name, &address) != 0) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    snprintf(canonical, sizeof(canonical), "%s", name);
    addresses[0] = (char *)&address;
    addresses[1] = NULL;
    aliases[0] = NULL;
    host = (struct hostent){canonical, aliases, AF_INET, 4, addresses};
    return &host;
}

struct hostent *gethostbyaddr(const void *address, socklen_t length, int family) {
    static struct hostent host;
    static char *addresses[2], *aliases[1];
    static uint32_t copy;
    static char text[16];
    if (family != AF_INET || length != 4) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    memcpy(&copy, address, 4);
    vx_format_ipv4(copy, text); /* (No reverse lookups: the address is its name.) */
    addresses[0] = (char *)&copy;
    addresses[1] = NULL;
    aliases[0] = NULL;
    host = (struct hostent){text, aliases, AF_INET, 4, addresses};
    return &host;
}

static const struct {
    const char *name;
    int port;
} services[] = {
    {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"domain", 53}, {"http", 80},
    {"pop3", 110}, {"ntp", 123}, {"imap", 143}, {"https", 443}, {"doom", 666},
};

struct servent *getservbyname(const char *name, const char *protocol) {
    static struct servent entry;
    static char *aliases[1];
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        if (strcmp(services[i].name, name) == 0) {
            entry = (struct servent){(char *)services[i].name, aliases, htons(services[i].port),
                                     (char *)(protocol ? protocol : "tcp")};
            return &entry;
        }
    }
    return NULL;
}

static int service_port(const char *service, int flags, int *port) {
    if (!service) {
        *port = 0;
        return 0;
    }
    char *end;
    long n = strtol(service, &end, 10);
    if (*service && !*end && n >= 0 && n <= 65535) {
        *port = (int)n;
        return 0;
    }
    if (flags & AI_NUMERICSERV) {
        return EAI_NONAME;
    }
    struct servent *s = getservbyname(service, NULL);
    if (!s) {
        return EAI_SERVICE;
    }
    *port = ntohs((uint16_t)s->s_port);
    return 0;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result) {
    int flags = hints ? hints->ai_flags : 0, family = hints ? hints->ai_family : AF_UNSPEC;
    int type = hints ? hints->ai_socktype : 0, protocol = hints ? hints->ai_protocol : 0;
    *result = NULL;
    if (family != AF_UNSPEC && family != AF_INET) {
        return EAI_FAMILY;
    }
    if (!node && !service) {
        return EAI_NONAME;
    }
    int port;
    int error = service_port(service, flags, &port);
    if (error) {
        return error;
    }
    uint32_t address;
    if (!node) {
        address = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    } else if (vx_parse_ipv4(node, &address) != 0) {
        if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        }
        long e = vx_resolve(node, &address);
        if (e != 0) {
            return e == -VX_ENOENT ? EAI_NONAME : EAI_AGAIN;
        }
    }
    /* One answer per kind of socket asked for (both, if none was). */
    int types[2] = {type ? type : SOCK_STREAM, SOCK_DGRAM};
    int count = type ? 1 : 2;
    struct addrinfo *last = NULL;
    for (int i = 0; i < count; i++) {
        struct addrinfo *info = calloc(1, sizeof(*info) + sizeof(struct sockaddr_in));
        if (!info) {
            freeaddrinfo(*result);
            *result = NULL;
            return EAI_MEMORY;
        }
        struct sockaddr_in *in = (struct sockaddr_in *)(info + 1);
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)port);
        in->sin_addr.s_addr = address;
        info->ai_family = AF_INET;
        info->ai_socktype = types[i];
        info->ai_protocol = protocol ? protocol
                                     : (types[i] == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
        info->ai_addrlen = sizeof(*in);
        info->ai_addr = (struct sockaddr *)in;
        if ((flags & AI_CANONNAME) && node) {
            info->ai_canonname = strdup(node);
        }
        if (last) {
            last->ai_next = info;
        } else {
            *result = info;
        }
        last = info;
    }
    return 0;
}

void freeaddrinfo(struct addrinfo *list) {
    while (list) {
        struct addrinfo *next = list->ai_next;
        free(list->ai_canonname);
        free(list);
        list = next;
    }
}

const char *gai_strerror(int error) {
    switch (error) {
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_SOCKTYPE: return "Socket type not supported";
    case EAI_SERVICE: return "Service not known";
    case EAI_MEMORY: return "Out of memory";
    default: return "Name resolution failed";
    }
}

int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_size,
                char *service, socklen_t service_size, int flags) {
    (void)flags;
    if (address->sa_family != AF_INET || length < sizeof(struct sockaddr_in)) {
        return EAI_FAMILY;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (host && host_size && !inet_ntop(AF_INET, &in->sin_addr, host, host_size)) {
        return EAI_OVERFLOW;
    }
    if (service && service_size) {
        snprintf(service, service_size, "%u", ntohs(in->sin_port));
    }
    return 0;
}
