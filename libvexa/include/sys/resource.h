#ifndef LIBVEXA_SYS_RESOURCE_H
#define LIBVEXA_SYS_RESOURCE_H

#include <sys/types.h>

/* Priorities: nice values, -20 (first) to 19 (last). A process group or a
 * user means this process (there's one user). */
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2


int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int value);

#endif
