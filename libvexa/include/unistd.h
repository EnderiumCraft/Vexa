#ifndef LIBVEXA_UNISTD_H
#define LIBVEXA_UNISTD_H

/* POSIX's basic calls, over Vexa's handles: a file descriptor is a native
 * handle. There's no fork (vx_spawn starts programs), and no users. */
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PHYS_PAGES 85

#define _POSIX_VERSION 200809L
#define _POSIX_THREADS 200809L
#define _POSIX_TIMERS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L

ssize_t read(int fd, void *buffer, size_t size);
ssize_t write(int fd, const void *buffer, size_t size);
ssize_t pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
int isatty(int fd);
int pipe(int fds[2]);
int dup(int fd);
int dup2(int fd, int to);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buffer, size_t size);
int ftruncate(int fd, off_t size);
int truncate(const char *path, off_t size);
int fsync(int fd);
void sync(void);
unsigned sleep(unsigned seconds);
int usleep(useconds_t us);
pid_t getpid(void);
int nice(int increment); /* Returns the new nice value. */
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
long sysconf(int name);
int getpagesize(void);
int gethostname(char *name, size_t size);
char *getlogin(void);
__attribute__((noreturn)) void _exit(int code);
/* Vexa can't replace a process's program: these start it, wait for it and
 * end with its exit code (see spawn.h for starting programs). */
int execv(const char *path, char *const argv[]);
int execve(const char *path, char *const argv[], char *const envp[]);
int execvp(const char *file, char *const argv[]);

extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *options);
int unlinkat(int dirfd, const char *path, int flags); /* (See openat in <fcntl.h>.) */

#ifdef __cplusplus
}
#endif

#endif
