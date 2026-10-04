#ifndef LIBVEXA_GETOPT_H
#define LIBVEXA_GETOPT_H

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Long options (--name, --name=value, --name value), as in glibc. */
struct option {
    const char *name;
    int has_arg;
    int *flag; /* If set, gets `val` (and getopt_long returns 0). */
    int val;
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

int getopt_long(int argc, char *const argv[], const char *options, const struct option *longs,
                int *index);
int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *longs, int *index);

#ifdef __cplusplus
}
#endif

#endif
