#ifndef LIBVEXA_SYS_WAIT_H
#define LIBVEXA_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1
#define WEXITSTATUS(s) (((s)&0xff00) >> 8)
#define WIFEXITED(s) (((s)&0x7f) == 0)
#define WIFSIGNALED(s) (((s)&0x7f) != 0 && ((s)&0x7f) != 0x7f)
#define WTERMSIG(s) ((s)&0x7f)

#endif
