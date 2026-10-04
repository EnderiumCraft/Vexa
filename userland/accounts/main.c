/* accounts: lists and changes the accounts (see <vexa/users.h>).
 *   accounts                          the accounts
 *   accounts add NAME [--full "Full Name"] [--admin] [--password-stdin]
 *   accounts remove NAME [--keep-home]
 *   accounts password [NAME] [--password-stdin]
 *   accounts admin NAME yes|no
 *   accounts rename NAME "Full Name"
 * Set-user-id root: administrators may change any account; anyone may
 * change their own password (giving the old one first). With
 * --password-stdin, passwords come a line each from the input (the old one
 * first, when it's asked for) instead of being asked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

static bool from_stdin;

static bool ask(const char *prompt, char *out, size_t size) {
    if (!from_stdin) {
        return vx_read_password(prompt, out, size);
    }
    size_t n = 0;
    char c;
    long got = 0;
    while ((got = vx_read(0, &c, 1)) == 1 && c != '\n') {
        if (n + 1 < size) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return got == 1 || n > 0;
}

/* A new password, typed twice unless it comes from the input. */
static bool new_password(char *out, size_t size) {
    if (!ask("New password: ", out, size)) {
        return false;
    }
    if (from_stdin) {
        return true;
    }
    char again[256];
    bool same = ask("Again: ", again, sizeof(again)) && !strcmp(again, out);
    memset(again, 0, sizeof(again));
    if (!same) {
        fprintf(stderr, "accounts: the passwords are not the same\n");
    }
    return same;
}

static int done(long error, const char *what) {
    if (error) {
        fprintf(stderr, "accounts: %s: %s\n", what, vx_strerror(error));
        return 1;
    }
    return 0;
}

static void list(void) {
    struct vx_user users[VX_USERS_MAX];
    int n = vx_users(users, VX_USERS_MAX);
    for (int i = 0; i < n; i++) {
        printf("%-12s %5u  %-24s %s%s\n", users[i].name, users[i].uid, users[i].full_name,
               users[i].home, vx_user_is_admin(users[i].name) ? "  (administrator)" : "");
    }
}

int main(int argc, char **argv) {
    const char *full_name = "";
    bool admin = false, keep_home = false;
    const char *words[4] = {0};
    int word_count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--password-stdin")) {
            from_stdin = true;
        } else if (!strcmp(argv[i], "--admin")) {
            admin = true;
        } else if (!strcmp(argv[i], "--keep-home")) {
            keep_home = true;
        } else if (!strcmp(argv[i], "--full") && i + 1 < argc) {
            full_name = argv[++i];
        } else if (word_count < 4) {
            words[word_count++] = argv[i];
        }
    }
    if (word_count == 0 || !strcmp(words[0], "list")) {
        list();
        return 0;
    }
    struct vx_credentials me;
    if (vx_credentials(NULL, &me) || me.euid != 0) {
        fprintf(stderr, "accounts: not installed to run as root (set-user-id)\n");
        return 1;
    }
    struct vx_user caller;
    if (vx_user_by_id(me.uid, &caller)) {
        fprintf(stderr, "accounts: who is this? (user %u has no account)\n", me.uid);
        return 1;
    }
    bool may_change_others = me.uid == 0 || vx_user_is_admin(caller.name);
    const char *command = words[0], *name = words[1] ? words[1] : caller.name;

    if (!strcmp(command, "password")) {
        bool own = !strcmp(name, caller.name);
        if (!own && !may_change_others) {
            return done(-VX_EPERM, name);
        }
        char old[256] = "", password[256];
        if (own && me.uid != 0 && vx_user_has_password(name)) {
            bool right = ask("Current password: ", old, sizeof(old)) &&
                         vx_password_check(name, old);
            memset(old, 0, sizeof(old));
            if (!right) {
                vx_sleep(1000);
                fprintf(stderr, "accounts: wrong password\n");
                return 1;
            }
        }
        if (!new_password(password, sizeof(password))) {
            return 1;
        }
        long error = vx_user_set_password(name, password);
        memset(password, 0, sizeof(password));
        if (!error) {
            printf("accounts: changed %s's password\n", name);
        }
        return done(error, name);
    }
    if (!may_change_others) {
        return done(-VX_EPERM, "only administrators may change accounts");
    }
    if (!strcmp(command, "add") && words[1]) {
        char password[256] = "";
        if (from_stdin && !ask("", password, sizeof(password))) {
            password[0] = '\0';
        }
        long error = vx_user_add(name, full_name, admin, password);
        memset(password, 0, sizeof(password));
        if (!error) {
            printf("accounts: added %s\n", name);
        }
        return done(error, name);
    }
    if (!strcmp(command, "remove") && words[1]) {
        if (!strcmp(name, caller.name)) {
            return done(-VX_EBUSY, "you can't remove your own account");
        }
        long error = vx_user_remove(name, !keep_home);
        if (!error) {
            printf("accounts: removed %s\n", name);
        }
        return done(error, name);
    }
    if (!strcmp(command, "admin") && words[1] && words[2]) {
        bool yes = !strcmp(words[2], "yes");
        if (!yes && !strcmp(name, caller.name)) {
            return done(-VX_EBUSY, "you can't stop being an administrator yourself");
        }
        return done(vx_user_set_admin(name, yes), name);
    }
    if (!strcmp(command, "rename") && words[1] && words[2]) {
        return done(vx_user_set_full_name(name, words[2]), name);
    }
    fprintf(stderr, "usage: accounts [add|remove|password|admin|rename] ...\n");
    return 2;
}
