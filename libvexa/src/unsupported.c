/* Process and file calls that Vexa does not have. Each one fails with ENOSYS, so
 * a program that tries one gets an error it can handle. Rust's std (its process and
 * file modules) references them, and a static link pulls them in with the rest. */
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

pid_t setsid(void) {
    errno = ENOSYS;
    return -1;
}

int setpgid(pid_t process, pid_t group) {
    (void)process;
    (void)group;
    errno = ENOSYS;
    return -1;
}

int chroot(const char *path) {
    (void)path;
    errno = ENOSYS;
    return -1;
}

int mkfifo(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int linkat(int from_fd, const char *from, int to_fd, const char *to, int flags) {
    (void)from_fd;
    (void)from;
    (void)to_fd;
    (void)to;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

DIR *fdopendir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return NULL;
}
