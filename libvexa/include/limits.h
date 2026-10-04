#ifndef LIBVEXA_LIMITS_H
#define LIBVEXA_LIMITS_H

#ifdef __cplusplus
extern "C" {
#endif

#include_next <limits.h> /* The compiler's: CHAR_BIT, INT_MAX... */

#define PATH_MAX 1024
#define NAME_MAX 255
#define PIPE_BUF 4096
#define IOV_MAX 1024
#define PAGE_SIZE 4096
#define PAGESIZE 4096
#define HOST_NAME_MAX 64
#define SSIZE_MAX LONG_MAX
#define PTHREAD_KEYS_MAX 128
#define _POSIX_PATH_MAX 256
#define NL_ARGMAX 9

#ifdef __cplusplus
}
#endif

#endif
