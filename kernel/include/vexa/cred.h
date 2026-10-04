#ifndef VEXA_CRED_H
#define VEXA_CRED_H

#include <stdbool.h>
#include <stdint.h>
#include <vexa/abi.h>

/* Who a process is: its user and group ids (see "Users and groups" in
 * <vexa/abi.h>). */
struct cred {
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t group_count;
    uint32_t groups[VX_GROUPS_MAX];
};

/* The calling process's credentials; the kernel's own threads are root. */
const struct cred *cred_current(void);
bool cred_in_group(const struct cred *cred, uint32_t gid);
static inline bool cred_is_root(const struct cred *cred) {
    return cred->euid == 0;
}

#endif
