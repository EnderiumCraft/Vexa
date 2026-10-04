/* whoami: the name of the account running it (its effective user). */
#include <stdio.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

int main(void) {
    struct vx_credentials me;
    struct vx_user user;
    vx_credentials(NULL, &me);
    if (vx_user_by_id(me.euid, &user) == 0) {
        printf("%s\n", user.name);
    } else {
        printf("%u\n", me.euid);
    }
    return 0;
}
