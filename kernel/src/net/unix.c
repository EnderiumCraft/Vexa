#include <vexa/abi.h>
#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/socket.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * Local sockets (VX_AF_UNIX): streams, datagrams and sequenced packets
 * between processes, named by a path in the file system (or by an abstract
 * name, starting with a NUL byte), or created in connected pairs. They can
 * carry handles along with the data (Linux: SCM_RIGHTS), which is how
 * programs hand each other open files and sockets.
 *
 * What one side sends is queued at the other as chunks; a stream reader
 * takes bytes across chunks, but stops where a chunk carrying handles begins,
 * so the handles arrive with the first byte sent with them.
 */

#define QUEUE_LIMIT (256 * 1024) /* Bytes waiting at one socket. */
#define NAME_MAX_LENGTH 108

static struct mutex unix_lock = MUTEX_INIT;

struct chunk {
    struct chunk *next;
    size_t length, offset;
    struct vx_socket_address from; /* Datagrams: the sender's name. */
    size_t from_length;
    struct object **objects;
    uint32_t *rights;
    int object_count;
    uint8_t data[];
};

enum unix_state { UNIX_UNCONNECTED, UNIX_LISTENING, UNIX_CONNECTED, UNIX_DISCONNECTED };

struct endpoint {
    struct endpoint *next; /* In `endpoints`. */
    struct socket *socket; /* NULL for a connection waiting for accept(). */
    int type;
    enum unix_state state;
    struct endpoint *peer; /* Connected peer, or a datagram socket's default destination. */
    uint32_t owner;        /* The process that made it (for SO_PEERCRED). */

    struct chunk *head, *tail;
    size_t queued;
    bool shut_read, shut_write, peer_done; /* peer_done: nothing more will come. */
    int error;

    /* The name (bound, or a server connection's listener's). */
    bool bound;
    char name[NAME_MAX_LENGTH];
    size_t name_length; /* Abstract names count their leading NUL. */
    struct file *file;  /* The file a path name is bound to. */
    struct endpoint *listener_of; /* A connection waiting to be accepted: its listener. */

    /* Listening. */
    int backlog, pending;
    struct endpoint *accept_head, *accept_tail, *accept_next;

    struct wait_queue wait;
};

static struct endpoint *endpoints;

static const struct socket_ops unix_ops;

static struct endpoint *endpoint_of(struct socket *socket) {
    return socket->data;
}

static bool is_stream(const struct endpoint *e) {
    return e->type == VX_SOCK_STREAM;
}

static uint32_t current_process_id(void) {
    struct process *process = process_current();
    return process ? process->id : 0;
}

static struct endpoint *endpoint_new(struct socket *socket, int type) {
    struct endpoint *e = kzalloc(sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->socket = socket;
    e->type = type;
    e->owner = current_process_id();
    e->next = endpoints;
    endpoints = e;
    return e;
}

/* Frees chunks and the handles they carry. Call without unix_lock: dropping
 * a handle can close another local socket. */
static void free_chunks(struct chunk *chunk) {
    while (chunk) {
        struct chunk *next = chunk->next;
        for (int i = 0; i < chunk->object_count; i++) {
            object_put(chunk->objects[i]);
        }
        kfree(chunk->objects);
        kfree(chunk->rights);
        kfree(chunk);
        chunk = next;
    }
}

/* ---- Names ---- */

/* The kernel path for a program's socket path: absolute, and as its
 * personality sees it (Linux programs' paths may live under /linux). */
static char *kernel_path(const char *path, size_t length) {
    struct process *process = process_current();
    if (!process) {
        return NULL;
    }
    char *absolute = process_absolute_path(process, path, length);
    if (absolute && process->personality && process->personality->translate_path) {
        char *translated = process->personality->translate_path(absolute);
        if (translated) {
            kfree(absolute);
            absolute = translated;
        }
    }
    return absolute;
}

/* Checks an address and extracts its name: a path (NUL-terminated in
 * `name`), or an abstract name (starting with NUL, `*length` bytes). */
static int parse_name(const struct vx_socket_address *address, size_t length, char *name,
                      size_t *name_length) {
    if (!address || length < 2 || length > sizeof(struct vx_unix_address)) {
        return -VX_EINVAL;
    }
    if (address->family != VX_AF_UNIX) {
        return -VX_EINVAL;
    }
    size_t n = length - 2;
    if (n == 0) {
        return -VX_EINVAL;
    }
    memcpy(name, address->local.path, n);
    if (name[0] != '\0') {
        /* A path: up to the first NUL. */
        size_t end = 0;
        while (end < n && name[end]) {
            end++;
        }
        if (end == NAME_MAX_LENGTH) {
            return -VX_ENAMETOOLONG;
        }
        name[end] = '\0';
        n = end;
    }
    *name_length = n;
    return 0;
}

static void make_name(const struct endpoint *e, struct vx_socket_address *address,
                      size_t *length) {
    memset(address, 0, sizeof(struct vx_unix_address));
    address->local.family = VX_AF_UNIX;
    if (!e || !e->name_length) {
        *length = 2;
        return;
    }
    memcpy(address->local.path, e->name, e->name_length);
    /* Paths include their terminating NUL, as on Linux. */
    *length = 2 + e->name_length + (e->name[0] ? 1 : 0);
    if (*length > sizeof(struct vx_unix_address)) {
        *length = sizeof(struct vx_unix_address);
    }
}

/* The bound socket with this name (a path: the file it is bound to). */
static struct endpoint *find_bound(const char *name, size_t name_length, struct file *file) {
    for (struct endpoint *e = endpoints; e; e = e->next) {
        if (!e->bound) {
            continue;
        }
        if (file ? e->file && e->file->vnode == file->vnode
                 : !e->file && e->name_length == name_length &&
                       memcmp(e->name, name, name_length) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Finds the socket an address names. */
static int lookup(const struct vx_socket_address *address, size_t length,
                  struct endpoint **out) {
    char name[NAME_MAX_LENGTH + 1];
    size_t name_length;
    int error = parse_name(address, length, name, &name_length);
    if (error) {
        return error;
    }
    struct file *file = NULL;
    if (name[0]) {
        char *path = kernel_path(name, name_length);
        if (!path) {
            return -VX_ENOMEM;
        }
        mutex_unlock(&unix_lock); /* The file system may take a while. */
        error = vfs_open(path, strlen(path), VX_OPEN_READ, &file);
        mutex_lock(&unix_lock);
        kfree(path);
        if (error) {
            return error;
        }
    }
    struct endpoint *e = find_bound(name, name_length, file);
    if (file) {
        mutex_unlock(&unix_lock);
        vfs_close(file);
        mutex_lock(&unix_lock);
    }
    if (!e) {
        return -VX_ECONNREFUSED;
    }
    *out = e;
    return 0;
}

static int unix_bind(struct socket *socket, const struct vx_socket_address *address,
                     size_t length) {
    char name[NAME_MAX_LENGTH + 1];
    size_t name_length;
    int error = parse_name(address, length, name, &name_length);
    if (error) {
        return error;
    }
    struct file *file = NULL;
    if (name[0]) {
        /* A path: the socket appears in the file system. */
        char *path = kernel_path(name, name_length);
        if (!path) {
            return -VX_ENOMEM;
        }
        struct vx_stat stat;
        if (vfs_lstat(path, strlen(path), &stat) == 0) {
            error = -VX_EADDRINUSE;
        } else {
            error = vfs_open(path, strlen(path), VX_OPEN_READ | VX_OPEN_WRITE | VX_OPEN_CREATE,
                             &file);
        }
        kfree(path);
        if (error) {
            return error;
        }
    }
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    if (e->bound) {
        error = -VX_EINVAL;
    } else if (!file && find_bound(name, name_length, NULL)) {
        error = -VX_EADDRINUSE;
    } else {
        memcpy(e->name, name, name_length);
        e->name_length = name_length;
        e->file = file;
        e->bound = true;
        file = NULL;
    }
    mutex_unlock(&unix_lock);
    if (file) {
        vfs_close(file);
    }
    return error;
}

static int unix_listen(struct socket *socket, int backlog) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int error = 0;
    if (e->type == VX_SOCK_DGRAM) {
        error = -VX_EOPNOTSUPP;
    } else if (!e->bound || (e->state != UNIX_UNCONNECTED && e->state != UNIX_LISTENING)) {
        error = -VX_EINVAL;
    } else {
        e->state = UNIX_LISTENING;
        e->backlog = backlog;
        wait_queue_wake_all(&e->wait); /* Connectors waiting for room. */
    }
    mutex_unlock(&unix_lock);
    return error;
}

static void connect_pair(struct endpoint *a, struct endpoint *b) {
    a->peer = b;
    b->peer = a;
    a->state = b->state = UNIX_CONNECTED;
}

static bool listener_has_room(void *arg) {
    struct endpoint *listener = arg;
    return listener->state != UNIX_LISTENING || listener->pending < listener->backlog;
}

static int unix_connect(struct socket *socket, const struct vx_socket_address *address,
                        size_t length) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    struct endpoint *target = NULL;
    int error = 0;
    if (e->type == VX_SOCK_DGRAM) {
        /* Just a default destination (AF_UNSPEC forgets it). */
        if (address && length >= 2 && address->family == 0) {
            e->peer = NULL;
        } else if (!(error = lookup(address, length, &target))) {
            e->peer = target;
        }
        goto out;
    }
    if (e->state == UNIX_CONNECTED) {
        error = -VX_EISCONN;
        goto out;
    }
    if (e->state != UNIX_UNCONNECTED) {
        error = -VX_EINVAL;
        goto out;
    }
    if ((error = lookup(address, length, &target)) != 0) {
        goto out;
    }
    if (target->state != UNIX_LISTENING || target->type != e->type) {
        error = target->type != e->type ? -VX_EPROTONOSUPPORT : -VX_ECONNREFUSED;
        goto out;
    }
    /* Wait for room in the listener's queue. */
    error = socket_wait(socket, &unix_lock, &target->wait, listener_has_room, target, 0, true);
    if (error) {
        goto out;
    }
    if (target->state != UNIX_LISTENING || e->state != UNIX_UNCONNECTED) {
        error = -VX_ECONNREFUSED;
        goto out;
    }
    /* The listener's side of the connection, until accept() gives it a socket. */
    struct endpoint *server = endpoint_new(NULL, e->type);
    if (!server) {
        error = -VX_ENOMEM;
        goto out;
    }
    memcpy(server->name, target->name, target->name_length);
    server->name_length = target->name_length;
    server->owner = target->owner;
    connect_pair(e, server);
    server->listener_of = target;
    if (target->accept_tail) {
        target->accept_tail->accept_next = server;
    } else {
        target->accept_head = server;
    }
    target->accept_tail = server;
    target->pending++;
    wait_queue_wake_all(&target->wait);
out:
    mutex_unlock(&unix_lock);
    return error;
}

static bool accept_ready(void *arg) {
    struct endpoint *e = arg;
    return e->accept_head || e->state != UNIX_LISTENING;
}

static int unix_accept(struct socket *socket, struct socket **out) {
    struct socket *new_socket = kzalloc(sizeof(*new_socket));
    if (!new_socket) {
        return -VX_ENOMEM;
    }
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int error = e->state == UNIX_LISTENING ? 0 : -VX_EINVAL;
    if (!error) {
        error = socket_wait(socket, &unix_lock, &e->wait, accept_ready, e, 0, false);
    }
    if (!error && e->state != UNIX_LISTENING) {
        error = -VX_EINVAL;
    }
    if (!error) {
        struct endpoint *server = e->accept_head;
        e->accept_head = server->accept_next;
        if (!e->accept_head) {
            e->accept_tail = NULL;
        }
        e->pending--;
        server->accept_next = NULL;
        server->listener_of = NULL;
        object_init(&new_socket->object, &socket_object_type);
        new_socket->family = VX_AF_UNIX;
        new_socket->type = server->type;
        new_socket->linger_seconds = -1;
        new_socket->ops = &unix_ops;
        new_socket->data = server;
        server->socket = new_socket;
        *out = new_socket;
        wait_queue_wake_all(&e->wait); /* Room for another connection. */
    }
    mutex_unlock(&unix_lock);
    if (error) {
        kfree(new_socket);
    }
    return error;
}

/* ---- Sending and receiving ---- */

struct send_wait {
    struct endpoint *self, *target;
    size_t need;
};

static bool can_send(void *arg) {
    struct send_wait *w = arg;
    struct endpoint *target = is_stream(w->self) || w->self->type == VX_SOCK_SEQPACKET
                                  ? w->self->peer
                                  : w->target;
    return !target || target->shut_read || w->self->shut_write ||
           target->queued + w->need <= QUEUE_LIMIT || target->queued == 0;
}

static int64_t unix_send(struct socket *socket, struct socket_message *message) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int64_t result = 0;
    size_t done = 0;
    struct endpoint *target = NULL;
    bool connection = e->type != VX_SOCK_DGRAM;
    if (e->shut_write) {
        result = -VX_EPIPE;
        goto out;
    }
    if (connection) {
        if (message->address && e->state != UNIX_CONNECTED) {
            result = -VX_EOPNOTSUPP;
            goto out;
        }
        if (e->state != UNIX_CONNECTED) {
            result = e->state == UNIX_DISCONNECTED ? -VX_EPIPE : -VX_ENOTCONN;
            goto out;
        }
    } else if (message->address) {
        if ((result = lookup(message->address, message->address_length, &target)) != 0) {
            goto out;
        }
    } else if (!(target = e->peer)) {
        result = e->error ? e->error : -VX_ENOTCONN;
        goto out;
    }
    if (!connection && message->size > QUEUE_LIMIT) {
        result = -VX_EMSGSIZE;
        goto out;
    }
    bool objects_sent = false;
    do {
        struct send_wait w = {e, target, 1};
        if (!connection || e->type == VX_SOCK_SEQPACKET) {
            w.need = message->size;
        }
        int error = socket_wait(socket, &unix_lock, &e->wait, can_send, &w,
                                done ? message->flags | VX_MSG_DONTWAIT : message->flags, true);
        if (error) {
            result = done ? 0 : error;
            break;
        }
        struct endpoint *to = connection ? e->peer : target;
        if (!to || to->shut_read || e->shut_write || (connection && e->state != UNIX_CONNECTED)) {
            result = done ? 0 : -VX_EPIPE;
            if (!connection && !done) {
                result = -VX_ECONNREFUSED;
            }
            break;
        }
        size_t n = message->size - done;
        if (is_stream(e)) {
            size_t room = to->queued < QUEUE_LIMIT ? QUEUE_LIMIT - to->queued : 0;
            if (n > room) {
                n = room ? room : (n < 4096 ? n : 4096);
            }
        }
        struct chunk *chunk = kmalloc(sizeof(*chunk) + n);
        if (!chunk) {
            result = done ? 0 : -VX_ENOMEM;
            break;
        }
        memset(chunk, 0, sizeof(*chunk));
        chunk->length = n;
        memcpy(chunk->data, (const uint8_t *)message->data + done, n);
        if (!connection) {
            make_name(e->bound ? e : NULL, &chunk->from, &chunk->from_length);
        }
        if (!objects_sent && message->object_count) {
            chunk->objects = kmalloc(message->object_count * sizeof(struct object *));
            chunk->rights = kmalloc(message->object_count * sizeof(uint32_t));
            if (!chunk->objects || !chunk->rights) {
                kfree(chunk->objects);
                kfree(chunk->rights);
                kfree(chunk);
                result = done ? 0 : -VX_ENOMEM;
                break;
            }
            for (int i = 0; i < message->object_count; i++) {
                object_ref(message->objects[i]);
                chunk->objects[i] = message->objects[i];
                chunk->rights[i] = message->object_rights[i];
            }
            chunk->object_count = message->object_count;
            objects_sent = true;
        }
        if (to->tail) {
            to->tail->next = chunk;
        } else {
            to->head = chunk;
        }
        to->tail = chunk;
        to->queued += n;
        done += n;
        wait_queue_wake_all(&to->wait);
    } while (done < message->size);
    if (done || message->size == 0) {
        result = result < 0 && !done ? result : (int64_t)done;
    }
out:
    mutex_unlock(&unix_lock);
    return result;
}

static bool can_receive(void *arg) {
    struct endpoint *e = arg;
    return e->head || e->shut_read || e->peer_done || e->state == UNIX_DISCONNECTED;
}

/* Hands a chunk's handles to the receiver (as many as it has room for). */
static void take_objects(struct chunk *chunk, struct socket_message *message,
                         struct chunk **garbage) {
    int n = chunk->object_count;
    for (int i = 0; i < n; i++) {
        if (message->objects && message->object_count < message->object_capacity) {
            message->objects[message->object_count] = chunk->objects[i];
            message->object_rights[message->object_count] = chunk->rights[i];
            message->object_count++;
        } else {
            /* No room: dropped (Linux sets MSG_CTRUNC). Put it with the
             * garbage so it's released without the lock. */
            struct chunk *g = kzalloc(sizeof(*g));
            struct object **one = kmalloc(sizeof(*one));
            if (g && one) {
                one[0] = chunk->objects[i];
                g->objects = one;
                g->object_count = 1;
                g->next = *garbage;
                *garbage = g;
            } else {
                kfree(g);
                kfree(one);
            }
        }
    }
    chunk->object_count = 0;
}

static int64_t unix_receive(struct socket *socket, struct socket_message *message) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    struct chunk *garbage = NULL;
    int64_t result = 0;
    size_t done = 0;
    bool connection = e->type != VX_SOCK_DGRAM;
    bool peek = message->flags & VX_MSG_PEEK;
    message->object_count = 0;
    if (connection && e->state != UNIX_CONNECTED && e->state != UNIX_DISCONNECTED) {
        result = -VX_ENOTCONN;
        goto out;
    }
    int error = socket_wait(socket, &unix_lock, &e->wait, can_receive, e, message->flags, false);
    if (error) {
        result = error;
        goto out;
    }
    if (!is_stream(e)) {
        /* One datagram or packet. */
        struct chunk *chunk = e->head;
        if (!chunk) {
            goto out; /* The end. */
        }
        size_t n = chunk->length < message->size ? chunk->length : message->size;
        memcpy(message->data, chunk->data, n);
        done = n;
        if (n < chunk->length) {
            if (message->flags & VX_MSG_TRUNC) {
                done = chunk->length;
            }
            message->flags |= VX_MSG_TRUNC;
        } else {
            message->flags &= ~VX_MSG_TRUNC;
        }
        if (message->address) {
            if (connection) {
                make_name(e->peer, message->address, &message->address_length);
            } else {
                memcpy(message->address, &chunk->from, sizeof(chunk->from));
                message->address_length = chunk->from_length;
            }
        }
        if (!peek) {
            take_objects(chunk, message, &garbage);
            e->head = chunk->next;
            if (!e->head) {
                e->tail = NULL;
            }
            e->queued -= chunk->length;
            chunk->next = garbage;
            garbage = chunk;
            if (e->peer) {
                wait_queue_wake_all(&e->peer->wait);
            }
        }
        goto out;
    }
    /* A stream: bytes across chunks, stopping before a chunk with handles. */
    struct chunk *chunk = e->head;
    size_t peek_offset = 0;
    while (chunk && done < message->size) {
        if (done && chunk->object_count && chunk->offset == 0) {
            break;
        }
        size_t available = chunk->length - chunk->offset - (peek ? peek_offset : 0);
        size_t n = available < message->size - done ? available : message->size - done;
        memcpy((uint8_t *)message->data + done, chunk->data + chunk->offset + peek_offset, n);
        done += n;
        if (peek) {
            peek_offset += n;
            if (peek_offset + chunk->offset == chunk->length) {
                chunk = chunk->next;
                peek_offset = 0;
            }
            continue;
        }
        if (chunk->object_count) {
            take_objects(chunk, message, &garbage);
        }
        chunk->offset += n;
        e->queued -= n;
        if (chunk->offset == chunk->length) {
            e->head = chunk->next;
            if (!e->head) {
                e->tail = NULL;
            }
            chunk->next = garbage;
            garbage = chunk;
            chunk = e->head;
        }
        if (!(message->flags & VX_MSG_WAITALL) && !chunk) {
            break;
        }
        if ((message->flags & VX_MSG_WAITALL) && !chunk && done < message->size) {
            if (socket_wait(socket, &unix_lock, &e->wait, can_receive, e, message->flags,
                            false) ||
                !e->head) {
                break;
            }
            chunk = e->head;
        }
    }
    if (done && e->peer && !peek) {
        wait_queue_wake_all(&e->peer->wait); /* Room for the writer. */
    }
    if (message->address) {
        make_name(e->peer, message->address, &message->address_length);
    }
out:
    mutex_unlock(&unix_lock);
    free_chunks(garbage);
    return done ? (int64_t)done : result;
}

static int unix_shutdown(struct socket *socket, int how) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int error = 0;
    struct chunk *garbage = NULL;
    if (e->type != VX_SOCK_DGRAM && e->state != UNIX_CONNECTED) {
        error = -VX_ENOTCONN;
    } else {
        if (how != VX_SHUT_WRITE) {
            e->shut_read = true;
        }
        if (how != VX_SHUT_READ) {
            e->shut_write = true;
            if (e->peer && e->type != VX_SOCK_DGRAM) {
                e->peer->peer_done = true;
                wait_queue_wake_all(&e->peer->wait);
            }
        }
        wait_queue_wake_all(&e->wait);
    }
    mutex_unlock(&unix_lock);
    free_chunks(garbage);
    return error;
}

static int unix_address(struct socket *socket, bool peer, struct vx_socket_address *address,
                        size_t *length) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int error = 0;
    if (peer) {
        if (!e->peer || (e->type != VX_SOCK_DGRAM && e->state != UNIX_CONNECTED)) {
            error = -VX_ENOTCONN;
        } else {
            make_name(e->peer, address, length);
        }
    } else {
        make_name(e->bound || e->name_length ? e : NULL, address, length);
    }
    mutex_unlock(&unix_lock);
    return error;
}

static uint32_t unix_poll(struct socket *socket) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    uint32_t ready = 0;
    if (e->state == UNIX_LISTENING) {
        ready = e->accept_head ? OBJECT_READABLE : 0;
    } else if (e->type == VX_SOCK_DGRAM) {
        ready = (e->head || e->shut_read ? OBJECT_READABLE : 0) |
                (!e->peer || e->peer->queued < QUEUE_LIMIT ? OBJECT_WRITABLE : 0);
    } else if (e->state == UNIX_UNCONNECTED) {
        ready = OBJECT_WRITABLE | OBJECT_HANGUP;
    } else {
        if (e->head || e->shut_read || e->peer_done || e->state == UNIX_DISCONNECTED) {
            ready |= OBJECT_READABLE;
        }
        if (e->state == UNIX_DISCONNECTED || !e->peer) {
            ready |= OBJECT_HANGUP | OBJECT_WRITABLE;
        } else if (e->peer->queued < QUEUE_LIMIT && !e->shut_write) {
            ready |= OBJECT_WRITABLE;
        }
        if (e->peer_done && e->shut_write) {
            ready |= OBJECT_HANGUP;
        }
    }
    mutex_unlock(&unix_lock);
    return ready;
}

static int64_t unix_pending(struct socket *socket) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int64_t n = is_stream(e) ? (int64_t)e->queued : e->head ? (int64_t)e->head->length : 0;
    mutex_unlock(&unix_lock);
    return n;
}

static int unix_take_error(struct socket *socket) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    int error = e->error;
    e->error = 0;
    mutex_unlock(&unix_lock);
    return error;
}

/* The endpoint is going away (its socket was closed, or it was never
 * accepted): disconnects everyone that points at it. With unix_lock held;
 * adds what must be freed without the lock to *garbage. */
static void endpoint_destroy(struct endpoint *e, struct chunk **garbage, struct file **file) {
    struct endpoint **p = &endpoints;
    while (*p && *p != e) {
        p = &(*p)->next;
    }
    if (*p) {
        *p = e->next;
    }
    for (struct endpoint *other = endpoints; other; other = other->next) {
        if (other->peer == e) {
            other->peer = NULL;
            if (other->type == VX_SOCK_DGRAM) {
                other->error = -VX_ECONNREFUSED;
            } else {
                other->state = UNIX_DISCONNECTED;
                other->peer_done = true;
            }
            wait_queue_wake_all(&other->wait);
        }
    }
    if (e->tail) {
        e->tail->next = *garbage;
        *garbage = e->head;
    }
    *file = e->file;
    kfree(e);
}

static void unix_release(struct socket *socket) {
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_of(socket);
    struct chunk *garbage = NULL;
    struct file *files[2] = {NULL, NULL};
    /* Connections nobody accepted go too. */
    struct endpoint *next;
    for (struct endpoint *c = e->accept_head; c; c = next) {
        next = c->accept_next;
        struct file *unused;
        endpoint_destroy(c, &garbage, &unused);
    }
    e->state = UNIX_DISCONNECTED;
    wait_queue_wake_all(&e->wait);
    endpoint_destroy(e, &garbage, &files[0]);
    mutex_unlock(&unix_lock);
    free_chunks(garbage);
    if (files[0]) {
        vfs_close(files[0]); /* The name stays in the file system, as on Linux. */
    }
}

static const struct socket_ops unix_ops = {
    .bind = unix_bind,
    .connect = unix_connect,
    .listen = unix_listen,
    .accept = unix_accept,
    .send = unix_send,
    .receive = unix_receive,
    .shutdown = unix_shutdown,
    .address = unix_address,
    .poll = unix_poll,
    .pending = unix_pending,
    .take_error = unix_take_error,
    .release = unix_release,
};

static int check_type(struct socket *socket) {
    if (socket->type != VX_SOCK_STREAM && socket->type != VX_SOCK_DGRAM &&
        socket->type != VX_SOCK_SEQPACKET) {
        return -VX_EPROTONOSUPPORT;
    }
    return socket->protocol == 0 ? 0 : -VX_EPROTONOSUPPORT;
}

int unix_create(struct socket *socket) {
    int error = check_type(socket);
    if (error) {
        return error;
    }
    mutex_lock(&unix_lock);
    struct endpoint *e = endpoint_new(socket, socket->type);
    mutex_unlock(&unix_lock);
    if (!e) {
        return -VX_ENOMEM;
    }
    socket->data = e;
    socket->ops = &unix_ops;
    return 0;
}

int unix_create_pair(struct socket *a, struct socket *b) {
    int error = check_type(a);
    if (error) {
        return error;
    }
    mutex_lock(&unix_lock);
    struct endpoint *ea = endpoint_new(a, a->type);
    struct endpoint *eb = ea ? endpoint_new(b, b->type) : NULL;
    if (!eb) {
        if (ea) {
            struct chunk *garbage = NULL;
            struct file *file;
            endpoint_destroy(ea, &garbage, &file);
        }
        mutex_unlock(&unix_lock);
        return -VX_ENOMEM;
    }
    connect_pair(ea, eb);
    mutex_unlock(&unix_lock);
    a->data = ea;
    a->ops = &unix_ops;
    b->data = eb;
    b->ops = &unix_ops;
    return 0;
}

uint32_t unix_peer_process(struct socket *socket) {
    mutex_lock(&unix_lock);
    struct endpoint *peer = endpoint_of(socket)->peer;
    uint32_t id = peer ? peer->owner : 0;
    mutex_unlock(&unix_lock);
    return id;
}
