#ifndef VEXA_SOCKET_H
#define VEXA_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>
#include <vexa/mutex.h>
#include <vexa/object.h>
#include <vexa/sched.h>

/*
 * Sockets: objects for talking over the network (VX_AF_INET: TCP, UDP and raw
 * ICMP) or between processes (VX_AF_UNIX), shared by both personalities.
 * Addresses have the layout of struct vx_socket_address, which is also
 * Linux's. Buffers are kernel memory; the personalities copy to and from
 * user memory.
 */

struct socket;

/* One send or receive. */
struct socket_message {
    void *data;
    size_t size;
    int flags; /* VX_MSG_* (in); VX_MSG_TRUNC set on the way out if cut short. */
    /* Send: the destination (or NULL). Receive: filled with the sender, if
     * not NULL; address_length is set to its size. */
    struct vx_socket_address *address;
    size_t address_length;
    /* Handles passed along (VX_AF_UNIX only). Send: objects to pass (the
     * references stay the caller's). Receive: filled with up to
     * object_capacity objects, each with a reference for the caller. */
    struct object **objects;
    uint32_t *object_rights;
    int object_count, object_capacity;
};

struct socket_ops {
    int (*bind)(struct socket *socket, const struct vx_socket_address *address, size_t length);
    int (*connect)(struct socket *socket, const struct vx_socket_address *address,
                   size_t length);
    int (*listen)(struct socket *socket, int backlog);
    /* Returns a new socket with one reference. */
    int (*accept)(struct socket *socket, struct socket **out);
    int64_t (*send)(struct socket *socket, struct socket_message *message);
    int64_t (*receive)(struct socket *socket, struct socket_message *message);
    int (*shutdown)(struct socket *socket, int how); /* VX_SHUT_* */
    /* The socket's own address, or its peer's. */
    int (*address)(struct socket *socket, bool peer, struct vx_socket_address *address,
                   size_t *length);
    uint32_t (*poll)(struct socket *socket); /* OBJECT_* bits */
    int64_t (*pending)(struct socket *socket); /* Bytes waiting to be read. */
    int (*take_error)(struct socket *socket);  /* A pending error (cleared). */
    void (*release)(struct socket *socket);    /* The last reference is gone. */
};

struct socket {
    struct object object;
    const struct socket_ops *ops;
    int family, type, protocol;
    uint64_t receive_timeout_ms, send_timeout_ms; /* 0: wait as long as it takes. */
    bool reuse_address, keep_alive, no_delay, broadcast;
    bool listening; /* listen() succeeded. */
    int linger_seconds; /* -1: off */
    void *data; /* The protocol's. */
};

extern const struct object_type socket_object_type;

/* Returns a new socket with one reference. `type` may include
 * VX_SOCK_NONBLOCK (VX_SOCK_CLOEXEC is left to the caller). */
int socket_create(int family, int type, int protocol, struct socket **out);
int socket_create_pair(int family, int type, int protocol, struct socket *out[2]);

int socket_bind(struct socket *socket, const struct vx_socket_address *address, size_t length);
int socket_connect(struct socket *socket, const struct vx_socket_address *address,
                   size_t length);
int socket_listen(struct socket *socket, int backlog);
int socket_accept(struct socket *socket, struct socket **out, int flags);
/* Sending on a connection the other side closed also sends SIGPIPE, unless
 * VX_MSG_NOSIGNAL. */
int64_t socket_send(struct socket *socket, struct socket_message *message);
int64_t socket_receive(struct socket *socket, struct socket_message *message);
int socket_shutdown(struct socket *socket, int how);
int socket_address(struct socket *socket, bool peer, struct vx_socket_address *address,
                   size_t *length);
int64_t socket_pending(struct socket *socket);
int socket_take_error(struct socket *socket);

/* The socket behind an object, or NULL if it isn't one. */
static inline struct socket *socket_of(struct object *object) {
    return object && object->type == &socket_object_type ? (struct socket *)object : NULL;
}

/* For protocols: waits (with `lock` held on entry and on return) until
 * ready(arg), honoring non-blocking mode (VX_MSG_DONTWAIT or the socket's)
 * and the socket's timeouts. Returns 0 once ready, -VX_EAGAIN (would wait or
 * timed out) or -VX_EINTR. */
int socket_wait(struct socket *socket, struct mutex *lock, struct wait_queue *queue,
                bool (*ready)(void *arg), void *arg, int flags, bool sending);

/* The protocols. */
int inet_create(struct socket *socket);
int unix_create(struct socket *socket);
int unix_create_pair(struct socket *a, struct socket *b);
/* The process at the other end of a local socket (0 if unknown). */
uint32_t unix_peer_process(struct socket *socket);

#endif
