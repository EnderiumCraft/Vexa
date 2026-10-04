/* id: who this is: user, group and the other groups, by number and name.
 * `id NAME` shows an account instead. */
#include <grp.h>
#include <stdio.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

static void show(const char *label, unsigned int id, bool user) {
    struct vx_user account;
    struct group *group = user ? NULL : getgrgid(id);
    const char *name = user ? (vx_user_by_id(id, &account) == 0 ? account.name : NULL)
                            : (group ? group->gr_name : NULL);
    printf("%s%u", label, id);
    if (name) {
        printf("(%s)", name);
    }
}

int main(int argc, char **argv) {
    struct vx_credentials me;
    vx_credentials(NULL, &me);
    if (argc > 1) {
        struct vx_user user;
        if (vx_user_by_name(argv[1], &user)) {
            fprintf(stderr, "id: %s: no such account\n", argv[1]);
            return 1;
        }
        me.uid = me.euid = user.uid;
        me.gid = me.egid = user.gid;
        me.group_count = (unsigned int)vx_user_groups(user.name, me.groups, VX_GROUPS_MAX);
    }
    show("uid=", me.uid, true);
    show(" gid=", me.gid, false);
    if (me.euid != me.uid) {
        show(" euid=", me.euid, true);
    }
    if (me.egid != me.gid) {
        show(" egid=", me.egid, false);
    }
    if (me.group_count) {
        printf(" groups=");
        for (unsigned int i = 0; i < me.group_count; i++) {
            show(i ? "," : "", me.groups[i], false);
        }
    }
    printf("\n");
    return 0;
}
