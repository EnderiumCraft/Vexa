/* chown: a new owner and/or group: chown USER[:GROUP] FILE... (names or
 * numbers; ":GROUP" alone changes just the group). Giving a file away is
 * for root; its owner may change its group to one of their own. */
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

static bool number(const char *text, unsigned int *out) {
    char *end;
    unsigned long n = strtoul(text, &end, 10);
    *out = (unsigned int)n;
    return text[0] && !*end;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: chown USER[:GROUP] FILE...\n");
        return 2;
    }
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", argv[1]);
    char *group_name = strchr(spec, ':');
    if (group_name) {
        *group_name++ = '\0';
    }
    unsigned int uid = VX_ID_KEEP, gid = VX_ID_KEEP;
    struct vx_user user;
    if (spec[0] && !number(spec, &uid)) {
        if (vx_user_by_name(spec, &user)) {
            fprintf(stderr, "chown: %s: no such account\n", spec);
            return 1;
        }
        uid = user.uid;
    }
    if (group_name && group_name[0] && !number(group_name, &gid)) {
        struct group *group = getgrnam(group_name);
        if (!group) {
            fprintf(stderr, "chown: %s: no such group\n", group_name);
            return 1;
        }
        gid = group->gr_gid;
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        long error = vx_chown(argv[i], uid, gid, 0);
        if (error) {
            fprintf(stderr, "chown: %s: %s\n", argv[i], vx_strerror(error));
            status = 1;
        }
    }
    return status;
}
