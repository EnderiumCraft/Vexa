#ifndef LIBVEXA_SYS_TIME_H
#define LIBVEXA_SYS_TIME_H

#include <sys/types.h>
#include <time.h>

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct timezone {
    int tz_minuteswest, tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

/* (fd_set and select, as other systems' <sys/time.h> give them too.) */
#include <sys/select.h>

#endif
