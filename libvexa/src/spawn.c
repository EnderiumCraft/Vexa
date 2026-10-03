/* posix_spawn, waitpid, and exec* (as near as Vexa comes: see execve). */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vexa/syscall.h>

#include "internal.h"

extern char **environ;

/* ---- File actions ---- */

enum { ACTION_OPEN, ACTION_CLOSE, ACTION_DUP2 };

struct __spawn_action {
    int kind, fd, new_fd, flags;
    mode_t mode;
    char *path;
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions) {
    memset(actions, 0, sizeof(*actions));
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions) {
    for (int i = 0; i < actions->count; i++) {
        free(actions->actions[i].path);
    }
    free(actions->actions);
    memset(actions, 0, sizeof(*actions));
    return 0;
}

static struct __spawn_action *add_action(posix_spawn_file_actions_t *actions) {
    if (actions->count == actions->capacity) {
        int capacity = actions->capacity ? actions->capacity * 2 : 4;
        struct __spawn_action *grown =
            realloc(actions->actions, (size_t)capacity * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        actions->actions = grown;
        actions->capacity = capacity;
    }
    struct __spawn_action *a = &actions->actions[actions->count++];
    memset(a, 0, sizeof(*a));
    return a;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd, const char *path,
                                     int flags, mode_t mode) {
    if (fd < 0) {
        return EBADF;
    }
    struct __spawn_action *a = add_action(actions);
    if (!a || !(a->path = strdup(path))) {
        return ENOMEM;
    }
    a->kind = ACTION_OPEN;
    a->fd = fd;
    a->flags = flags;
    a->mode = mode;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd) {
    if (fd < 0) {
        return EBADF;
    }
    struct __spawn_action *a = add_action(actions);
    if (!a) {
        return ENOMEM;
    }
    a->kind = ACTION_CLOSE;
    a->fd = fd;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int new_fd) {
    if (fd < 0 || new_fd < 0) {
        return EBADF;
    }
    struct __spawn_action *a = add_action(actions);
    if (!a) {
        return ENOMEM;
    }
    a->kind = ACTION_DUP2;
    a->fd = fd;
    a->new_fd = new_fd;
    return 0;
}

/* ---- Attributes (the process group is used; the rest is kept) ---- */

int posix_spawnattr_init(posix_spawnattr_t *a) {
    memset(a, 0, sizeof(*a));
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *a) {
    (void)a;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags) {
    a->flags = flags;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *flags) {
    *flags = a->flags;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t group) {
    a->group = group;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *a, pid_t *group) {
    *group = a->group;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *mask) {
    a->mask = *mask;
    return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *a, sigset_t *mask) {
    *mask = a->mask;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *defaults) {
    a->defaults = *defaults;
    return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *a, sigset_t *defaults) {
    *defaults = a->defaults;
    return 0;
}

/* ---- Children ---- */

#define MAX_CHILDREN 64

static struct {
    pid_t pid;
    int handle;
} children[MAX_CHILDREN];

static void remember(pid_t pid, int handle) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (!children[i].pid) {
            children[i].pid = pid;
            children[i].handle = handle;
            return;
        }
    }
    vx_close(handle); /* (Too many to keep: it can't be waited for.) */
}

/* Starts `path` with the standard handles the actions make; 0 or an errno. */
static int spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]) {
    int handles[3] = {0, 1, 2};
    int opened[16], opened_count = 0, error = 0;
    for (int i = 0; actions && i < actions->count && !error; i++) {
        const struct __spawn_action *a = &actions->actions[i];
        switch (a->kind) {
        case ACTION_OPEN: {
            int fd = open(a->path, a->flags, a->mode);
            if (fd < 0) {
                error = errno;
                break;
            }
            if (opened_count < 16) {
                opened[opened_count++] = fd;
            }
            if (a->fd < 3) {
                handles[a->fd] = fd;
            }
            break;
        }
        case ACTION_CLOSE:
            if (a->fd < 3) {
                handles[a->fd] = -1;
            }
            break;
        case ACTION_DUP2:
            if (a->new_fd < 3) {
                handles[a->new_fd] = a->fd < 3 ? handles[a->fd] : a->fd;
            }
            break;
        }
    }
    if (!error) {
        char *const *env = envp ? envp : environ;
        unsigned long argc = 0, envc = 0;
        while (argv && argv[argc]) {
            argc++;
        }
        while (env && env[envc]) {
            envc++;
        }
        struct vx_spawn s = {
            .argv = (const char *const *)argv, .argc = argc, .envp = (const char *const *)env,
            .envc = envc, .handles = {handles[0], handles[1], handles[2]},
            .flags = attributes && (attributes->flags & POSIX_SPAWN_SETPGROUP) &&
                             attributes->group == 0
                         ? VX_SPAWN_NEW_GROUP
                         : 0,
        };
        int handle = vx_spawn(path, &s);
        if (handle < 0) {
            error = __vx_errno_of(handle);
        } else {
            pid_t child = (pid_t)vx_handle_process_id(handle);
            remember(child, handle);
            if (pid) {
                *pid = child;
            }
        }
    }
    for (int i = 0; i < opened_count; i++) {
        close(opened[i]);
    }
    return error;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]) {
    return spawn(pid, path, actions, attributes, argv, envp);
}

/* `file` in each of $PATH's folders, until one is there. */
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]) {
    if (strchr(file, '/')) {
        return spawn(pid, file, actions, attributes, argv, envp);
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/bin";
    }
    int error = ENOENT;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t length = end ? (size_t)(end - path) : strlen(path);
        char full[512];
        if (length + strlen(file) + 2 <= sizeof(full)) {
            memcpy(full, path, length);
            full[length] = '/';
            strcpy(full + length + 1, file);
            if (access(full, X_OK) == 0 || access(full, F_OK) == 0) {
                return spawn(pid, full, actions, attributes, argv, envp);
            }
        }
        path += length + (end ? 1 : 0);
    }
    return error;
}

/* ---- Waiting ---- */

static int status_of(long code) {
    return (int)((code & 0xff) << 8);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    for (;;) {
        bool any = false;
        for (int i = 0; i < MAX_CHILDREN; i++) {
            if (!children[i].pid || (pid > 0 && children[i].pid != pid)) {
                continue;
            }
            any = true;
            /* One child: wait for it. Any child: check each, then again. */
            bool block = pid > 0 && !(options & WNOHANG);
            long code = vx_wait(children[i].handle, block ? 0 : VX_WAIT_NO_HANG);
            if (code == -VX_EAGAIN) {
                continue;
            }
            pid_t done = children[i].pid;
            vx_close(children[i].handle);
            children[i].pid = 0;
            if (code < 0) {
                errno = __vx_errno_of(code);
                return -1;
            }
            if (status) {
                *status = status_of(code);
            }
            return done;
        }
        if (!any) {
            errno = ECHILD;
            return -1;
        }
        if (options & WNOHANG) {
            return 0;
        }
        vx_sleep(10);
    }
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

/* ---- exec* ----
 * Vexa can't put another program in this process; the nearest thing: start
 * it with the same standard handles, wait for it, and end with its exit
 * code. (Its process id is another one, and this one stays meanwhile.) */

int execve(const char *path, char *const argv[], char *const envp[]) {
    pid_t child;
    int error = spawn(&child, path, NULL, NULL, argv, envp);
    if (error) {
        errno = error;
        return -1;
    }
    int status = 0;
    waitpid(child, &status, 0);
    _exit(WEXITSTATUS(status));
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    pid_t child;
    int error = posix_spawnp(&child, file, NULL, NULL, argv, environ);
    if (error) {
        errno = error;
        return -1;
    }
    int status = 0;
    waitpid(child, &status, 0);
    _exit(WEXITSTATUS(status));
}
