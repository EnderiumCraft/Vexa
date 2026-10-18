/* What LLVM's Unix code expects from the C library and libvexa leaves out, for building LLVM
 * for Vexa (tools/build-llvm-vexa.sh includes this in every file). They're for running other
 * programs, crash handlers and file system statistics, which Mesa's llvmpipe doesn't use:
 * enough to compile, and failing cleanly if they're ever called. */
#ifndef VEXA_LLVM_COMPAT_H
#define VEXA_LLVM_COMPAT_H

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SA_ONSTACK
#define SA_ONSTACK 0
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND 0
#endif
#ifndef SA_NODEFER
#define SA_NODEFER 0
#endif
#ifndef _SC_ARG_MAX
#define _SC_ARG_MAX 0 /* (libvexa's sysconf doesn't know it: LLVM then takes _POSIX_ARG_MAX.) */
#endif
#ifndef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX 70 /* (Unknown to libvexa's sysconf: LLVM then takes a size.) */
#endif
#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
#endif
#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* No fork on Vexa (programs start with posix_spawn). */
static inline pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}
static inline pid_t getsid(pid_t pid) {
    (void)pid;
    return getpid();
}
/* No alarms: a wait with a time limit just waits. */
static inline unsigned alarm(unsigned seconds) {
    (void)seconds;
    return 0;
}

struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss;
};
static inline pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    if (usage) {
        memset(usage, 0, sizeof(*usage));
    }
    return waitpid(pid, status, options);
}

/* No hard links. */
static inline int link(const char *from, const char *to) {
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}
/* A hint: nothing to do. */
static inline int madvise(void *address, size_t length, int advice) {
    (void)address;
    (void)length;
    (void)advice;
    return 0;
}
/* The reentrant forms, on libvexa's getpwuid/getpwnam (the strings stay theirs). */
static inline int getpwuid_r(uid_t uid, struct passwd *pwd, char *buffer, size_t size,
                             struct passwd **result) {
    (void)buffer;
    (void)size;
    struct passwd *found = getpwuid(uid);
    *result = found ? (*pwd = *found, pwd) : NULL;
    return found ? 0 : ENOENT;
}
static inline int getpwnam_r(const char *name, struct passwd *pwd, char *buffer, size_t size,
                             struct passwd **result) {
    (void)buffer;
    (void)size;
    struct passwd *found = getpwnam(name);
    *result = found ? (*pwd = *found, pwd) : NULL;
    return found ? 0 : ENOENT;
}

#ifdef __cplusplus
}
#endif

#endif
