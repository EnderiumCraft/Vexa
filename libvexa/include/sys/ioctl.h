#ifndef LIBVEXA_SYS_IOCTL_H
#define LIBVEXA_SYS_IOCTL_H

#include <sys/ioccom.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD 0x541b /* (Bytes waiting: not known; says 0.) */
#define FIONBIO 0x5421  /* Non-blocking on (int != 0) or off. */
#define FIOCLEX 0x5451  /* Close-on-exec on: descriptors have no such flag, so a no-op, as fcntl's F_SETFD. */
#define FIONCLEX 0x5450 /* Close-on-exec off: likewise. */

/* Other requests go to the device (vx_control), with the argument's size
 * from the request (see <sys/ioccom.h>). */
int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
