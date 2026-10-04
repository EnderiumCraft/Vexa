/* vauth: checks a password, for programs that can't read /etc/shadow (the
 * lock screen). Set-user-id root. Reads the account's name and the password
 * from its input, a line each; exits 0 if it's right, 1 if not. Others than
 * root may only check their own account, and a wrong guess costs a second
 * (but asking whether there's a password at all, with an empty one, doesn't). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

static bool read_line(char *out, size_t size) {
    size_t n = 0;
    char c;
    long got;
    while ((got = vx_read(0, &c, 1)) == 1 && c != '\n') {
        if (n + 1 < size) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return got == 1 || n > 0;
}

int main(void) {
    char name[64], password[256];
    if (!read_line(name, sizeof(name)) || !read_line(password, sizeof(password))) {
        return 2;
    }
    struct vx_credentials me;
    struct vx_user caller;
    if (vx_credentials(NULL, &me) || me.euid != 0) {
        return 2; /* Not installed set-user-id root. */
    }
    if (me.uid != 0 && (vx_user_by_id(me.uid, &caller) || strcmp(caller.name, name))) {
        return 1;
    }
    bool right = vx_password_check(name, password), password_given = password[0] != '\0';
    memset(password, 0, sizeof(password));
    if (!right && password_given) {
        vx_sleep(1000);
    }
    return right ? 0 : 1;
}
