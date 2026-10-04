#ifndef _GRP_H
#define _GRP_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Groups from /etc/group. The result is overwritten by the next call. */
struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int setgroups(size_t size, const gid_t *list);

#ifdef __cplusplus
}
#endif

#endif
