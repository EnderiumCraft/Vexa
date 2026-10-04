#ifndef LIBVEXA_SYS_SOCKET_H
#define LIBVEXA_SYS_SOCKET_H

/* BSD sockets, over Vexa's own (the same constants and address layouts as
 * Linux's): TCP and UDP over IPv4, and local sockets. */
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __data[126];
};

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct linger {
    int l_onoff, l_linger;
};

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define AF_INET6 10 /* (Not supported yet: socket() refuses it.) */
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_LOCAL
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000

#define SOL_SOCKET 1
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_DONTROUTE 0x4
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SOMAXCONN 128

int socket(int family, int type, int protocol);
int socketpair(int family, int type, int protocol, int fds[2]);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *address, socklen_t *length);
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
int connect(int fd, const struct sockaddr *address, socklen_t length);
ssize_t send(int fd, const void *buffer, size_t size, int flags);
ssize_t recv(int fd, void *buffer, size_t size, int flags);
ssize_t sendto(int fd, const void *buffer, size_t size, int flags, const struct sockaddr *to,
               socklen_t length);
ssize_t recvfrom(int fd, void *buffer, size_t size, int flags, struct sockaddr *from,
                 socklen_t *length);
ssize_t sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t recvmsg(int fd, struct msghdr *message, int flags);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *address, socklen_t *length);
int getpeername(int fd, struct sockaddr *address, socklen_t *length);
int setsockopt(int fd, int level, int name, const void *value, socklen_t length);
int getsockopt(int fd, int level, int name, void *value, socklen_t *length);

#ifdef __cplusplus
}
#endif

#endif
