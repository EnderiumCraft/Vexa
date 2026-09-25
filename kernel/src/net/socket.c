#include <vexa/abi.h>
#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/signal.h>
#include <vexa/socket.h>

/*
 * Sockets as objects: what every kind of socket has in common (options,
 * blocking and timeouts, SIGPIPE), dispatching the rest to the protocol.
 */

static void socket_destroy(struct object *object) {
    struct socket *socket = (struct socket *)object;
    if (socket->ops && socket->ops->release) {
        socket->ops->release(socket);
    }
    kfree(socket);
}

static int64_t socket_object_read(struct object *object, void *buffer, size_t size) {
    struct socket_message message = {.data = buffer, .size = size};
    return socket_receive((struct socket *)object, &message);
}

static int64_t socket_object_write(struct object *object, const void *buffer, size_t size) {
    struct socket_message message = {.data = (void *)buffer, .size = size};
    return socket_send((struct socket *)object, &message);
}

static uint32_t socket_object_poll(struct object *object) {
    struct socket *socket = (struct socket *)object;
    return socket->ops->poll ? socket->ops->poll(socket) : OBJECT_READABLE | OBJECT_WRITABLE;
}

const struct object_type socket_object_type = {
    .name = "socket",
    .destroy = socket_destroy,
    .read = socket_object_read,
    .write = socket_object_write,
    .poll = socket_object_poll,
};

static struct socket *socket_new(int family, int type, int protocol) {
    struct socket *socket = kzalloc(sizeof(*socket));
    if (!socket) {
        return NULL;
    }
    object_init(&socket->object, &socket_object_type);
    socket->family = family;
    socket->type = type & VX_SOCK_TYPE_MASK;
    socket->protocol = protocol;
    socket->linger_seconds = -1;
    if (type & VX_SOCK_NONBLOCK) {
        socket->object.flags |= OBJECT_NONBLOCK;
    }
    return socket;
}

int socket_create(int family, int type, int protocol, struct socket **out) {
    if (type & ~(VX_SOCK_TYPE_MASK | VX_SOCK_NONBLOCK | VX_SOCK_CLOEXEC)) {
        return -VX_EINVAL;
    }
    if (family != VX_AF_INET && family != VX_AF_UNIX) {
        return -VX_EAFNOSUPPORT;
    }
    struct socket *socket = socket_new(family, type, protocol);
    if (!socket) {
        return -VX_ENOMEM;
    }
    int error = family == VX_AF_INET ? inet_create(socket) : unix_create(socket);
    if (error) {
        kfree(socket); /* The protocol has nothing to release yet. */
        return error;
    }
    *out = socket;
    return 0;
}

int socket_create_pair(int family, int type, int protocol, struct socket *out[2]) {
    if (type & ~(VX_SOCK_TYPE_MASK | VX_SOCK_NONBLOCK | VX_SOCK_CLOEXEC)) {
        return -VX_EINVAL;
    }
    if (family != VX_AF_UNIX) {
        return family == VX_AF_INET ? -VX_EOPNOTSUPP : -VX_EAFNOSUPPORT;
    }
    struct socket *a = socket_new(family, type, protocol);
    struct socket *b = socket_new(family, type, protocol);
    int error = a && b ? unix_create_pair(a, b) : -VX_ENOMEM;
    if (error) {
        kfree(a);
        kfree(b);
        return error;
    }
    out[0] = a;
    out[1] = b;
    return 0;
}

int socket_bind(struct socket *socket, const struct vx_socket_address *address, size_t length) {
    return socket->ops->bind ? socket->ops->bind(socket, address, length) : -VX_EOPNOTSUPP;
}

int socket_connect(struct socket *socket, const struct vx_socket_address *address,
                   size_t length) {
    return socket->ops->connect ? socket->ops->connect(socket, address, length)
                                : -VX_EOPNOTSUPP;
}

int socket_listen(struct socket *socket, int backlog) {
    if (backlog <= 0) {
        backlog = 1;
    }
    if (backlog > 128) {
        backlog = 128;
    }
    int error = socket->ops->listen ? socket->ops->listen(socket, backlog) : -VX_EOPNOTSUPP;
    if (!error) {
        socket->listening = true;
    }
    return error;
}

int socket_accept(struct socket *socket, struct socket **out, int flags) {
    if (!socket->ops->accept) {
        return -VX_EOPNOTSUPP;
    }
    int error = socket->ops->accept(socket, out);
    if (!error) {
        /* The new socket inherits the listener's options, but not its
         * non-blocking mode (as on Linux): that comes from `flags`. */
        struct socket *new_socket = *out;
        new_socket->receive_timeout_ms = socket->receive_timeout_ms;
        new_socket->send_timeout_ms = socket->send_timeout_ms;
        new_socket->keep_alive = socket->keep_alive;
        new_socket->no_delay = socket->no_delay;
        if (flags & VX_SOCK_NONBLOCK) {
            new_socket->object.flags |= OBJECT_NONBLOCK;
        }
    }
    return error;
}

int64_t socket_send(struct socket *socket, struct socket_message *message) {
    if (!socket->ops->send) {
        return -VX_EOPNOTSUPP;
    }
    int64_t result = socket->ops->send(socket, message);
    if (result == -VX_EPIPE && !(message->flags & VX_MSG_NOSIGNAL)) {
        struct process *process = process_current();
        if (process) {
            signal_send(process, VX_SIGPIPE);
        }
    }
    return result;
}

int64_t socket_receive(struct socket *socket, struct socket_message *message) {
    return socket->ops->receive ? socket->ops->receive(socket, message) : -VX_EOPNOTSUPP;
}

int socket_shutdown(struct socket *socket, int how) {
    if (how != VX_SHUT_READ && how != VX_SHUT_WRITE && how != VX_SHUT_BOTH) {
        return -VX_EINVAL;
    }
    return socket->ops->shutdown ? socket->ops->shutdown(socket, how) : -VX_EOPNOTSUPP;
}

int socket_address(struct socket *socket, bool peer, struct vx_socket_address *address,
                   size_t *length) {
    return socket->ops->address ? socket->ops->address(socket, peer, address, length)
                                : -VX_EOPNOTSUPP;
}

int64_t socket_pending(struct socket *socket) {
    return socket->ops->pending ? socket->ops->pending(socket) : 0;
}

int socket_take_error(struct socket *socket) {
    return socket->ops->take_error ? socket->ops->take_error(socket) : 0;
}

int socket_wait(struct socket *socket, struct mutex *lock, struct wait_queue *queue,
                bool (*ready)(void *arg), void *arg, int flags, bool sending) {
    bool nonblocking = (flags & VX_MSG_DONTWAIT) || (socket->object.flags & OBJECT_NONBLOCK);
    uint64_t timeout = sending ? socket->send_timeout_ms : socket->receive_timeout_ms;
    while (!ready(arg)) {
        if (nonblocking) {
            return -VX_EAGAIN;
        }
        mutex_unlock(lock);
        int error = timeout ? wait_queue_wait_timeout(queue, ready, arg, timeout, true)
                            : wait_queue_wait_interruptible(queue, ready, arg);
        mutex_lock(lock);
        if (error == -VX_ETIMEDOUT) {
            return ready(arg) ? 0 : -VX_EAGAIN;
        }
        if (error) {
            return error;
        }
    }
    return 0;
}
