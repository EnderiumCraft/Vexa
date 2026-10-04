#ifndef LIBVEXA_SPAWN_H
#define LIBVEXA_SPAWN_H

/* posix_spawn: starting another program (Vexa has no fork; this is how). Its
 * standard handles (0, 1, 2) can be opened, closed or duplicated first; the
 * new program gets only those, like every Vexa program. */
#include <signal.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSIX_SPAWN_RESETIDS 0x01
#define POSIX_SPAWN_SETPGROUP 0x02
#define POSIX_SPAWN_SETSIGDEF 0x04
#define POSIX_SPAWN_SETSIGMASK 0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER 0x20

typedef struct {
    short flags;
    pid_t group;
    sigset_t mask, defaults;
} posix_spawnattr_t;

struct __spawn_action;
typedef struct {
    int count, capacity;
    struct __spawn_action *actions;
} posix_spawn_file_actions_t;

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]);
/* The same, looking for `file` in $PATH if it has no slash. */
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attributes, char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd, const char *path,
                                     int flags, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int new_fd);

int posix_spawnattr_init(posix_spawnattr_t *attributes);
int posix_spawnattr_destroy(posix_spawnattr_t *attributes);
int posix_spawnattr_setflags(posix_spawnattr_t *attributes, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t *attributes, short *flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attributes, pid_t group);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attributes, pid_t *group);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attributes, const sigset_t *mask);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attributes, sigset_t *mask);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attributes, const sigset_t *defaults);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attributes, sigset_t *defaults);

#ifdef __cplusplus
}
#endif

#endif
