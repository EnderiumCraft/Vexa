/* What LLVM's Unix code expects from the C library and libvexa leaves out, for building LLVM
 * for Vexa (tools/build-llvm-vexa.sh includes this in every file). They're for running other
 * programs, crash handlers and file system statistics, which Mesa's llvmpipe doesn't use:
 * enough to compile, and failing cleanly if they're ever called. */
#ifndef VEXA_LLVM_COMPAT_H
#define VEXA_LLVM_COMPAT_H

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
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

#ifdef __cplusplus
}
#endif

#endif
