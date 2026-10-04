#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Accounts from /etc/passwd (see <vexa/users.h>). The result is overwritten
 * by the next call. */
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

#ifdef __cplusplus
}
#endif

#endif
