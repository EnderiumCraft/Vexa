#ifndef LIBVEXA_SYS_IOCTL_H
#define LIBVEXA_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD 0x541b /* (Bytes waiting: not known; says 0.) */
#define FIONBIO 0x5421  /* Non-blocking on (int != 0) or off. */

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
