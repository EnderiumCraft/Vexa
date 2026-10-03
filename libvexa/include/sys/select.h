#ifndef LIBVEXA_SYS_SELECT_H
#define LIBVEXA_SYS_SELECT_H

#include <sys/time.h>
#include <sys/types.h>

#define FD_SETSIZE 1024

typedef struct {
    unsigned long bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define __FD_WORD(fd) ((fd) / (8 * (int)sizeof(unsigned long)))
#define __FD_BIT(fd) (1UL << ((fd) % (8 * (int)sizeof(unsigned long))))
#define FD_ZERO(set) __builtin_memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) ((set)->bits[__FD_WORD(fd)] |= __FD_BIT(fd))
#define FD_CLR(fd, set) ((set)->bits[__FD_WORD(fd)] &= ~__FD_BIT(fd))
#define FD_ISSET(fd, set) (((set)->bits[__FD_WORD(fd)] & __FD_BIT(fd)) != 0)

int select(int count, fd_set *read, fd_set *write, fd_set *except, struct timeval *timeout);

#endif
