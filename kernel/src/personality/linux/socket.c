#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/mm.h>
#include <vexa/net.h>
#include <vexa/object.h>
#include <vexa/process.h>
#include <vexa/socket.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>

#include "linux.h"
#include "sockets.h"

/*
 * BSD sockets for Linux programs, mapped onto the core's sockets
 * (core/socket.h). Linux's constants and address layouts for AF_INET and
 * AF_UNIX are the same as Vexa's, so addresses pass through as they are;
 * what's left is the calling conventions: address lengths, iovecs and
 * control messages (SCM_RIGHTS: passing file descriptors), and options.
 */

/* System calls take all six arguments whether they use them or not. */
#pragma GCC diagnostic ignored "-Wunused-parameter"

#define MESSAGE_MAX (256 * 1024) /* Bytes one call moves. */
#define RIGHTS_MAX 64             /* Descriptors in one message. */

static struct process *me(void) {
    return process_current();
}

static bool user_range_ok(uint64_t address, uint64_t size) {
    return address >= USER_BASE && address + size >= address && address + size <= USER_END;
}

/* The socket behind a descriptor, with a reference. */
static struct socket *get_socket(uint64_t fd, int64_t *error) {
    uint32_t rights;
    struct object *object = handle_get_any(me()->handles, (int)fd, &rights);
    if (!object) {
        *error = -LE_EBADF;
        return NULL;
    }
    struct socket *socket = socket_of(object);
    if (!socket) {
        object_put(object);
        *error = -LE_ENOTSOCK;
    }
    return socket;
}

static void put(struct socket *socket) {
    object_put(&socket->object);
}

static int64_t install(struct socket *socket, bool close_on_exec) {
    int fd = handle_add(me()->handles, &socket->object, HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE);
    if (fd < 0) {
        put(socket);
        return linux_errno(fd);
    }
    if (close_on_exec) {
        handle_set_flags(me()->handles, fd, HANDLE_FLAG_CLOSE_ON_EXEC);
    }
    return fd;
}

static int copy_address_in(uint64_t address, uint64_t length, struct vx_socket_address *out) {
    if (length > sizeof(*out)) {
        return -LE_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    return copy_from_user(out, address, length) ? 0 : -LE_EFAULT;
}

/* Linux's convention: copy at most *length_pointer bytes, and store the
 * real length there. */
static int64_t copy_address_out(const struct vx_socket_address *address, size_t length,
                                uint64_t user_address, uint64_t user_length) {
    if (!user_address || !user_length) {
        return 0;
    }
    uint32_t room;
    if (!copy_from_user(&room, user_length, sizeof(room))) {
        return -LE_EFAULT;
    }
    if ((int32_t)room < 0) {
        return -LE_EINVAL;
    }
    size_t n = length < room ? length : room;
    uint32_t actual = (uint32_t)length;
    if (!copy_to_user(user_address, address, n) ||
        !copy_to_user(user_length, &actual, sizeof(actual))) {
        return -LE_EFAULT;
    }
    return 0;
}

static int translate_flags(uint64_t flags) {
    /* The same values; drop what has no meaning here. */
    return (int)(flags & (LINUX_MSG_PEEK | LINUX_MSG_TRUNC | LINUX_MSG_DONTWAIT |
                          LINUX_MSG_WAITALL | LINUX_MSG_NOSIGNAL));
}

int64_t linux_sys_socket(struct interrupt_frame *f, uint64_t domain, uint64_t type,
                         uint64_t protocol, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (domain != LINUX_AF_UNIX && domain != LINUX_AF_INET) {
        return -LE_EAFNOSUPPORT; /* IPv6 and netlink: callers fall back. */
    }
    struct socket *socket;
    int error = socket_create((int)domain, (int)(type & ~(uint64_t)LINUX_SOCK_CLOEXEC),
                              (int)protocol, &socket);
    if (error) {
        return linux_errno(error);
    }
    return install(socket, type & LINUX_SOCK_CLOEXEC);
}

int64_t linux_sys_socketpair(struct interrupt_frame *f, uint64_t domain, uint64_t type,
                             uint64_t protocol, uint64_t out, uint64_t a4, uint64_t a5) {
    if (!user_range_ok(out, 2 * sizeof(int))) {
        return -LE_EFAULT;
    }
    struct socket *pair[2];
    int error = socket_create_pair((int)domain, (int)(type & ~(uint64_t)LINUX_SOCK_CLOEXEC),
                                   (int)protocol, pair);
    if (error) {
        return linux_errno(error);
    }
    bool cloexec = type & LINUX_SOCK_CLOEXEC;
    int fds[2];
    int64_t first = install(pair[0], cloexec);
    if (first < 0) {
        put(pair[1]);
        return first;
    }
    int64_t second = install(pair[1], cloexec);
    if (second < 0) {
        handle_close(me()->handles, (int)first);
        return second;
    }
    fds[0] = (int)first;
    fds[1] = (int)second;
    if (!copy_to_user(out, fds, sizeof(fds))) {
        handle_close(me()->handles, fds[0]);
        handle_close(me()->handles, fds[1]);
        return -LE_EFAULT;
    }
    return 0;
}

int64_t linux_sys_bind(struct interrupt_frame *f, uint64_t fd, uint64_t address, uint64_t length,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    struct vx_socket_address a;
    int64_t error = copy_address_in(address, length, &a);
    struct socket *socket = error ? NULL : get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    error = linux_errno(socket_bind(socket, &a, length));
    put(socket);
    return error;
}

int64_t linux_sys_connect(struct interrupt_frame *f, uint64_t fd, uint64_t address,
                          uint64_t length, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct vx_socket_address a;
    int64_t error = copy_address_in(address, length, &a);
    struct socket *socket = error ? NULL : get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    error = linux_errno(socket_connect(socket, &a, length));
    put(socket);
    return error;
}

int64_t linux_sys_listen(struct interrupt_frame *f, uint64_t fd, uint64_t backlog, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    error = linux_errno(socket_listen(socket, (int)backlog));
    put(socket);
    return error;
}

int64_t linux_sys_accept4(struct interrupt_frame *f, uint64_t fd, uint64_t address,
                          uint64_t length, uint64_t flags, uint64_t a4, uint64_t a5) {
    if (flags & ~(uint64_t)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) {
        return -LE_EINVAL;
    }
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    struct socket *new_socket;
    int result = socket_accept(socket, &new_socket, (int)(flags & LINUX_SOCK_NONBLOCK));
    put(socket);
    if (result) {
        return linux_errno(result);
    }
    if (address) {
        struct vx_socket_address a;
        size_t n = 0;
        memset(&a, 0, sizeof(a));
        socket_address(new_socket, true, &a, &n);
        if ((error = copy_address_out(&a, n, address, length)) != 0) {
            put(new_socket);
            return error;
        }
    }
    return install(new_socket, flags & LINUX_SOCK_CLOEXEC);
}

int64_t linux_sys_accept(struct interrupt_frame *f, uint64_t fd, uint64_t address,
                         uint64_t length, uint64_t a3, uint64_t a4, uint64_t a5) {
    return linux_sys_accept4(f, fd, address, length, 0, 0, 0);
}

static int64_t get_name(uint64_t fd, bool peer, uint64_t address, uint64_t length) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    struct vx_socket_address a;
    memset(&a, 0, sizeof(a));
    size_t n = 0;
    error = linux_errno(socket_address(socket, peer, &a, &n));
    put(socket);
    return error ? error : copy_address_out(&a, n, address, length);
}

int64_t linux_sys_getsockname(struct interrupt_frame *f, uint64_t fd, uint64_t address,
                              uint64_t length, uint64_t a3, uint64_t a4, uint64_t a5) {
    return get_name(fd, false, address, length);
}

int64_t linux_sys_getpeername(struct interrupt_frame *f, uint64_t fd, uint64_t address,
                              uint64_t length, uint64_t a3, uint64_t a4, uint64_t a5) {
    return get_name(fd, true, address, length);
}

int64_t linux_sys_shutdown(struct interrupt_frame *f, uint64_t fd, uint64_t how, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    error = linux_errno(socket_shutdown(socket, (int)how));
    put(socket);
    return error;
}

/* ---- Sending and receiving ---- */

/* Gathers iovecs into one kernel buffer (of at most MESSAGE_MAX bytes). */
static int64_t gather(uint64_t iov, uint64_t count, uint8_t **out, size_t *size) {
    if (count > 1024) {
        return -LE_EINVAL;
    }
    struct linux_iovec *v = kmalloc(count * sizeof(*v) + 1);
    if (!v) {
        return -LE_ENOMEM;
    }
    int64_t error = copy_from_user(v, iov, count * sizeof(*v)) ? 0 : -LE_EFAULT;
    size_t total = 0;
    for (uint64_t i = 0; !error && i < count; i++) {
        /* Empty buffers may have any address (musl's fflush passes NULL). */
        if (v[i].length && !user_range_ok(v[i].base, v[i].length)) {
            error = -LE_EFAULT;
        }
        total += v[i].length;
    }
    if (total > MESSAGE_MAX) {
        total = MESSAGE_MAX;
    }
    uint8_t *buffer = error ? NULL : kmalloc(total + 1);
    if (!error && !buffer) {
        error = -LE_ENOMEM;
    }
    size_t done = 0;
    for (uint64_t i = 0; !error && i < count && done < total; i++) {
        size_t n = v[i].length < total - done ? v[i].length : total - done;
        if (n && !copy_from_user(buffer + done, v[i].base, n)) {
            error = -LE_EFAULT;
        }
        done += n;
    }
    kfree(v);
    if (error) {
        kfree(buffer);
        return error;
    }
    *out = buffer;
    *size = total;
    return 0;
}

/* Scatters `size` bytes into iovecs. */
static int64_t scatter(uint64_t iov, uint64_t count, const uint8_t *data, size_t size) {
    size_t done = 0;
    for (uint64_t i = 0; i < count && done < size; i++) {
        struct linux_iovec v;
        if (!copy_from_user(&v, iov + i * sizeof(v), sizeof(v))) {
            return -LE_EFAULT;
        }
        size_t n = v.length < size - done ? v.length : size - done;
        if (n && !copy_to_user(v.base, data + done, n)) {
            return -LE_EFAULT;
        }
        done += n;
    }
    return 0;
}

/* The total size of some iovecs. */
static int64_t iov_size(uint64_t iov, uint64_t count, size_t *size) {
    if (count > 1024) {
        return -LE_EINVAL;
    }
    *size = 0;
    for (uint64_t i = 0; i < count; i++) {
        struct linux_iovec v;
        if (!copy_from_user(&v, iov + i * sizeof(v), sizeof(v))) {
            return -LE_EFAULT;
        }
        if (v.length && !user_range_ok(v.base, v.length)) {
            return -LE_EFAULT;
        }
        *size += v.length;
    }
    if (*size > MESSAGE_MAX) {
        *size = MESSAGE_MAX;
    }
    return 0;
}

/* Reads SCM_RIGHTS descriptors from a control buffer. */
static int64_t read_rights(uint64_t control, uint64_t length, struct object **objects,
                           uint32_t *rights, int *count) {
    *count = 0;
    uint64_t at = 0;
    while (at + sizeof(struct linux_cmsghdr) <= length) {
        struct linux_cmsghdr header;
        if (!copy_from_user(&header, control + at, sizeof(header))) {
            return -LE_EFAULT;
        }
        if (header.length < sizeof(header) || at + header.length > length) {
            return -LE_EINVAL;
        }
        if (header.level == LINUX_SOL_SOCKET && header.type == LINUX_SCM_RIGHTS) {
            uint64_t n = (header.length - sizeof(header)) / sizeof(int32_t);
            for (uint64_t i = 0; i < n; i++) {
                int32_t fd;
                if (*count == RIGHTS_MAX) {
                    return -LE_EINVAL;
                }
                if (!copy_from_user(&fd, control + at + sizeof(header) + i * sizeof(fd),
                                    sizeof(fd))) {
                    return -LE_EFAULT;
                }
                struct object *object = handle_get_any(me()->handles, fd, &rights[*count]);
                if (!object) {
                    return -LE_EBADF;
                }
                objects[(*count)++] = object;
            }
        }
        /* Other kinds (SCM_CREDENTIALS...) are ignored. */
        at += (header.length + 7) & ~7ULL;
    }
    return 0;
}

int64_t linux_socket_sendmsg(uint64_t fd, const struct linux_msghdr *m, uint64_t flags) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    struct vx_socket_address address;
    uint8_t *buffer = NULL;
    size_t size = 0;
    struct object *objects[RIGHTS_MAX];
    uint32_t rights[RIGHTS_MAX];
    int count = 0;
    error = m->name ? copy_address_in(m->name, m->name_length, &address) : 0;
    if (!error) {
        error = gather(m->iov, m->iov_count, &buffer, &size);
    }
    if (!error && m->control && m->control_length) {
        error = read_rights(m->control, m->control_length, objects, rights, &count);
    }
    if (!error) {
        struct socket_message message = {
            .data = buffer,
            .size = size,
            .flags = translate_flags(flags),
            .address = m->name ? &address : NULL,
            .address_length = m->name_length,
            .objects = objects,
            .object_rights = rights,
            .object_count = count,
        };
        error = linux_errno(socket_send(socket, &message));
    }
    for (int i = 0; i < count; i++) {
        object_put(objects[i]);
    }
    kfree(buffer);
    put(socket);
    return error;
}

/* Installs received descriptors and writes the SCM_RIGHTS message. */
static int64_t write_rights(struct linux_msghdr *m, struct object **objects, uint32_t *rights,
                            int count, bool close_on_exec) {
    uint64_t room = m->control_length;
    m->control_length = 0;
    if (!count) {
        return 0;
    }
    int fits = 0;
    if (m->control && room >= sizeof(struct linux_cmsghdr) + sizeof(int32_t)) {
        fits = (int)((room - sizeof(struct linux_cmsghdr)) / sizeof(int32_t));
    }
    if (fits < count) {
        m->flags |= LINUX_MSG_CTRUNC;
    }
    int installed = 0;
    for (int i = 0; i < count; i++) {
        if (i >= fits) {
            object_put(objects[i]);
            continue;
        }
        int fd = handle_add(me()->handles, objects[i], rights[i]);
        if (fd < 0) {
            object_put(objects[i]);
            m->flags |= LINUX_MSG_CTRUNC;
            continue;
        }
        if (close_on_exec) {
            handle_set_flags(me()->handles, fd, HANDLE_FLAG_CLOSE_ON_EXEC);
        }
        int32_t value = fd;
        if (!copy_to_user(m->control + sizeof(struct linux_cmsghdr) + installed * sizeof(value),
                          &value, sizeof(value))) {
            return -LE_EFAULT;
        }
        installed++;
    }
    if (installed) {
        struct linux_cmsghdr header = {
            .length = sizeof(header) + installed * sizeof(int32_t),
            .level = LINUX_SOL_SOCKET,
            .type = LINUX_SCM_RIGHTS,
        };
        if (!copy_to_user(m->control, &header, sizeof(header))) {
            return -LE_EFAULT;
        }
        m->control_length = (header.length + 7) & ~7ULL;
    }
    return 0;
}

int64_t linux_socket_recvmsg(uint64_t fd, struct linux_msghdr *m, uint64_t flags) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    size_t size = 0;
    error = iov_size(m->iov, m->iov_count, &size);
    uint8_t *buffer = error ? NULL : kmalloc(size + 1);
    if (!error && !buffer) {
        error = -LE_ENOMEM;
    }
    struct object *objects[RIGHTS_MAX];
    uint32_t rights[RIGHTS_MAX];
    struct vx_socket_address address;
    memset(&address, 0, sizeof(address));
    struct socket_message message = {
        .data = buffer,
        .size = size,
        .flags = translate_flags(flags),
        .address = m->name ? &address : NULL,
        .objects = objects,
        .object_rights = rights,
        .object_capacity = RIGHTS_MAX,
    };
    int64_t result = error;
    if (!error) {
        result = linux_errno(socket_receive(socket, &message));
    }
    m->flags = 0;
    if (result >= 0) {
        size_t n = (size_t)result < size ? (size_t)result : size;
        if (message.flags & VX_MSG_TRUNC) {
            m->flags |= LINUX_MSG_TRUNC;
        }
        int64_t e = scatter(m->iov, m->iov_count, buffer, n);
        if (!e && m->name) {
            uint32_t room = m->name_length;
            size_t length = message.address_length;
            if (!copy_to_user(m->name, &address, length < room ? length : room)) {
                e = -LE_EFAULT;
            }
            m->name_length = (uint32_t)length;
        }
        if (!e) {
            e = write_rights(m, objects, rights, message.object_count,
                             flags & LINUX_MSG_CMSG_CLOEXEC);
        } else {
            for (int i = 0; i < message.object_count; i++) {
                object_put(objects[i]);
            }
        }
        if (e) {
            result = e;
        }
    }
    kfree(buffer);
    put(socket);
    return result;
}

int64_t linux_sys_sendto(struct interrupt_frame *f, uint64_t fd, uint64_t buffer, uint64_t size,
                         uint64_t flags, uint64_t address, uint64_t length) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    if (!user_range_ok(buffer, size)) {
        put(socket);
        return -LE_EFAULT;
    }
    struct vx_socket_address a;
    error = address ? copy_address_in(address, length, &a) : 0;
    if (size > MESSAGE_MAX) {
        size = MESSAGE_MAX;
    }
    uint8_t *data = error ? NULL : kmalloc(size + 1);
    if (!error && !data) {
        error = -LE_ENOMEM;
    }
    if (!error && !copy_from_user(data, buffer, size)) {
        error = -LE_EFAULT;
    }
    if (!error) {
        struct socket_message message = {
            .data = data,
            .size = size,
            .flags = translate_flags(flags),
            .address = address ? &a : NULL,
            .address_length = length,
        };
        error = linux_errno(socket_send(socket, &message));
    }
    kfree(data);
    put(socket);
    return error;
}

int64_t linux_sys_recvfrom(struct interrupt_frame *f, uint64_t fd, uint64_t buffer,
                           uint64_t size, uint64_t flags, uint64_t address, uint64_t length) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    if (!user_range_ok(buffer, size)) {
        put(socket);
        return -LE_EFAULT;
    }
    if (size > MESSAGE_MAX) {
        size = MESSAGE_MAX;
    }
    uint8_t *data = kmalloc(size + 1);
    struct vx_socket_address a;
    memset(&a, 0, sizeof(a));
    struct socket_message message = {
        .data = data,
        .size = size,
        .flags = translate_flags(flags),
        .address = address ? &a : NULL,
    };
    int64_t result = data ? linux_errno(socket_receive(socket, &message)) : -LE_ENOMEM;
    if (result > 0 && !copy_to_user(buffer, data, (size_t)result < size ? (size_t)result : size)) {
        result = -LE_EFAULT;
    }
    if (result >= 0 && address) {
        error = copy_address_out(&a, message.address_length, address, length);
        if (error) {
            result = error;
        }
    }
    kfree(data);
    put(socket);
    return result;
}

int64_t linux_sys_sendmsg(struct interrupt_frame *f, uint64_t fd, uint64_t user_message,
                          uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_msghdr m;
    if (!copy_from_user(&m, user_message, sizeof(m))) {
        return -LE_EFAULT;
    }
    return linux_socket_sendmsg(fd, &m, flags);
}

int64_t linux_sys_recvmsg(struct interrupt_frame *f, uint64_t fd, uint64_t user_message,
                          uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5) {
    struct linux_msghdr m;
    if (!copy_from_user(&m, user_message, sizeof(m))) {
        return -LE_EFAULT;
    }
    int64_t result = linux_socket_recvmsg(fd, &m, flags);
    if (result >= 0 && !copy_to_user(user_message, &m, sizeof(m))) {
        return -LE_EFAULT;
    }
    return result;
}

int64_t linux_sys_sendmmsg(struct interrupt_frame *f, uint64_t fd, uint64_t vector,
                           uint64_t count, uint64_t flags, uint64_t a4, uint64_t a5) {
    if (count > 1024) {
        count = 1024;
    }
    uint64_t sent = 0;
    for (; sent < count; sent++) {
        struct linux_mmsghdr m;
        uint64_t at = vector + sent * sizeof(m);
        if (!copy_from_user(&m, at, sizeof(m))) {
            return sent ? (int64_t)sent : -LE_EFAULT;
        }
        int64_t n = linux_socket_sendmsg(fd, &m.header, flags);
        if (n < 0) {
            return sent ? (int64_t)sent : n;
        }
        m.length = (uint32_t)n;
        if (!copy_to_user(at + offsetof(struct linux_mmsghdr, length), &m.length,
                          sizeof(m.length))) {
            return -LE_EFAULT;
        }
    }
    return (int64_t)sent;
}

int64_t linux_sys_recvmmsg(struct interrupt_frame *f, uint64_t fd, uint64_t vector,
                           uint64_t count, uint64_t flags, uint64_t timeout, uint64_t a5) {
    if (count > 1024) {
        count = 1024;
    }
    uint64_t received = 0;
    for (; received < count; received++) {
        struct linux_mmsghdr m;
        uint64_t at = vector + received * sizeof(m);
        if (!copy_from_user(&m, at, sizeof(m))) {
            return received ? (int64_t)received : -LE_EFAULT;
        }
        /* After the first message: only what's already there. */
        uint64_t these = received && (flags & LINUX_MSG_WAITFORONE) ? flags | LINUX_MSG_DONTWAIT
                                                                     : flags;
        int64_t n = linux_socket_recvmsg(fd, &m.header, these);
        if (n < 0) {
            return received ? (int64_t)received : n;
        }
        m.length = (uint32_t)n;
        if (!copy_to_user(at, &m, sizeof(m))) {
            return -LE_EFAULT;
        }
    }
    return (int64_t)received;
}

/* read() and write() on a socket: one receive or send (never split, so a
 * datagram stays whole). */
int64_t linux_socket_io(struct object *object, uint64_t buffer, uint64_t size, bool write) {
    struct socket *socket = socket_of(object);
    if (size > MESSAGE_MAX) {
        size = MESSAGE_MAX;
    }
    uint8_t *data = kmalloc(size + 1);
    if (!data) {
        return -LE_ENOMEM;
    }
    struct socket_message message = {.data = data, .size = size};
    int64_t result;
    if (write) {
        result = copy_from_user(data, buffer, size) ? linux_errno(socket_send(socket, &message))
                                                    : -LE_EFAULT;
    } else {
        result = linux_errno(socket_receive(socket, &message));
        if (result > 0 &&
            !copy_to_user(buffer, data, (size_t)result < size ? (size_t)result : size)) {
            result = -LE_EFAULT;
        }
    }
    kfree(data);
    return result;
}

/* ---- Options ---- */

static int64_t read_int(uint64_t value, uint64_t length, int32_t *out) {
    if (length < sizeof(int32_t)) {
        return -LE_EINVAL;
    }
    return copy_from_user(out, value, sizeof(*out)) ? 0 : -LE_EFAULT;
}

static int64_t read_timeout(uint64_t value, uint64_t length, uint64_t *ms) {
    struct linux_timeval tv;
    if (length < sizeof(tv)) {
        return -LE_EINVAL;
    }
    if (!copy_from_user(&tv, value, sizeof(tv))) {
        return -LE_EFAULT;
    }
    if (tv.sec < 0 || tv.usec < 0 || tv.usec >= 1000000) {
        return -LE_EINVAL;
    }
    *ms = (uint64_t)tv.sec * 1000 + ((uint64_t)tv.usec + 999) / 1000;
    return 0;
}

int64_t linux_sys_setsockopt(struct interrupt_frame *f, uint64_t fd, uint64_t level,
                             uint64_t name, uint64_t value, uint64_t length, uint64_t a5) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    int32_t n = 0;
    error = 0;
    if (level == LINUX_SOL_SOCKET) {
        switch (name) {
        case LINUX_SO_RCVTIMEO:
            error = read_timeout(value, length, &socket->receive_timeout_ms);
            break;
        case LINUX_SO_SNDTIMEO:
            error = read_timeout(value, length, &socket->send_timeout_ms);
            break;
        case LINUX_SO_LINGER: {
            struct linux_linger linger;
            if (length < sizeof(linger) || !copy_from_user(&linger, value, sizeof(linger))) {
                error = -LE_EINVAL;
            } else {
                socket->linger_seconds = linger.on ? linger.seconds : -1;
            }
            break;
        }
        case LINUX_SO_REUSEADDR:
        case LINUX_SO_REUSEPORT:
            if (!(error = read_int(value, length, &n))) {
                socket->reuse_address = n != 0;
            }
            break;
        case LINUX_SO_KEEPALIVE:
            if (!(error = read_int(value, length, &n))) {
                socket->keep_alive = n != 0;
            }
            break;
        case LINUX_SO_BROADCAST:
            if (!(error = read_int(value, length, &n))) {
                socket->broadcast = n != 0;
            }
            break;
        default:
            break; /* Buffer sizes, SO_PASSCRED...: accepted, nothing to do. */
        }
    } else if (level == LINUX_IPPROTO_TCP && name == LINUX_TCP_NODELAY) {
        if (!(error = read_int(value, length, &n))) {
            socket->no_delay = n != 0; /* Always true in effect: no Nagle. */
        }
    }
    /* Other IP and TCP options are accepted quietly. */
    put(socket);
    return error;
}

int64_t linux_sys_getsockopt(struct interrupt_frame *f, uint64_t fd, uint64_t level,
                             uint64_t name, uint64_t value, uint64_t length_pointer,
                             uint64_t a5) {
    int64_t error;
    struct socket *socket = get_socket(fd, &error);
    if (!socket) {
        return error;
    }
    uint32_t room;
    if (!copy_from_user(&room, length_pointer, sizeof(room))) {
        put(socket);
        return -LE_EFAULT;
    }
    union {
        int32_t n;
        struct linux_timeval tv;
        struct linux_linger linger;
        struct linux_ucred credentials;
    } out;
    memset(&out, 0, sizeof(out));
    uint32_t size = sizeof(int32_t);
    error = 0;
    if (level == LINUX_SOL_SOCKET) {
        switch (name) {
        case LINUX_SO_TYPE: out.n = socket->type; break;
        case LINUX_SO_DOMAIN: out.n = socket->family; break;
        case LINUX_SO_PROTOCOL: out.n = socket->protocol; break;
        case LINUX_SO_ERROR: out.n = (int32_t)-linux_errno(socket_take_error(socket)); break;
        case LINUX_SO_REUSEADDR:
        case LINUX_SO_REUSEPORT: out.n = socket->reuse_address; break;
        case LINUX_SO_KEEPALIVE: out.n = socket->keep_alive; break;
        case LINUX_SO_BROADCAST: out.n = socket->broadcast; break;
        case LINUX_SO_SNDBUF:
        case LINUX_SO_RCVBUF: out.n = 256 * 1024; break;
        case LINUX_SO_ACCEPTCONN: out.n = socket->listening; break;
        case LINUX_SO_RCVTIMEO:
        case LINUX_SO_SNDTIMEO: {
            uint64_t ms = name == LINUX_SO_RCVTIMEO ? socket->receive_timeout_ms
                                                    : socket->send_timeout_ms;
            out.tv.sec = (int64_t)(ms / 1000);
            out.tv.usec = (int64_t)(ms % 1000) * 1000;
            size = sizeof(out.tv);
            break;
        }
        case LINUX_SO_LINGER:
            out.linger.on = socket->linger_seconds >= 0;
            out.linger.seconds = socket->linger_seconds >= 0 ? socket->linger_seconds : 0;
            size = sizeof(out.linger);
            break;
        case LINUX_SO_PEERCRED:
            if (socket->family != VX_AF_UNIX) {
                error = -LE_ENOPROTOOPT;
            }
            out.credentials.pid = (int32_t)unix_peer_process(socket);
            size = sizeof(out.credentials);
            break;
        default:
            error = -LE_ENOPROTOOPT;
        }
    } else if (level == LINUX_IPPROTO_TCP && socket->protocol == VX_IPPROTO_TCP) {
        switch (name) {
        case LINUX_TCP_NODELAY: out.n = socket->no_delay; break;
        case LINUX_TCP_MAXSEG: out.n = NET_MTU - 40; break;
        default: error = -LE_ENOPROTOOPT;
        }
    } else {
        error = -LE_ENOPROTOOPT;
    }
    put(socket);
    if (error) {
        return error;
    }
    uint32_t n = size < room ? size : room;
    if (!copy_to_user(value, &out, n) || !copy_to_user(length_pointer, &n, sizeof(n))) {
        return -LE_EFAULT;
    }
    return 0;
}

/* ---- ioctl: interface information (ifconfig) and FIONREAD ---- */

static void fill_ifreq(struct linux_ifreq *r, const struct vx_net_interface *n, uint64_t request) {
    struct vx_inet_address *a = (struct vx_inet_address *)&r->address;
    switch (request) {
    case LINUX_SIOCGIFFLAGS:
        r->flags = (int16_t)((n->flags & VX_NET_UP ? LINUX_IFF_UP | LINUX_IFF_RUNNING : 0) |
                             (n->flags & VX_NET_LOOPBACK ? LINUX_IFF_LOOPBACK
                                                         : LINUX_IFF_BROADCAST |
                                                               LINUX_IFF_MULTICAST));
        break;
    case LINUX_SIOCGIFADDR:
    case LINUX_SIOCGIFNETMASK:
    case LINUX_SIOCGIFBRDADDR:
    case LINUX_SIOCGIFDSTADDR:
        memset(a, 0, sizeof(*a));
        a->family = VX_AF_INET;
        a->address = request == LINUX_SIOCGIFADDR      ? n->address
                     : request == LINUX_SIOCGIFNETMASK ? n->netmask
                     : request == LINUX_SIOCGIFBRDADDR ? (n->address | ~n->netmask)
                                                       : 0;
        break;
    case LINUX_SIOCGIFHWADDR:
        memset(&r->address, 0, sizeof(r->address));
        r->address.family = n->flags & VX_NET_LOOPBACK ? 772 : 1; /* ARPHRD_LOOPBACK, _ETHER */
        memcpy(r->address.data, n->mac, 6);
        break;
    case LINUX_SIOCGIFMTU: r->value = (int32_t)n->mtu; break;
    case LINUX_SIOCGIFMETRIC: r->value = 0; break;
    case LINUX_SIOCGIFTXQLEN: r->value = 1000; break;
    }
}

int64_t linux_socket_ioctl(struct object *object, uint64_t request, uint64_t arg) {
    struct socket *socket = socket_of(object);
    if (!socket) {
        return -LE_ENOTTY;
    }
    if (request == LINUX_FIONREAD) {
        int32_t n = (int32_t)socket_pending(socket);
        return copy_to_user(arg, &n, sizeof(n)) ? 0 : -LE_EFAULT;
    }
    struct vx_net_interface list[8];
    int count = net_snapshot(list, 8);
    if (count > 8) {
        count = 8;
    }
    if (request == LINUX_SIOCGIFCONF) {
        struct linux_ifconf conf;
        if (!copy_from_user(&conf, arg, sizeof(conf))) {
            return -LE_EFAULT;
        }
        int used = 0;
        for (int i = 0; i < count; i++) {
            if (!list[i].address) {
                continue;
            }
            if (conf.buffer) {
                if (used + (int)sizeof(struct linux_ifreq) > conf.length) {
                    break;
                }
                struct linux_ifreq r;
                memset(&r, 0, sizeof(r));
                memcpy(r.name, list[i].name, sizeof(list[i].name));
                fill_ifreq(&r, &list[i], LINUX_SIOCGIFADDR);
                if (!copy_to_user(conf.buffer + used, &r, sizeof(r))) {
                    return -LE_EFAULT;
                }
            }
            used += (int)sizeof(struct linux_ifreq);
        }
        conf.length = used;
        return copy_to_user(arg, &conf, sizeof(conf)) ? 0 : -LE_EFAULT;
    }
    switch (request) {
    case LINUX_SIOCGIFNAME:
    case LINUX_SIOCGIFFLAGS:
    case LINUX_SIOCGIFADDR:
    case LINUX_SIOCGIFNETMASK:
    case LINUX_SIOCGIFBRDADDR:
    case LINUX_SIOCGIFDSTADDR:
    case LINUX_SIOCGIFHWADDR:
    case LINUX_SIOCGIFMTU:
    case LINUX_SIOCGIFMETRIC:
    case LINUX_SIOCGIFTXQLEN:
    case LINUX_SIOCGIFINDEX:
        break;
    default:
        return -LE_ENOTTY;
    }
    struct linux_ifreq r;
    if (!copy_from_user(&r, arg, sizeof(r))) {
        return -LE_EFAULT;
    }
    r.name[sizeof(r.name) - 1] = '\0';
    for (int i = 0; i < count; i++) {
        bool match = request == LINUX_SIOCGIFNAME ? r.value == i + 1
                                                  : strcmp(r.name, list[i].name) == 0;
        if (!match) {
            continue;
        }
        if (request == LINUX_SIOCGIFNAME) {
            memset(r.name, 0, sizeof(r.name));
            memcpy(r.name, list[i].name, sizeof(list[i].name));
        } else if (request == LINUX_SIOCGIFINDEX) {
            r.value = i + 1;
        } else {
            fill_ifreq(&r, &list[i], request);
        }
        return copy_to_user(arg, &r, sizeof(r)) ? 0 : -LE_EFAULT;
    }
    return -LE_ENODEV;
}
