#ifndef LIBVEXA_SYS_IOCCOM_H
#define LIBVEXA_SYS_IOCCOM_H

/* ioctl request numbers, encoded as on Linux (which Vexa's devices read):
 * the direction (bits 30-31), the argument's size (16-29), a type (8-15)
 * and a number (0-7). */
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) \
    (((dir) << 30) | ((unsigned)(size) << 16) | ((unsigned)(type) << 8) | (unsigned)(nr))
#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t) _IOC(_IOC_READ, (type), (nr), sizeof(t))
#define _IOW(type, nr, t) _IOC(_IOC_WRITE, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(t))
#define _IOC_DIR(r) (((r) >> 30) & 3)
#define _IOC_TYPE(r) (((r) >> 8) & 0xff)
#define _IOC_NR(r) ((r)&0xff)
#define _IOC_SIZE(r) (((r) >> 16) & 0x3fff)

#endif
