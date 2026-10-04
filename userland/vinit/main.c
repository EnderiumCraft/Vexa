/*
 * vinit: the first user program, started by the kernel as process 1.
 *
 * Puts back the settings kept on disk (see <vexa/settings.h>) and the
 * computer's name, makes each account's home folder (/home/NAME, with
 * Desktop, Documents, Pictures...: /home is on that disk too, when there is
 * one, so what's in them stays), shows the welcome message, then starts the
 * desktop: from the boot CD (no root= on the kernel's command line) with
 * the Installer open. The desktop logs someone in; when they log out, it
 * starts again. When the desktop is left (or can't start, or the command
 * line says `console`), it keeps a shell (vsh) running on the console: if
 * the shell exits, it starts a new one. Once an account has a password, the
 * console asks who's there first (see <vexa/users.h>).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/desktop.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

static void show(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        fputs(line, stdout);
    }
    fclose(file);
}

/* Whether the kernel's command line has `word` (whole, or as `word=...`). */
static int cmdline_has(const char *word) {
    char line[512];
    FILE *file = fopen("/proc/cmdline", "r");
    if (!file) {
        return 0;
    }
    int found = 0;
    if (fgets(line, sizeof(line), file)) {
        size_t n = strlen(word);
        for (char *p = strtok(line, " \n"); p && !found; p = strtok(NULL, " \n")) {
            found = strncmp(p, word, n) == 0 && (p[n] == '\0' || p[n] == '=');
        }
    }
    fclose(file);
    return found;
}

/* Each account's home folder, with the usual folders in it: theirs. */
static void make_homes(void) {
    vx_mkdir("/home");
    struct vx_user users[VX_USERS_MAX];
    int n = vx_users(users, VX_USERS_MAX);
    static const char *const folders[] = {"", "/Desktop", "/Documents", "/Pictures", "/Music",
                                          "/Videos"};
    for (int i = 0; i < n; i++) {
        if (strncmp(users[i].home, "/home/", 6)) {
            continue;
        }
        for (size_t f = 0; f < sizeof(folders) / sizeof(folders[0]); f++) {
            char path[200];
            snprintf(path, sizeof(path), "%s%s", users[i].home, folders[f]);
            if (vx_mkdir(path) == 0) {
                vx_chown(path, users[i].uid, users[i].gid, 0);
            }
        }
    }
}

/* Whether any account has a password: then the console asks for one too. */
static bool passwords_set(void) {
    struct vx_user users[VX_USERS_MAX];
    int n = vx_users(users, VX_USERS_MAX);
    for (int i = 0; i < n; i++) {
        if (vx_user_has_password(users[i].name)) {
            return true;
        }
    }
    return vx_user_has_password("root");
}

/* The console's "login:": an account and its password; true once someone
 * got in (and *user is them). */
static bool console_login(struct vx_user *user) {
    char name[64] = "", password[256];
    char host[80] = "vexa";
    vx_get_hostname(host, sizeof(host));
    printf("\n%s login: ", host);
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin)) {
        return false;
    }
    name[strcspn(name, "\r\n")] = '\0';
    if (!name[0]) {
        return false;
    }
    bool right = vx_read_password("Password: ", password, sizeof(password)) &&
                 vx_user_by_name(name, user) == 0 && vx_password_check(name, password) &&
                 (user->uid != 0 || vx_user_has_password("root"));
    memset(password, 0, sizeof(password));
    if (!right) {
        vx_sleep(1000);
        printf("Login incorrect\n");
    }
    return right;
}

/* Starts the shell as `user`. vinit can't give up root itself, so `sudo -u`
 * (which, run by root, asks nothing) does it. */
static int start_shell_as(const struct vx_user *user, struct vx_spawn *spawn) {
    const char *argv[] = {"sudo", "-u", user->name, "/bin/vsh"};
    spawn->argv = argv;
    spawn->argc = 4;
    int shell = vx_spawn("/bin/sudo", spawn);
    printf("vinit: %s logged in on the console\n", user->name);
    return shell;
}

/* The desktop, until it's left (logging out starts it again); false if it
 * couldn't start. */
static int run_desktop(const char *const *envp, size_t envc) {
    const char *installer[] = {"desktop", "--boot", "/bin/installer"};
    const char *plain[] = {"desktop", "--boot"};
    int live = !cmdline_has("root"); /* (From the CD: nothing installed is running.) */
again:;
    struct vx_spawn spawn = {
        .argv = live ? installer : plain, .argc = live ? 3 : 2, .envp = envp, .envc = envc,
        .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
    };
    int desktop = vx_spawn("/bin/desktop", &spawn);
    if (desktop < 0) {
        printf("vinit: cannot start the desktop: %s\n", vx_strerror(desktop));
        return 0;
    }
    long code = vx_wait(desktop, 0);
    vx_close(desktop);
    if (code == DESKTOP_EXIT_LOGOUT) {
        live = 0; /* (The Installer opens the first time only.) */
        goto again;
    }
    if (code != 0) {
        printf("vinit: the desktop stopped (code %ld)\n", code);
    }
    return 1;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    /* Ctrl+C is for the programs the shell runs, never for vinit. */
    vx_signal(VX_SIGINT, VX_SIGNAL_IGNORE);
    vx_signal(VX_SIGQUIT, VX_SIGNAL_IGNORE);
    int restored = vx_settings_restore();
    char disk[128];
    if (restored && vx_settings_disk(disk, sizeof(disk))) {
        printf("vinit: %d settings file%s from %s\n", restored, restored == 1 ? "" : "s", disk);
    }
    FILE *name = fopen("/etc/hostname", "r");
    char line[80];
    if (name && fgets(line, sizeof(line), name)) {
        line[strcspn(line, "\r\n")] = '\0';
        vx_set_hostname(line);
    }
    if (name) {
        fclose(name);
    }
    /* The home folders: on the disk that keeps settings, or in memory. */
    if (vx_settings_disk(disk, sizeof(disk))) {
        char home[200];
        snprintf(home, sizeof(home), "%s/home", disk);
        vx_mkdir(home);
        if (vx_symlink(home, "/home") == 0) {
            printf("vinit: home folders on %s\n", disk);
        }
    }
    make_homes();
    /* Apps are installed by copying them into /apps: administrators may. */
    vx_chown(VX_APPS_DIR, VX_ID_KEEP, VX_GROUP_ADMIN, 0);
    vx_chmod(VX_APPS_DIR, 0775, 0);
    show("/etc/motd");
    fflush(stdout);

    size_t envc = 0;
    while (envp[envc]) {
        envc++;
    }
    if (!cmdline_has("console")) {
        run_desktop((const char *const *)envp, envc);
        printf("Back at the console: `desktop` starts the desktop again.\n");
        fflush(stdout);
    }
    for (;;) {
        const char *shell_argv[] = {"vsh"};
        struct vx_spawn spawn = {
            .argv = shell_argv, .argc = 1, .envp = (const char *const *)envp, .envc = envc,
            .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
        };
        int shell;
        if (passwords_set()) {
            /* Someone's shell, started by a child that becomes them (vinit
             * stays root). */
            struct vx_user user;
            if (!console_login(&user)) {
                continue;
            }
            shell = start_shell_as(&user, &spawn);
        } else {
            shell = vx_spawn("/bin/vsh", &spawn);
        }
        if (shell < 0) {
            printf("vinit: cannot start /bin/vsh: %s\n", vx_strerror(shell));
            vx_sleep(5000);
            continue;
        }
        long code = vx_wait(shell, 0);
        vx_close(shell);
        printf("vinit: the shell exited (code %ld); starting a new one\n", code);
        vx_sleep(500);
    }
}
