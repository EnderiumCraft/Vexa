#ifndef LIBVEXA_POLL_H
#define LIBVEXA_POLL_H

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events, revents;
};

int poll(struct pollfd *fds, nfds_t count, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
