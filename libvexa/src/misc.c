/* Signals (as far as Vexa has them), locales (one: "C"), assert, sched,
 * syslog (to standard error) and flock (not kept). */
#include <errno.h>
#include <locale.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <syslog.h>
#include <unistd.h>
#include <vexa/syscall.h>

static sighandler_t handlers[NSIG];

sighandler_t signal(int number, sighandler_t handler) {
    if (number <= 0 || number >= NSIG || number == SIGKILL || number == SIGSTOP) {
        errno = EINVAL;
        return SIG_ERR;
    }
    sighandler_t old = handlers[number];
    handlers[number] = handler;
    /* Vexa can ignore a signal or take the default action; a handler of the
     * program's own isn't called, so the default applies to it. */
    vx_signal(number, handler == SIG_IGN ? VX_SIGNAL_IGNORE : VX_SIGNAL_DEFAULT);
    return old;
}

int sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    if (number <= 0 || number >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (old) {
        old->sa_handler = handlers[number];
        old->sa_mask = 0;
        old->sa_flags = 0;
    }
    if (action) {
        signal(number, action->sa_handler);
    }
    return 0;
}

int raise(int number) {
    if (number > 0 && number < NSIG && handlers[number] != SIG_DFL &&
        handlers[number] != SIG_IGN) {
        handlers[number](number);
        return 0;
    }
    if (number > 0 && number < NSIG && handlers[number] == SIG_IGN) {
        return 0;
    }
    return vx_kill(vx_process_id(), number) ? -1 : 0;
}

int kill(pid_t pid, int number) {
    long error = vx_kill(pid, number);
    if (error) {
        errno = error == -VX_ESRCH ? ESRCH : EINVAL;
        return -1;
    }
    return 0;
}

int sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    *set = ~0ul;
    return 0;
}

int sigaddset(sigset_t *set, int number) {
    *set |= 1ul << number;
    return 0;
}

int sigdelset(sigset_t *set, int number) {
    *set &= ~(1ul << number);
    return 0;
}

int sigismember(const sigset_t *set, int number) {
    return (int)((*set >> number) & 1);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    (void)how, (void)set;
    if (old) {
        *old = 0;
    }
    return 0;
}

/* ---- Locales ---- */

char *setlocale(int category, const char *locale) {
    (void)category;
    if (!locale || !*locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX") ||
        strstr(locale, "UTF-8") || strstr(locale, "utf8")) {
        return "C";
    }
    return NULL;
}

struct lconv *localeconv(void) {
    static struct lconv c = {".", "", "", "", "", "", "", "", "", "",
                             127, 127, 127, 127, 127, 127, 127, 127};
    return &c;
}

/* ---- assert ---- */

void __assert_fail(const char *expression, const char *file, int line, const char *function) {
    fprintf(stderr, "%s:%d: %s: assertion failed: %s\n", file, line, function, expression);
    abort();
}

/* ---- sched ---- */

int sched_yield(void) {
    vx_yield();
    return 0;
}

int sched_get_priority_min(int policy) {
    (void)policy;
    return 0;
}

int sched_get_priority_max(int policy) {
    (void)policy;
    return 0;
}

/* ---- syslog: to standard error ---- */

static const char *log_ident;
static int log_options, log_mask = 0xff;

void openlog(const char *ident, int options, int facility) {
    (void)facility;
    log_ident = ident;
    log_options = options;
}

void vsyslog(int priority, const char *format, va_list args) {
    if (!(log_mask & LOG_MASK(priority & 7))) {
        return;
    }
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    size_t length = strlen(message);
    const char *ident = log_ident ? log_ident : "syslog";
    if (log_options & LOG_PID) {
        fprintf(stderr, "%s[%d]: %s%s", ident, (int)getpid(), message,
                length && message[length - 1] == '\n' ? "" : "\n");
    } else {
        fprintf(stderr, "%s: %s%s", ident, message,
                length && message[length - 1] == '\n' ? "" : "\n");
    }
}

void syslog(int priority, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsyslog(priority, format, args);
    va_end(args);
}

void closelog(void) {
    log_ident = NULL;
}

int setlogmask(int mask) {
    int old = log_mask;
    if (mask) {
        log_mask = mask;
    }
    return old;
}

/* ---- flock: Vexa keeps no file locks ---- */

int flock(int fd, int operation) {
    (void)operation;
    struct vx_stat st;
    if (vx_handle_stat(fd, &st)) {
        errno = EBADF;
        return -1;
    }
    return 0;
}
