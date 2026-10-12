#ifndef LIBVEXA_SYS_UN_H
#define LIBVEXA_SYS_UN_H

/* UNIX domain sockets (AF_UNIX), with the Linux layout of their address. */
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

#ifdef __cplusplus
}
#endif

#endif
