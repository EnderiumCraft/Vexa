/*
 * vinit: the first user program, started by the kernel as process 1.
 *
 * Puts back the settings kept on disk (see <vexa/settings.h>) and the
 * computer's name, makes the home folder (/home, with Desktop, Documents
 * and Pictures: on that disk too, when there is one, so what's in them
 * stays), shows the welcome message, then keeps a shell (vsh) running on
 * the console: if the shell exits, it starts a new one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>

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
    /* The home folder: on the disk that keeps settings, or in memory. */
    if (vx_settings_disk(disk, sizeof(disk))) {
        char home[200];
        snprintf(home, sizeof(home), "%s/home", disk);
        vx_mkdir(home);
        if (vx_symlink(home, "/home") == 0) {
            printf("vinit: home folder on %s\n", disk);
        }
    }
    vx_mkdir("/home");
    static const char *const folders[] = {"/home/Desktop", "/home/Documents", "/home/Pictures",
                                          "/Trash"};
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        vx_mkdir(folders[i]);
    }
    show("/etc/motd");
    fflush(stdout);

    size_t envc = 0;
    while (envp[envc]) {
        envc++;
    }
    for (;;) {
        const char *shell_argv[] = {"vsh"};
        struct vx_spawn spawn = {
            .argv = shell_argv, .argc = 1, .envp = (const char *const *)envp, .envc = envc,
            .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
        };
        int shell = vx_spawn("/bin/vsh", &spawn);
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
