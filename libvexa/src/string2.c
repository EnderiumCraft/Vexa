/* More of <string.h> and <strings.h>: errors, tokens, case-blind comparison. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const error_names[] = {
    [0] = "Success", [EPERM] = "Operation not permitted", [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process", [EINTR] = "Interrupted", [EIO] = "Input/output error",
    [ENXIO] = "No such device or address", [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Not a program", [EBADF] = "Bad file descriptor", [ECHILD] = "No child process",
    [EAGAIN] = "Try again", [ENOMEM] = "Out of memory", [EACCES] = "Permission denied",
    [EFAULT] = "Bad address", [EBUSY] = "In use", [EEXIST] = "File exists",
    [EXDEV] = "Crosses file systems", [ENODEV] = "No such device", [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory", [EINVAL] = "Invalid argument", [ENFILE] = "Too many open files",
    [EMFILE] = "Too many open files", [ENOTTY] = "Not a terminal", [EFBIG] = "File too big",
    [ENOSPC] = "No space left on device", [ESPIPE] = "Can't seek", [EROFS] = "Read-only file system",
    [EPIPE] = "Broken pipe", [EDOM] = "Argument out of domain", [ERANGE] = "Result out of range",
    [EDEADLK] = "Deadlock", [ENAMETOOLONG] = "Name too long", [ENOSYS] = "Not implemented",
    [ENOTEMPTY] = "Directory not empty", [ELOOP] = "Too many symbolic links",
    [EOVERFLOW] = "Value too large", [EILSEQ] = "Invalid multibyte sequence",
    [ENOTSOCK] = "Not a socket", [EMSGSIZE] = "Message too long",
    [EPROTONOSUPPORT] = "Protocol not supported", [EOPNOTSUPP] = "Not supported",
    [EAFNOSUPPORT] = "Address family not supported", [EADDRINUSE] = "Address in use",
    [EADDRNOTAVAIL] = "Address not available", [ENETUNREACH] = "Network unreachable",
    [ECONNABORTED] = "Connection aborted", [ECONNRESET] = "Connection reset",
    [EISCONN] = "Already connected", [ENOTCONN] = "Not connected", [ETIMEDOUT] = "Timed out",
    [ECONNREFUSED] = "Connection refused", [EHOSTUNREACH] = "Host unreachable",
    [EALREADY] = "Already in progress", [EINPROGRESS] = "In progress", [ECANCELED] = "Canceled",
};

char *strerror(int error) {
    if (error >= 0 && (size_t)error < sizeof(error_names) / sizeof(error_names[0]) &&
        error_names[error]) {
        return (char *)error_names[error];
    }
    static char unknown[32];
    snprintf(unknown, sizeof(unknown), "Unknown error %d", error);
    return unknown;
}

int strerror_r(int error, char *out, size_t size) {
    snprintf(out, size, "%s", strerror(error));
    return 0;
}

char *strsignal(int signal) {
    static const char *const names[] = {
        [1] = "Hangup", [2] = "Interrupt", [3] = "Quit", [4] = "Illegal instruction",
        [6] = "Aborted", [8] = "Floating point exception", [9] = "Killed",
        [11] = "Segmentation fault", [13] = "Broken pipe", [14] = "Alarm clock", [15] = "Terminated",
    };
    return signal > 0 && signal < 16 && names[signal] ? (char *)names[signal] : "Unknown signal";
}

char *strtok_r(char *s, const char *delimiters, char **state) {
    if (!s) {
        s = *state;
    }
    if (!s) {
        return NULL;
    }
    s += strspn(s, delimiters);
    if (!*s) {
        *state = NULL;
        return NULL;
    }
    char *end = s + strcspn(s, delimiters);
    if (*end) {
        *end++ = '\0';
        *state = end;
    } else {
        *state = NULL;
    }
    return s;
}

char *strtok(char *s, const char *delimiters) {
    static char *state;
    return strtok_r(s, delimiters, &state);
}

char *strsep(char **s, const char *delimiters) {
    char *start = *s;
    if (!start) {
        return NULL;
    }
    char *end = start + strcspn(start, delimiters);
    if (*end) {
        *end++ = '\0';
        *s = end;
    } else {
        *s = NULL;
    }
    return start;
}

int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) {
            return ca - cb;
        }
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) {
            return ca - cb;
        }
    }
    return 0;
}

char *strcasestr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, n) == 0) {
            return (char *)haystack;
        }
    }
    return n ? NULL : (char *)haystack;
}

int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

size_t strxfrm(char *out, const char *s, size_t n) {
    size_t length = strlen(s);
    if (n) {
        size_t copy = length < n - 1 ? length : n - 1;
        memcpy(out, s, copy);
        out[copy] = '\0';
    }
    return length;
}

char *stpcpy(char *dest, const char *src) {
    size_t n = strlen(src);
    memcpy(dest, src, n + 1);
    return dest + n;
}

char *stpncpy(char *dest, const char *src, size_t n) {
    size_t length = strnlen(src, n);
    memcpy(dest, src, length);
    memset(dest + length, 0, n - length);
    return dest + length;
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t length = strlen(src);
    if (size) {
        size_t copy = length < size - 1 ? length : size - 1;
        memcpy(dest, src, copy);
        dest[copy] = '\0';
    }
    return length;
}

size_t strlcat(char *dest, const char *src, size_t size) {
    size_t have = strnlen(dest, size);
    return have == size ? size + strlen(src) : have + strlcpy(dest + have, src, size - have);
}

void *memccpy(void *dest, const void *src, int c, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if (s[i] == (unsigned char)c) {
            return d + i + 1;
        }
    }
    return NULL;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) {
        if (p[n] == (unsigned char)c) {
            return (void *)(p + n);
        }
    }
    return NULL;
}

void *mempcpy(void *dest, const void *src, size_t n) {
    return (char *)memcpy(dest, src, n) + n;
}

int ffs(int value) {
    return __builtin_ffs(value);
}

void bzero(void *p, size_t n) {
    memset(p, 0, n);
}

int bcmp(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n);
}
