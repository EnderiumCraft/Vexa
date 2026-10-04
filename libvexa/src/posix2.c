/* More of POSIX and its neighbours: getopt_long, uname, scandir,
 * nl_langinfo (the C locale), and the *at file calls (fstatat, unlinkat,
 * openat, mkdirat) for directories opened with opendir or open(O_DIRECTORY),
 * whose paths libvexa remembers. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <langinfo.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include "internal.h"

/* ---- Directory handles' paths ---- */

#define DIR_PATHS 64

static struct {
    int fd;
    char *path;
} dir_paths[DIR_PATHS];
static struct vx_mutex dir_lock = VX_MUTEX_INIT;

void __vx_remember_dir(int fd, const char *path) {
    char *full = realpath(path, NULL);
    if (!full) {
        full = strdup(path);
    }
    vx_mutex_lock(&dir_lock);
    for (int i = 0; i < DIR_PATHS && full; i++) {
        if (!dir_paths[i].path) {
            dir_paths[i].fd = fd;
            dir_paths[i].path = full;
            full = NULL;
        }
    }
    vx_mutex_unlock(&dir_lock);
    free(full); /* (No room: *at on this handle won't work.) */
}

void __vx_forget_dir(int fd) {
    vx_mutex_lock(&dir_lock);
    for (int i = 0; i < DIR_PATHS; i++) {
        if (dir_paths[i].path && dir_paths[i].fd == fd) {
            free(dir_paths[i].path);
            dir_paths[i].path = NULL;
        }
    }
    vx_mutex_unlock(&dir_lock);
}

/* `name` relative to `dirfd` (AT_FDCWD: the current directory) as a path. */
static int at_path(int dirfd, const char *name, char *out, size_t size) {
    if (name[0] == '/' || dirfd == AT_FDCWD) {
        if (strlen(name) >= size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(out, name);
        return 0;
    }
    int found = 0;
    vx_mutex_lock(&dir_lock);
    for (int i = 0; i < DIR_PATHS && !found; i++) {
        if (dir_paths[i].path && dir_paths[i].fd == dirfd) {
            found = snprintf(out, size, "%s/%s", dir_paths[i].path, name) < (int)size ? 1 : -1;
        }
    }
    vx_mutex_unlock(&dir_lock);
    if (found <= 0) {
        errno = found ? ENAMETOOLONG : EBADF;
        return -1;
    }
    return 0;
}

int fstatat(int dirfd, const char *name, struct stat *st, int flags) {
    char path[PATH_MAX];
    if (at_path(dirfd, name, path, sizeof(path))) {
        return -1;
    }
    return flags & AT_SYMLINK_NOFOLLOW ? lstat(path, st) : stat(path, st);
}

int unlinkat(int dirfd, const char *name, int flags) {
    char path[PATH_MAX];
    if (at_path(dirfd, name, path, sizeof(path))) {
        return -1;
    }
    return flags & AT_REMOVEDIR ? rmdir(path) : unlink(path);
}

int mkdirat(int dirfd, const char *name, mode_t mode) {
    char path[PATH_MAX];
    if (at_path(dirfd, name, path, sizeof(path))) {
        return -1;
    }
    return mkdir(path, mode);
}

int openat(int dirfd, const char *name, int flags, ...) {
    char path[PATH_MAX];
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (at_path(dirfd, name, path, sizeof(path))) {
        return -1;
    }
    return open(path, flags, mode);
}

/* ---- uname ---- */

int uname(struct utsname *u) {
    struct vx_system_info info;
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Vexa");
    if (gethostname(u->nodename, sizeof(u->nodename)) != 0) {
        strcpy(u->nodename, "vexa");
    }
    if (vx_system_info(&info) == 0) {
        snprintf(u->release, sizeof(u->release), "%.*s", (int)sizeof(info.version),
                 info.version);
    }
    strcpy(u->version, "Vexa");
    strcpy(u->machine, "x86_64");
    return 0;
}

/* ---- getopt_long: --name, --name=value, --name value ---- */

static int long_option(int argc, char *const argv[], const char *options,
                       const struct option *longs, int *index) {
    const char *arg = argv[optind] + 2, *equals = strchr(arg, '=');
    size_t n = equals ? (size_t)(equals - arg) : strlen(arg);
    int match = -1;
    for (int i = 0; longs[i].name; i++) {
        if (strncmp(longs[i].name, arg, n) == 0) {
            if (strlen(longs[i].name) == n) {
                match = i; /* Exact. */
                break;
            }
            if (match < 0) {
                match = i; /* An abbreviation. */
            }
        }
    }
    optind++;
    if (match < 0) {
        if (opterr && options[0] != ':') {
            fprintf(stderr, "%s: unknown option --%.*s\n", argv[0], (int)n, arg);
        }
        optopt = 0;
        return '?';
    }
    const struct option *o = &longs[match];
    if (index) {
        *index = match;
    }
    optarg = NULL;
    if (o->has_arg == required_argument) {
        if (equals) {
            optarg = (char *)equals + 1;
        } else if (optind < argc) {
            optarg = argv[optind++];
        } else {
            if (opterr && options[0] != ':') {
                fprintf(stderr, "%s: option --%s needs a value\n", argv[0], o->name);
            }
            optopt = o->val;
            return options[0] == ':' ? ':' : '?';
        }
    } else if (o->has_arg == optional_argument) {
        optarg = equals ? (char *)equals + 1 : NULL;
    } else if (equals) {
        if (opterr && options[0] != ':') {
            fprintf(stderr, "%s: option --%s takes no value\n", argv[0], o->name);
        }
        optopt = o->val;
        return '?';
    }
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

int getopt_long(int argc, char *const argv[], const char *options, const struct option *longs,
                int *index) {
    if (optind < argc && argv[optind] && argv[optind][0] == '-' && argv[optind][1] == '-' &&
        argv[optind][2]) {
        return long_option(argc, argv, options, longs, index);
    }
    return getopt(argc, argv, options);
}

int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *longs, int *index) {
    return getopt_long(argc, argv, options, longs, index);
}

/* ---- scandir ---- */

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *path, struct dirent ***list, int (*keep)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **)) {
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent **entries = NULL, *e;
    size_t count = 0, capacity = 0;
    while ((e = readdir(dir))) {
        if (keep && !keep(e)) {
            continue;
        }
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            struct dirent **more = realloc(entries, capacity * sizeof(*entries));
            if (!more) {
                goto fail;
            }
            entries = more;
        }
        if (!(entries[count] = malloc(sizeof(**entries)))) {
            goto fail;
        }
        memcpy(entries[count++], e, sizeof(**entries));
    }
    closedir(dir);
    if (compare && count) {
        qsort(entries, count, sizeof(*entries), (int (*)(const void *, const void *))compare);
    }
    *list = entries;
    return (int)count;
fail:
    while (count) {
        free(entries[--count]);
    }
    free(entries);
    closedir(dir);
    errno = ENOMEM;
    return -1;
}

/* ---- nl_langinfo: the C locale's (as musl's) ---- */

static const char c_time[] =
    "Sun\0" "Mon\0" "Tue\0" "Wed\0" "Thu\0" "Fri\0" "Sat\0"
    "Sunday\0" "Monday\0" "Tuesday\0" "Wednesday\0" "Thursday\0" "Friday\0" "Saturday\0"
    "Jan\0" "Feb\0" "Mar\0" "Apr\0" "May\0" "Jun\0"
    "Jul\0" "Aug\0" "Sep\0" "Oct\0" "Nov\0" "Dec\0"
    "January\0" "February\0" "March\0" "April\0" "May\0" "June\0" "July\0" "August\0"
    "September\0" "October\0" "November\0" "December\0"
    "AM\0" "PM\0"
    "%a %b %e %T %Y\0" "%m/%d/%y\0" "%H:%M:%S\0" "%I:%M:%S %p\0"
    "\0" "\0" "%m/%d/%y\0" "0123456789\0" "%a %b %e %T %Y\0" "%H:%M:%S";
static const char c_messages[] = "^[yY]\0" "^[nN]\0" "yes\0" "no";
static const char c_numeric[] = ".\0" "";

char *nl_langinfo(nl_item item) {
    int category = item >> 16, index = item & 0xffff;
    const char *s;
    if (item == CODESET) {
        return "UTF-8";
    }
    switch (category) {
    case 1: /* LC_NUMERIC */
        if (index > 1) {
            return "";
        }
        s = c_numeric;
        break;
    case 2: /* LC_TIME */
        if (index > 0x31) {
            return "";
        }
        s = c_time;
        break;
    case 5: /* LC_MESSAGES */
        if (index > 3) {
            return "";
        }
        s = c_messages;
        break;
    default:
        return "";
    }
    for (; index; index--) {
        s += strlen(s) + 1;
    }
    return (char *)s;
}
