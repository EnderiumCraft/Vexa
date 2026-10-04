#ifndef VEXA_USERS_H
#define VEXA_USERS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Accounts, in the Unix files:
 *   /etc/passwd  name:x:uid:gid:full name:home:shell   (anyone may read it)
 *   /etc/group   name:x:gid:member,member...
 *   /etc/shadow  name:password hash:                    (root only)
 * Root is user 0. People's accounts start at user 1000; members of the
 * group "admin" (10) may change the system (add accounts, install Vexa,
 * use `sudo`). An empty hash means the account has no password.
 */

#define VX_USER_FIRST 1000
#define VX_GROUP_ADMIN 10
#define VX_USERS_MAX 32
#define VX_HASH_MAX 96

struct vx_user {
    char name[32];
    unsigned int uid, gid;
    char full_name[64];
    char home[128];
    char shell[64];
};

/* 0, or -VX_ENOENT if there's no such account. */
int vx_user_by_name(const char *name, struct vx_user *out);
int vx_user_by_id(unsigned int uid, struct vx_user *out);
/* People's accounts (user 1000 and up), in /etc/passwd order: how many. */
int vx_users(struct vx_user *out, int max);
/* A group's id, or -VX_ENOENT. */
int vx_group_id(const char *name);
/* The groups `name` belongs to besides its own `gid` (up to `max`): how many. */
int vx_user_groups(const char *name, unsigned int *groups, int max);
bool vx_user_is_admin(const char *name);

/* The calling process's account (its real user), and its home folder
 * ($HOME, else the account's, else "/home"). */
int vx_current_user(struct vx_user *out);
const char *vx_home(void);
/* A path in the home folder: "$HOME/<name>" ("Documents", ".Trash"...). */
void vx_home_path(char *out, size_t size, const char *name);
/* The same, returned (one of a few buffers that take turns: copy it if it
 * has to last). */
const char *vx_home_folder(const char *name);
/* Where the Trash is: "$HOME/.Trash". */
#define VX_TRASH_NAME ".Trash"

/* Passwords: a salted, stretched SHA-256 ("$5v$salt$digest"). */
void vx_password_make(const char *password, char out[VX_HASH_MAX]);
bool vx_password_matches(const char *password, const char *hash);
/* Whether `password` is the account's (also true for "" with no password
 * set). Root reads /etc/shadow itself; others ask the set-user-id helper
 * /bin/vauth. */
bool vx_password_check(const char *name, const char *password);
/* Whether the account has a password at all (root only: false otherwise). */
bool vx_user_has_password(const char *name);

/* Asks for a password on the terminal (standard input), without showing
 * it: true if one was read (it may be empty). */
bool vx_read_password(const char *prompt, char *out, size_t size);

/* Changing accounts (root only; `accounts` does it for administrators).
 * They return 0 or a negative VX_E* error. */
int vx_user_add(const char *name, const char *full_name, bool admin, const char *password);
int vx_user_remove(const char *name, bool remove_home);
int vx_user_set_password(const char *name, const char *password);
int vx_user_set_admin(const char *name, bool admin);
int vx_user_set_full_name(const char *name, const char *full_name);
/* Becomes the account: groups, then group and user ids (all three), HOME,
 * USER, LOGNAME and SHELL. For root only (login, sudo). */
int vx_become_user(const struct vx_user *user);

#ifdef __cplusplus
}
#endif

#endif
