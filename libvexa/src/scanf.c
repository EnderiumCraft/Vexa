/* The scanf family: one reader over a FILE or a string. */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct reader {
    FILE *file;       /* Either a file... */
    const char *text; /* ...or a string. */
    size_t consumed;  /* Characters taken, for %n. */
};

static int next(struct reader *r) {
    int c;
    if (r->file) {
        c = fgetc(r->file);
    } else {
        c = *r->text ? (unsigned char)*r->text++ : EOF;
    }
    if (c != EOF) {
        r->consumed++;
    }
    return c;
}

static void back(struct reader *r, int c) {
    if (c == EOF) {
        return;
    }
    r->consumed--;
    if (r->file) {
        ungetc(c, r->file);
    } else {
        r->text--;
    }
}

static void skip_space(struct reader *r) {
    int c;
    while ((c = next(r)) != EOF && isspace(c)) {
    }
    back(r, c);
}

/* Reads up to `width` characters that could make a number into `out`.
 * Returns how many were read. */
static size_t read_integer(struct reader *r, char *out, size_t width, int *base) {
    size_t n = 0;
    int c = next(r);
    if ((c == '+' || c == '-') && n < width) {
        out[n++] = (char)c;
        c = next(r);
    }
    if ((*base == 0 || *base == 16) && c == '0' && n < width) {
        out[n++] = (char)c;
        c = next(r);
        if ((c == 'x' || c == 'X') && n < width) {
            int after = next(r);
            if (isxdigit(after)) {
                out[n++] = (char)c;
                *base = 16;
                c = after;
            } else { /* "0x" with nothing after: just the 0. */
                back(r, after);
                if (!r->file) {
                    back(r, c);
                    c = next(r);
                }
                /* (A FILE can only take one character back; the x is lost.) */
                out[n] = '\0';
                back(r, c);
                return n;
            }
        } else if (*base == 0) {
            *base = 8;
        }
    }
    if (*base == 0) {
        *base = 10;
    }
    while (c != EOF && n < width) {
        int digit = isdigit(c) ? c - '0' : isalpha(c) ? tolower(c) - 'a' + 10 : 99;
        if (digit >= *base) {
            break;
        }
        out[n++] = (char)c;
        c = next(r);
    }
    back(r, c);
    out[n] = '\0';
    return n;
}

static size_t read_float(struct reader *r, char *out, size_t width) {
    size_t n = 0;
    int c = next(r);
    if ((c == '+' || c == '-') && n < width) {
        out[n++] = (char)c;
        c = next(r);
    }
    /* inf, infinity, nan */
    if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
        const char *word = (c == 'i' || c == 'I') ? "infinity" : "nan";
        size_t i = 0;
        while (word[i] && n < width && c != EOF && tolower(c) == word[i]) {
            out[n++] = (char)c;
            i++;
            c = next(r);
        }
        back(r, c);
        out[n] = '\0';
        return n;
    }
    bool hex = false, digits = false;
    if (c == '0' && n < width) {
        out[n++] = (char)c;
        digits = true;
        c = next(r);
        if ((c == 'x' || c == 'X') && n < width) {
            hex = true;
            out[n++] = (char)c;
            c = next(r);
        }
    }
    bool dot = false, exponent = false;
    while (c != EOF && n < width) {
        if (hex ? isxdigit(c) : isdigit(c)) {
            digits = true;
        } else if (c == '.' && !dot && !exponent) {
            dot = true;
        } else if (digits && !exponent && (hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E'))) {
            exponent = true;
            out[n++] = (char)c;
            c = next(r);
            if ((c == '+' || c == '-') && n < width) {
                out[n++] = (char)c;
                c = next(r);
            }
            continue;
        } else {
            break;
        }
        out[n++] = (char)c;
        c = next(r);
    }
    back(r, c);
    out[n] = '\0';
    return n;
}

/* A %[...] set: which bytes match. Returns the position after the ']'. */
static const char *parse_set(const char *f, bool set[256]) {
    bool invert = false;
    if (*f == '^') {
        invert = true;
        f++;
    }
    memset(set, 0, 256);
    if (*f == ']') {
        set[']'] = true;
        f++;
    }
    for (; *f && *f != ']'; f++) {
        if (f[1] == '-' && f[2] && f[2] != ']') {
            for (int c = (unsigned char)f[0]; c <= (unsigned char)f[2]; c++) {
                set[c] = true;
            }
            f += 2;
        } else {
            set[(unsigned char)*f] = true;
        }
    }
    if (invert) {
        for (int i = 0; i < 256; i++) {
            set[i] = !set[i];
        }
    }
    return *f ? f + 1 : f;
}

static void store_integer(va_list *args, int length, bool is_signed, unsigned long long value) {
    switch (length) {
    case -2:
        *va_arg(*args, char *) = (char)value;
        break;
    case -1:
        *va_arg(*args, short *) = (short)value;
        break;
    case 0:
        *va_arg(*args, int *) = (int)value;
        break;
    case 1:
        *va_arg(*args, long *) = (long)value;
        break;
    default:
        *va_arg(*args, long long *) = (long long)value;
        break;
    }
    (void)is_signed;
}

static int scan(struct reader *r, const char *f, va_list ap) {
    va_list args;
    va_copy(args, ap);
    int assigned = 0;
    bool any_input = false;
    char buffer[512];
    for (; *f; f++) {
        if (isspace((unsigned char)*f)) {
            skip_space(r);
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') {
                f++;
                skip_space(r);
            }
            int c = next(r);
            if (c != (unsigned char)*f) {
                back(r, c);
                if (c == EOF && !any_input) {
                    va_end(args);
                    return EOF;
                }
                break;
            }
            any_input = true;
            continue;
        }
        f++;
        bool skip = false;
        if (*f == '*') {
            skip = true;
            f++;
        }
        size_t width = 0;
        while (isdigit((unsigned char)*f)) {
            width = width * 10 + (size_t)(*f++ - '0');
        }
        bool allocate = false;
        if (*f == 'm') {
            allocate = true;
            f++;
        }
        int length = 0;
        bool long_double = false;
        for (;; f++) {
            if (*f == 'l') length++;
            else if (*f == 'h') length--;
            else if (*f == 'z' || *f == 'j' || *f == 't') length = 1;
            else if (*f == 'q') length = 2;
            else if (*f == 'L') long_double = true, length = 2;
            else break;
        }
        char conv = *f;
        if (!conv) {
            break;
        }
        if (conv == 'n') {
            if (!skip) {
                store_integer(&args, length, true, r->consumed);
            }
            continue;
        }
        if (conv != 'c' && conv != '[') {
            skip_space(r);
        }
        /* Fails on end of input before the first conversion. */
        int peek = next(r);
        if (peek == EOF) {
            va_end(args);
            return assigned || any_input ? assigned : EOF;
        }
        back(r, peek);
        any_input = true;

        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            size_t limit = width && width < sizeof buffer - 1 ? width : sizeof buffer - 1;
            size_t n = read_integer(r, buffer, limit, &base);
            if (n == 0 || (n == 1 && (buffer[0] == '+' || buffer[0] == '-'))) {
                goto done;
            }
            if (!skip) {
                if (conv == 'p') {
                    *va_arg(args, void **) = (void *)(uintptr_t)strtoull(buffer, NULL, 16);
                } else if (conv == 'd' || conv == 'i') {
                    store_integer(&args, length, true, (unsigned long long)strtoll(buffer, NULL, base));
                } else {
                    store_integer(&args, length, false, strtoull(buffer, NULL, base));
                }
                assigned++;
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            size_t limit = width && width < sizeof buffer - 1 ? width : sizeof buffer - 1;
            size_t n = read_float(r, buffer, limit);
            char *end;
            long double value = strtold(buffer, &end);
            if (n == 0 || end == buffer) {
                goto done;
            }
            if (!skip) {
                if (long_double) {
                    *va_arg(args, long double *) = value;
                } else if (length >= 1) {
                    *va_arg(args, double *) = (double)value;
                } else {
                    *va_arg(args, float *) = (float)value;
                }
                assigned++;
            }
            break;
        }
        case 's':
        case 'c':
        case '[': {
            bool set[256];
            if (conv == '[') {
                f = parse_set(f + 1, set) - 1;
            } else if (conv == 's') {
                for (int i = 0; i < 256; i++) {
                    set[i] = !isspace(i);
                }
            } else {
                memset(set, 1, sizeof set);
            }
            if (width == 0) {
                width = conv == 'c' ? 1 : SIZE_MAX;
            }
            char *out = NULL;
            char **allocated = NULL;
            size_t capacity = 0;
            if (!skip) {
                if (allocate) {
                    allocated = va_arg(args, char **);
                    capacity = 32;
                    out = malloc(capacity);
                    if (!out) {
                        goto done;
                    }
                } else {
                    out = va_arg(args, char *);
                }
            }
            size_t n = 0;
            int c = EOF;
            while (n < width && (c = next(r)) != EOF && set[c]) {
                if (out) {
                    if (allocated && n + 2 > capacity) {
                        char *bigger = realloc(out, capacity * 2);
                        if (!bigger) {
                            free(out);
                            goto done;
                        }
                        out = bigger;
                        capacity *= 2;
                    }
                    out[n] = (char)c;
                }
                n++;
                c = EOF;
            }
            back(r, c);
            if (n == 0 || (conv == 'c' && n < width)) {
                if (allocated) {
                    free(out);
                }
                goto done;
            }
            if (out) {
                if (conv != 'c') {
                    out[n] = '\0';
                }
                if (allocated) {
                    *allocated = out;
                }
                assigned++;
            }
            break;
        }
        default:
            goto done;
        }
    }
done:
    va_end(args);
    return assigned;
}

int vfscanf(FILE *file, const char *f, va_list args) {
    struct reader r = {.file = file};
    return scan(&r, f, args);
}

int vsscanf(const char *text, const char *f, va_list args) {
    struct reader r = {.text = text};
    return scan(&r, f, args);
}

int vscanf(const char *f, va_list args) {
    return vfscanf(stdin, f, args);
}

int fscanf(FILE *file, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vfscanf(file, f, args);
    va_end(args);
    return n;
}

int sscanf(const char *text, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vsscanf(text, f, args);
    va_end(args);
    return n;
}

int scanf(const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vfscanf(stdin, f, args);
    va_end(args);
    return n;
}
