#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

/* printf formats into a buffer and hands it to the kernel in one vx_log call,
 * so a line from one program doesn't get split by another's output. */

struct output {
    char buffer[512];
    size_t used;
    int total;
};

static void flush(struct output *out) {
    if (out->used) {
        vx_log(out->buffer, out->used);
        out->used = 0;
    }
}

static void put(struct output *out, char c) {
    if (out->used == sizeof(out->buffer)) {
        flush(out);
    }
    out->buffer[out->used++] = c;
    out->total++;
}

static void put_unsigned(struct output *out, uint64_t value, unsigned base, int min_digits) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    while (n < min_digits) {
        digits[n++] = '0';
    }
    while (n--) {
        put(out, digits[n]);
    }
}

int printf(const char *format, ...) {
    struct output out = {.used = 0, .total = 0};
    va_list args;
    va_start(args, format);
    for (const char *f = format; *f; f++) {
        if (*f != '%') {
            put(&out, *f);
            continue;
        }
        f++;
        bool is_long = false;
        while (*f == 'l') {
            is_long = true;
            f++;
        }
        switch (*f) {
        case 's': {
            const char *s = va_arg(args, const char *);
            for (s = s ? s : "(null)"; *s; s++) {
                put(&out, *s);
            }
            break;
        }
        case 'c':
            put(&out, (char)va_arg(args, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = is_long ? va_arg(args, long) : va_arg(args, int);
            if (v < 0) {
                put(&out, '-');
                put_unsigned(&out, (uint64_t)-v, 10, 1);
            } else {
                put_unsigned(&out, (uint64_t)v, 10, 1);
            }
            break;
        }
        case 'u':
            put_unsigned(&out, is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned), 10, 1);
            break;
        case 'x':
            put_unsigned(&out, is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned), 16, 1);
            break;
        case 'p':
            put(&out, '0');
            put(&out, 'x');
            put_unsigned(&out, (uint64_t)va_arg(args, void *), 16, 1);
            break;
        case '%':
            put(&out, '%');
            break;
        case '\0':
            f--;
            break;
        default:
            put(&out, '%');
            put(&out, *f);
            break;
        }
    }
    va_end(args);
    flush(&out);
    return out.total;
}

int puts(const char *text) {
    return printf("%s\n", text);
}
