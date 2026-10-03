/* Numbers from text (strtod, strtoll...), random numbers, searching, and
 * other parts of <stdlib.h> beyond the basics. */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vexa/syscall.h>

/* ---- Integers ---- */

unsigned long long strtoull(const char *text, char **end, int base) {
    const char *p = text;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = *p++ == '-';
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }
    unsigned long long value = 0;
    bool any = false, overflow = false;
    for (;; p++) {
        int digit = isdigit((unsigned char)*p) ? *p - '0'
                    : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10
                                                 : 99;
        if (digit >= base) {
            break;
        }
        any = true;
        if (value > (ULLONG_MAX - (unsigned)digit) / (unsigned)base) {
            overflow = true;
        }
        value = value * (unsigned)base + (unsigned)digit;
    }
    if (end) {
        *end = (char *)(any ? p : text);
    }
    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return negative ? -value : value;
}

long long strtoll(const char *text, char **end, int base) {
    const char *p = text;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    bool negative = *p == '-';
    int saved = errno;
    errno = 0;
    unsigned long long magnitude = strtoull(negative ? p + 1 : p, end, base);
    if (end && *end == p + (negative ? 1 : 0)) {
        *end = (char *)text;
    }
    if (errno == ERANGE || magnitude > (unsigned long long)LLONG_MAX + negative) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    errno = saved;
    return negative ? -(long long)magnitude : (long long)magnitude;
}

intmax_t strtoimax(const char *text, char **end, int base) {
    return strtoll(text, end, base);
}

uintmax_t strtoumax(const char *text, char **end, int base) {
    return strtoull(text, end, base);
}

long long atoll(const char *text) {
    return strtoll(text, NULL, 10);
}

long long llabs(long long value) {
    return value < 0 ? -value : value;
}

div_t div(int a, int b) {
    return (div_t){a / b, a % b};
}

ldiv_t ldiv(long a, long b) {
    return (ldiv_t){a / b, a % b};
}

lldiv_t lldiv(long long a, long long b) {
    return (lldiv_t){a / b, a % b};
}

/* ---- Floating point ----
 * Decimal: up to 19 significant digits into an integer, then scaled by the
 * power of ten in long double (64-bit precision), which rounds to the right
 * double for nearly everything. Also hex floats, "inf" and "nan". */

static long double power10(int e) {
    long double result = 1, factor = 10;
    bool negative = e < 0;
    unsigned n = (unsigned)(negative ? -e : e);
    while (n) {
        if (n & 1) {
            result *= factor;
        }
        factor *= factor;
        n >>= 1;
    }
    return negative ? 1 / result : result;
}

long double strtold(const char *text, char **end) {
    const char *p = text;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = *p++ == '-';
    }
    long double sign = negative ? -1.0L : 1.0L;
    if (strncasecmp(p, "inf", 3) == 0) {
        p += strncasecmp(p, "infinity", 8) == 0 ? 8 : 3;
        if (end) {
            *end = (char *)p;
        }
        return sign * INFINITY;
    }
    if (strncasecmp(p, "nan", 3) == 0) {
        p += 3;
        if (*p == '(') {
            const char *q = strchr(p, ')');
            if (q) {
                p = q + 1;
            }
        }
        if (end) {
            *end = (char *)p;
        }
        return negative ? -NAN : NAN;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        p += 2;
        long double value = 0;
        int exponent = 0;
        bool dot = false;
        for (;; p++) {
            if (*p == '.' && !dot) {
                dot = true;
                continue;
            }
            if (!isxdigit((unsigned char)*p)) {
                break;
            }
            int digit = isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10;
            value = value * 16 + digit;
            if (dot) {
                exponent -= 4;
            }
        }
        if (*p == 'p' || *p == 'P') {
            char *after;
            long e = strtol(p + 1, &after, 10);
            if (after != p + 1) {
                exponent += (int)e;
                p = after;
            }
        }
        if (end) {
            *end = (char *)p;
        }
        return sign * ldexpl(value, exponent);
    }
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0;
    bool any = false, dot = false;
    for (;; p++) {
        if (*p == '.' && !dot) {
            dot = true;
            continue;
        }
        if (!isdigit((unsigned char)*p)) {
            break;
        }
        any = true;
        if (digits < 19) {
            if (mantissa || *p != '0') {
                mantissa = mantissa * 10 + (uint64_t)(*p - '0');
                digits++;
            }
            if (dot) {
                exponent--;
            }
        } else if (!dot) {
            exponent++;
        }
    }
    if (!any) {
        if (end) {
            *end = (char *)text;
        }
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        char *after;
        long e = strtol(p + 1, &after, 10);
        if (after != p + 1) {
            exponent += e > 100000 ? 100000 : e < -100000 ? -100000 : (int)e;
            p = after;
        }
    }
    if (end) {
        *end = (char *)p;
    }
    if (!mantissa) {
        return sign * 0.0L;
    }
    long double value = (long double)mantissa;
    /* Scale in two steps near the limits, so neither overflows on the way. */
    if (exponent < -4900) {
        value *= power10(-4900);
        exponent += 4900;
    }
    value *= power10(exponent);
    return sign * value;
}

double strtod(const char *text, char **end) {
    long double value = strtold(text, end);
    double result = (double)value;
    if ((isinf(result) && !isinf(value)) || (result == 0 && value != 0)) {
        errno = ERANGE;
    }
    return result;
}

float strtof(const char *text, char **end) {
    long double value = strtold(text, end);
    float result = (float)value;
    if ((isinf(result) && !isinf(value)) || (result == 0 && value != 0)) {
        errno = ERANGE;
    }
    return result;
}

double atof(const char *text) {
    return strtod(text, NULL);
}

/* ---- Random numbers (a 64-bit LCG; the high bits are the good ones) ---- */

static uint64_t random_state = 1;

int rand_r(unsigned *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return (int)((*seed >> 1) & RAND_MAX);
}

int rand(void) {
    random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
    return (int)(random_state >> 33);
}

void srand(unsigned seed) {
    random_state = seed;
}

long random(void) {
    return rand();
}

void srandom(unsigned seed) {
    srand(seed);
}

/* ---- Searching ---- */

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    const char *low = base;
    while (count) {
        const char *middle = low + (count / 2) * size;
        int c = compare(key, middle);
        if (c == 0) {
            return (void *)middle;
        }
        if (c > 0) {
            low = middle + size;
            count -= count / 2 + 1;
        } else {
            count /= 2;
        }
    }
    return NULL;
}

/* ---- Files ---- */

static void fill_random(char *x) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint64_t r = (uint64_t)vx_uptime() * 2654435761u ^ (uint64_t)vx_process_id() << 32 ^
                 (uint64_t)rand();
    for (int i = 0; i < 6; i++) {
        x[i] = letters[r % 62];
        r = r / 62 ^ (uint64_t)rand() << 20;
    }
}

int mkstemp(char *pattern) {
    size_t n = strlen(pattern);
    if (n < 6 || strcmp(pattern + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    for (int tries = 0; tries < 100; tries++) {
        fill_random(pattern + n - 6);
        struct vx_stat st;
        if (vx_stat(pattern, &st) == 0) {
            continue;
        }
        int fd = vx_open(pattern, VX_OPEN_READ | VX_OPEN_WRITE | VX_OPEN_CREATE);
        if (fd >= 0) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}

char *mkdtemp(char *pattern) {
    size_t n = strlen(pattern);
    if (n < 6 || strcmp(pattern + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return NULL;
    }
    for (int tries = 0; tries < 100; tries++) {
        fill_random(pattern + n - 6);
        if (vx_mkdir(pattern) == 0) {
            return pattern;
        }
    }
    errno = EEXIST;
    return NULL;
}

char *mktemp(char *pattern) {
    size_t n = strlen(pattern);
    if (n >= 6) {
        fill_random(pattern + n - 6);
    }
    return pattern;
}

/* A path made absolute, with ".", ".." and symbolic links worked out. */
char *realpath(const char *path, char *resolved) {
    char work[PATH_MAX * 2], out[PATH_MAX];
    if (path[0] == '/') {
        snprintf(work, sizeof(work), "%s", path);
    } else {
        char here[PATH_MAX];
        if (!getcwd(here, sizeof(here))) {
            return NULL;
        }
        snprintf(work, sizeof(work), "%s/%s", here, path);
    }
    size_t length = 0;
    out[0] = '\0';
    int links = 0;
    char *rest = work;
    while (*rest) {
        while (*rest == '/') {
            rest++;
        }
        char *slash = strchr(rest, '/');
        size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
        if (n == 0 || (n == 1 && rest[0] == '.')) {
            rest += n;
            continue;
        }
        if (n == 2 && rest[0] == '.' && rest[1] == '.') {
            while (length && out[length - 1] != '/') {
                length--;
            }
            if (length) {
                length--;
            }
            out[length] = '\0';
            rest += n;
            continue;
        }
        if (length + 1 + n >= sizeof(out)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        out[length++] = '/';
        memcpy(out + length, rest, n);
        length += n;
        out[length] = '\0';
        rest += n;
        struct vx_stat st;
        long error = vx_lstat(out, &st);
        if (error) {
            errno = error == -VX_ENOENT ? ENOENT : EIO;
            return NULL;
        }
        if (st.type == VX_TYPE_SYMLINK) {
            char target[PATH_MAX];
            long t = vx_readlink(out, target, sizeof(target) - 1);
            if (t < 0 || ++links > 32) {
                errno = ELOOP;
                return NULL;
            }
            target[t] = '\0';
            char next[PATH_MAX * 2];
            if (target[0] == '/') {
                snprintf(next, sizeof(next), "%s%s", target, rest);
                length = 0;
            } else {
                while (length && out[length - 1] != '/') {
                    length--;
                }
                out[length] = '\0';
                snprintf(next, sizeof(next), "%s%s", target, rest);
            }
            snprintf(work, sizeof(work), "%s", next);
            rest = work;
        }
    }
    if (!length) {
        out[length++] = '/';
        out[length] = '\0';
    }
    if (!resolved) {
        return strdup(out);
    }
    memcpy(resolved, out, length + 1);
    return resolved;
}

char *tmpnam(char *out) {
    static char name[32];
    char *p = out ? out : name;
    snprintf(p, 32, "/tmp/tmp.XXXXXX");
    return mktemp(p);
}

int putenv(char *assignment) {
    char *eq = strchr(assignment, '=');
    if (!eq) {
        return unsetenv(assignment);
    }
    char name[256];
    size_t n = (size_t)(eq - assignment) < sizeof(name) - 1 ? (size_t)(eq - assignment)
                                                             : sizeof(name) - 1;
    memcpy(name, assignment, n);
    name[n] = '\0';
    return setenv(name, eq + 1, 1);
}

/* ---- Multibyte text: UTF-8 ---- */

int mbtowc(wchar_t *out, const char *s, size_t n) {
    if (!s) {
        return 0;
    }
    const unsigned char *u = (const unsigned char *)s;
    if (n == 0) {
        return -1;
    }
    unsigned c = u[0];
    int length = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3
                 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (!length || (size_t)length > n) {
        errno = EILSEQ;
        return -1;
    }
    if (length > 1) {
        c &= 0xff >> (length + 1);
        for (int i = 1; i < length; i++) {
            if ((u[i] & 0xc0) != 0x80) {
                errno = EILSEQ;
                return -1;
            }
            c = c << 6 | (u[i] & 0x3f);
        }
    }
    if (out) {
        *out = (wchar_t)c;
    }
    return c ? length : 0;
}

int mblen(const char *s, size_t n) {
    return mbtowc(NULL, s, n);
}

int wctomb(char *out, wchar_t wc) {
    if (!out) {
        return 0;
    }
    unsigned c = (unsigned)wc;
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    if (c < 0x110000) {
        out[0] = (char)(0xf0 | c >> 18);
        out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[3] = (char)(0x80 | (c & 0x3f));
        return 4;
    }
    errno = EILSEQ;
    return -1;
}

size_t mbstowcs(wchar_t *out, const char *s, size_t n) {
    size_t count = 0;
    while (*s && (!out || count < n)) {
        wchar_t c;
        int length = mbtowc(&c, s, 4);
        if (length < 0) {
            return (size_t)-1;
        }
        if (out) {
            out[count] = c;
        }
        count++;
        s += length;
    }
    if (out && count < n) {
        out[count] = 0;
    }
    return count;
}

size_t wcstombs(char *out, const wchar_t *s, size_t n) {
    size_t count = 0;
    char buffer[4];
    for (; *s; s++) {
        int length = wctomb(buffer, *s);
        if (length < 0) {
            return (size_t)-1;
        }
        if (out) {
            if (count + (size_t)length > n) {
                break;
            }
            memcpy(out + count, buffer, (size_t)length);
        }
        count += (size_t)length;
    }
    if (out && count < n) {
        out[count] = '\0';
    }
    return count;
}
