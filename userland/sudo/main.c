/* sudo: runs a command as root (or, with -u, as another account), for
 * administrators: sudo [-u USER] COMMAND [ARGUMENT...]. Asks for your own
 * password first, if your account has one. Set-user-id root. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

int main(int argc, char **argv) {
    int first = 1;
    const char *target = "root";
    if (argc > 2 && !strcmp(argv[1], "-u")) {
        target = argv[2];
        first = 3;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: sudo [-u USER] COMMAND [ARGUMENT...]\n");
        return 2;
    }
    struct vx_credentials me;
    if (vx_credentials(NULL, &me) || me.euid != 0) {
        fprintf(stderr, "sudo: not installed to run as root (set-user-id)\n");
        return 1;
    }
    struct vx_user caller, user;
    if (me.uid != 0) {
        if (vx_user_by_id(me.uid, &caller) || !vx_user_is_admin(caller.name)) {
            fprintf(stderr, "sudo: only administrators may do that\n");
            return 1;
        }
        if (vx_user_has_password(caller.name)) {
            char password[256], prompt[96];
            snprintf(prompt, sizeof(prompt), "Password for %s: ", caller.name);
            bool right = vx_read_password(prompt, password, sizeof(password)) &&
                         vx_password_check(caller.name, password);
            memset(password, 0, sizeof(password));
            if (!right) {
                vx_sleep(1000);
                fprintf(stderr, "sudo: wrong password\n");
                return 1;
            }
        }
    }
    if (vx_user_by_name(target, &user)) {
        fprintf(stderr, "sudo: %s: no such account\n", target);
        return 1;
    }
    long error = vx_become_user(&user);
    if (error) {
        fprintf(stderr, "sudo: %s\n", vx_strerror(error));
        return 1;
    }
    execvp(argv[first], argv + first);
    fprintf(stderr, "sudo: %s: %s\n", argv[first], strerror(errno));
    return 127;
}
