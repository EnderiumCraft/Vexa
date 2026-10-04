/* hostname: shows the computer's name, or sets it: hostname [name]. Setting
 * it is for root and administrators (it's set-user-id root), and it's kept
 * in /etc/hostname for the next start. */
#include <stdio.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

int main(int argc, char **argv) {
    if (argc > 1) {
        struct vx_credentials me;
        struct vx_user user;
        vx_credentials(NULL, &me);
        if (me.uid != 0 && (vx_user_by_id(me.uid, &user) || !vx_user_is_admin(user.name))) {
            fprintf(stderr, "hostname: only administrators may name the computer\n");
            return 1;
        }
        long error = vx_set_hostname(argv[1]);
        if (error) {
            fprintf(stderr, "hostname: %s: %s (letters, digits, '-' and '.')\n", argv[1],
                    vx_strerror(error));
            return 1;
        }
        char line[96];
        int n = snprintf(line, sizeof(line), "%s\n", argv[1]);
        vx_settings_write_file("hostname", line, (size_t)n);
    }
    char name[80];
    long error = vx_get_hostname(name, sizeof(name));
    if (error) {
        fprintf(stderr, "hostname: %s\n", vx_strerror(error));
        return 1;
    }
    printf("%s\n", name);
    return 0;
}
