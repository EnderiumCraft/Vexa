#ifndef VEXA_PERSONALITY_LINUX_SOCKETS_H
#define VEXA_PERSONALITY_LINUX_SOCKETS_H

/* The Linux subsystem's socket calls (socket.c), for linux.c's table. */

#include <stdbool.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/object.h>

#define LINUX_SOCKET_CALL(name)                                                             \
    int64_t linux_sys_##name(struct interrupt_frame *f, uint64_t a0, uint64_t a1, uint64_t a2, \
                             uint64_t a3, uint64_t a4, uint64_t a5)

LINUX_SOCKET_CALL(socket);
LINUX_SOCKET_CALL(socketpair);
LINUX_SOCKET_CALL(bind);
LINUX_SOCKET_CALL(connect);
LINUX_SOCKET_CALL(listen);
LINUX_SOCKET_CALL(accept);
LINUX_SOCKET_CALL(accept4);
LINUX_SOCKET_CALL(getsockname);
LINUX_SOCKET_CALL(getpeername);
LINUX_SOCKET_CALL(shutdown);
LINUX_SOCKET_CALL(sendto);
LINUX_SOCKET_CALL(recvfrom);
LINUX_SOCKET_CALL(sendmsg);
LINUX_SOCKET_CALL(recvmsg);
LINUX_SOCKET_CALL(sendmmsg);
LINUX_SOCKET_CALL(recvmmsg);
LINUX_SOCKET_CALL(setsockopt);
LINUX_SOCKET_CALL(getsockopt);

/* sendmsg and recvmsg with the message header in kernel memory (readv and
 * writev on sockets use them too). */
struct linux_msghdr;
int64_t linux_socket_sendmsg(uint64_t fd, const struct linux_msghdr *m, uint64_t flags);
int64_t linux_socket_recvmsg(uint64_t fd, struct linux_msghdr *m, uint64_t flags);
/* read() and write() on a socket. */
int64_t linux_socket_io(struct object *object, uint64_t buffer, uint64_t size, bool write);
/* Socket ioctls (FIONREAD, SIOCGIF*); -LE_ENOTTY if it isn't one. */
int64_t linux_socket_ioctl(struct object *object, uint64_t request, uint64_t arg);

/* A core result (negative VX_E*) as a Linux one (linux.c). */
int64_t linux_errno(int64_t result);

#endif
