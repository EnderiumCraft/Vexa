#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include "format.h"
#include "internal.h"

/* ---- Files ----
 * Each FILE has a lock, so threads can share one; the list of open files has
 * another. Internal helpers ending in _unlocked expect the FILE locked. */

enum buffering { UNBUFFERED, LINE_BUFFERED, FULLY_BUFFERED };

struct vx_file {
    int handle;
    bool readable, writable, eof, error;
    enum buffering buffering;
    char *buffer;
    size_t size;
    size_t write_used;          /* Bytes waiting to be written. */
    size_t read_pos, read_len;  /* Buffered input. */
    bool has_ungot;             /* ungetc's character, read before the buffer. */
    unsigned char ungot;
    bool own_buffer;            /* (Not given with setvbuf.) */
    int process;                /* popen's process handle, or 0. */
    bool cookie_file;           /* fopencookie (open_memstream, fmemopen): */
    void *cookie;               /* its functions do the I/O, not a handle. */
    cookie_io_functions_t io;
    struct vx_mutex lock;
    struct vx_file *next;
};

static struct vx_mutex list_lock = VX_MUTEX_INIT;

static struct vx_file std_files[3];
static char stdin_buffer[BUFSIZ], stdout_buffer[BUFSIZ];
static struct vx_file *open_files;

FILE *stdin = &std_files[0];
FILE *stdout = &std_files[1];
FILE *stderr = &std_files[2];

void __libvexa_stdio_init(void) {
    std_files[0] = (struct vx_file){.handle = 0, .readable = true, .buffering = FULLY_BUFFERED,
                                    .buffer = stdin_buffer, .size = BUFSIZ};
    std_files[1] = (struct vx_file){.handle = 1, .writable = true, .buffering = LINE_BUFFERED,
                                    .buffer = stdout_buffer, .size = BUFSIZ};
    std_files[2] = (struct vx_file){.handle = 2, .writable = true, .buffering = UNBUFFERED};
    std_files[0].next = &std_files[1];
    std_files[1].next = &std_files[2];
    open_files = &std_files[0];
}

/* The I/O under the buffers: the handle's, or a cookie file's functions
 * (vx-style results: a count, or a negative VX_E code). */
static long raw_write(FILE *file, const void *data, size_t n) {
    if (file->cookie_file) {
        ssize_t r = file->io.write ? file->io.write(file->cookie, data, n) : -1;
        return r < 0 ? -VX_EIO : r;
    }
    return vx_write(file->handle, data, n);
}

static long raw_read(FILE *file, void *data, size_t n) {
    if (file->cookie_file) {
        ssize_t r = file->io.read ? file->io.read(file->cookie, data, n) : 0;
        return r < 0 ? -VX_EIO : r;
    }
    return vx_read(file->handle, data, n);
}

static long raw_seek(FILE *file, long offset, int whence) {
    if (file->cookie_file) {
        off_t position = offset;
        if (!file->io.seek || file->io.seek(file->cookie, &position, whence) < 0) {
            return -VX_ESPIPE;
        }
        return (long)position;
    }
    return vx_seek(file->handle, offset, whence);
}

static int flush_one(FILE *file) {
    size_t done = 0;
    while (done < file->write_used) {
        long n = raw_write(file, file->buffer + done, file->write_used - done);
        if (n <= 0) {
            file->error = true;
            file->write_used = 0;
            return EOF;
        }
        done += n;
    }
    file->write_used = 0;
    return 0;
}

int fflush(FILE *file) {
    if (file) {
        vx_mutex_lock(&file->lock);
        int result = file->writable ? flush_one(file) : 0;
        vx_mutex_unlock(&file->lock);
        return result;
    }
    int result = 0;
    vx_mutex_lock(&list_lock);
    for (FILE *f = open_files; f; f = f->next) {
        vx_mutex_lock(&f->lock);
        if (f->writable && flush_one(f)) {
            result = EOF;
        }
        vx_mutex_unlock(&f->lock);
    }
    vx_mutex_unlock(&list_lock);
    return result;
}

static FILE *new_file(int handle, bool readable, bool writable) {
    FILE *file = calloc(1, sizeof(FILE));
    char *buffer = malloc(BUFSIZ);
    if (!file || !buffer) {
        free(file);
        free(buffer);
        return NULL;
    }
    file->handle = handle;
    file->readable = readable;
    file->writable = writable;
    file->buffering = FULLY_BUFFERED;
    file->buffer = buffer;
    file->size = BUFSIZ;
    file->own_buffer = true;
    vx_mutex_lock(&list_lock);
    file->next = open_files;
    open_files = file;
    vx_mutex_unlock(&list_lock);
    return file;
}

static unsigned mode_flags(const char *mode, bool *readable, bool *writable) {
    bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r':
        *readable = true;
        *writable = plus;
        return VX_OPEN_READ | (plus ? VX_OPEN_WRITE : 0);
    case 'w':
        *readable = plus;
        *writable = true;
        return VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE | (plus ? VX_OPEN_READ : 0);
    case 'a':
        *readable = plus;
        *writable = true;
        return VX_OPEN_APPEND | VX_OPEN_CREATE | (plus ? VX_OPEN_READ : 0);
    default:
        return 0;
    }
}

FILE *fopen(const char *path, const char *mode) {
    bool readable = false, writable = false;
    unsigned flags = mode_flags(mode, &readable, &writable);
    if (!flags) {
        return NULL;
    }
    int handle = vx_open(path, flags);
    if (handle < 0) {
        return NULL;
    }
    FILE *file = new_file(handle, readable, writable);
    if (!file) {
        vx_close(handle);
    }
    return file;
}

FILE *fdopen(int handle, const char *mode) {
    bool readable = false, writable = false;
    return mode_flags(mode, &readable, &writable) ? new_file(handle, readable, writable) : NULL;
}

int fclose(FILE *file) {
    int result = fflush(file);
    if (file >= &std_files[0] && file <= &std_files[2]) {
        return result;
    }
    vx_mutex_lock(&list_lock);
    for (FILE **link = &open_files; *link; link = &(*link)->next) {
        if (*link == file) {
            *link = file->next;
            break;
        }
    }
    vx_mutex_unlock(&list_lock);
    if (file->cookie_file) {
        if (file->io.close && file->io.close(file->cookie) < 0) {
            result = EOF;
        }
    } else {
        vx_close(file->handle);
    }
    if (file->process) { /* popen: wait for the program. */
        long code = vx_wait(file->process, 0);
        vx_close(file->process);
        result = code < 0 ? -1 : (int)((code & 0xff) << 8);
    }
    if (file->own_buffer) {
        free(file->buffer);
    }
    free(file);
    return result;
}

int fileno(FILE *file) {
    if (file->cookie_file) {
        errno = EBADF;
        return -1;
    }
    return file->handle;
}

int feof(FILE *file) {
    return file->eof;
}

int ferror(FILE *file) {
    return file->error;
}

/* Before writing after reading: the handle is past what was read ahead. */
static void drop_input(FILE *file) {
    size_t ahead = file->read_len - file->read_pos + (file->has_ungot ? 1 : 0);
    if (ahead) {
        raw_seek(file, -(long)ahead, VX_SEEK_CURRENT);
    }
    file->read_pos = file->read_len = 0;
    file->has_ungot = false;
}

static int fputc_unlocked(int c, FILE *file) {
    if (!file->writable) {
        file->error = true;
        return EOF;
    }
    if (file->read_len || file->has_ungot) {
        drop_input(file);
    }
    if (file->buffering == UNBUFFERED) {
        char ch = (char)c;
        return raw_write(file, &ch, 1) == 1 ? (unsigned char)c : EOF;
    }
    if (file->write_used == file->size && flush_one(file)) {
        return EOF;
    }
    file->buffer[file->write_used++] = (char)c;
    if (file->buffering == LINE_BUFFERED && c == '\n' && flush_one(file)) {
        return EOF;
    }
    return (unsigned char)c;
}

int fputc(int c, FILE *file) {
    vx_mutex_lock(&file->lock);
    int result = fputc_unlocked(c, file);
    vx_mutex_unlock(&file->lock);
    return result;
}

static size_t fwrite_unlocked(const void *buffer, size_t size, size_t count, FILE *file) {
    const char *p = buffer;
    size_t total = size * count;
    if (file->read_len || file->has_ungot) {
        drop_input(file);
    }
    if (file->buffering == UNBUFFERED || (file->write_used == 0 && total >= file->size)) {
        size_t done = 0;
        while (done < total) {
            long n = raw_write(file, p + done, total - done);
            if (n <= 0) {
                file->error = true;
                break;
            }
            done += n;
        }
        return size ? done / size : 0;
    }
    for (size_t i = 0; i < total; i++) {
        if (fputc_unlocked(p[i], file) == EOF) {
            return size ? i / size : 0;
        }
    }
    return count;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *file) {
    vx_mutex_lock(&file->lock);
    size_t result = fwrite_unlocked(buffer, size, count, file);
    vx_mutex_unlock(&file->lock);
    return result;
}

int fputs(const char *text, FILE *file) {
    size_t n = strlen(text);
    return fwrite(text, 1, n, file) == n ? 0 : EOF;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *text) {
    size_t n = strlen(text);
    vx_mutex_lock(&stdout->lock);
    int result = fwrite_unlocked(text, 1, n, stdout) == n && fputc_unlocked('\n', stdout) != EOF
                     ? 0
                     : EOF;
    vx_mutex_unlock(&stdout->lock);
    return result;
}

static int fgetc_unlocked(FILE *file) {
    if (!file->readable) {
        file->error = true;
        return EOF;
    }
    if (file->has_ungot) {
        file->has_ungot = false;
        return file->ungot;
    }
    if (file->write_used) {
        flush_one(file);
    }
    if (file->read_pos == file->read_len) {
        if (file == stdin) {
            fflush(stdout); /* Show any prompt before waiting for input. */
        }
        long n = raw_read(file, file->buffer, file->size);
        if (n <= 0) {
            if (n < 0) {
                file->error = true;
            } else {
                file->eof = true;
            }
            return EOF;
        }
        file->read_pos = 0;
        file->read_len = (size_t)n;
    }
    return (unsigned char)file->buffer[file->read_pos++];
}

int fgetc(FILE *file) {
    vx_mutex_lock(&file->lock);
    int c = fgetc_unlocked(file);
    vx_mutex_unlock(&file->lock);
    return c;
}

int getchar(void) {
    return fgetc(stdin);
}

char *fgets(char *line, int size, FILE *file) {
    int n = 0;
    vx_mutex_lock(&file->lock);
    while (n < size - 1) {
        int c = fgetc_unlocked(file);
        if (c == EOF) {
            break;
        }
        line[n++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    vx_mutex_unlock(&file->lock);
    if (n == 0) {
        return NULL;
    }
    line[n] = '\0';
    return line;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *file) {
    char *p = buffer;
    size_t total = size * count, done = 0;
    vx_mutex_lock(&file->lock);
    while (done < total) {
        if (!file->has_ungot && file->read_pos < file->read_len) {
            size_t n = file->read_len - file->read_pos;
            n = n < total - done ? n : total - done;
            memcpy(p + done, file->buffer + file->read_pos, n);
            file->read_pos += n;
            done += n;
            continue;
        }
        int c = fgetc_unlocked(file);
        if (c == EOF) {
            break;
        }
        p[done++] = (char)c;
    }
    vx_mutex_unlock(&file->lock);
    return size ? done / size : 0;
}

/* ---- Formatting ---- */

#define emit __libvexa_emit

void __libvexa_emit(struct sink *sink, char c) {
    if (sink->file) {
        fputc_unlocked(c, sink->file);
    } else if (sink->count + 1 < sink->size) {
        sink->out[sink->count] = c;
    }
    sink->count++;
}

static void emit_padded(struct sink *sink, const char *text, size_t length, int width, bool left,
                        char pad) {
    size_t padding = width > 0 && (size_t)width > length ? (size_t)width - length : 0;
    if (!left) {
        for (size_t i = 0; i < padding; i++) {
            emit(sink, pad);
        }
    }
    for (size_t i = 0; i < length; i++) {
        emit(sink, text[i]);
    }
    if (left) {
        for (size_t i = 0; i < padding; i++) {
            emit(sink, ' ');
        }
    }
}

static int format(struct sink *sink, const char *f, va_list args) {
    for (; *f; f++) {
        if (*f != '%') {
            emit(sink, *f);
            continue;
        }
        f++;
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') zero = true;
            else if (*f == '+') plus = true;
            else if (*f == ' ') space = true;
            else if (*f == '#') alt = true;
            else break;
        }
        int width = 0;
        if (*f == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            f++;
        }
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f++ - '0');
        }
        int precision = -1;
        if (*f == '.') {
            f++;
            precision = 0;
            if (*f == '*') {
                precision = va_arg(args, int);
                f++;
            }
            while (*f >= '0' && *f <= '9') {
                precision = precision * 10 + (*f++ - '0');
            }
        }
        int length = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        bool long_double = false;
        for (;; f++) {
            if (*f == 'l') length++;
            else if (*f == 'h') length--;
            else if (*f == 'z' || *f == 'j' || *f == 't') length = 1;
            else if (*f == 'q') length = 2;
            else if (*f == 'L') long_double = true;
            else break;
        }

        char digits[72];
        size_t n = 0;
        switch (*f) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'p': {
            bool is_signed = *f == 'd' || *f == 'i';
            unsigned base = *f == 'x' || *f == 'X' || *f == 'p' ? 16 : *f == 'o' ? 8 : 10;
            const char *alphabet = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            uint64_t value;
            bool negative = false;
            if (*f == 'p') {
                value = (uint64_t)va_arg(args, void *);
            } else if (is_signed) {
                int64_t v = length >= 1 ? va_arg(args, long) : va_arg(args, int);
                if (length == -1) v = (short)v;
                if (length <= -2) v = (signed char)v;
                negative = v < 0;
                value = negative ? -(uint64_t)v : (uint64_t)v;
            } else {
                value = length >= 1 ? va_arg(args, unsigned long) : va_arg(args, unsigned);
                if (length == -1) value = (unsigned short)value;
                if (length <= -2) value = (unsigned char)value;
            }
            char reversed[24];
            size_t count = 0;
            do {
                reversed[count++] = alphabet[value % base];
                value /= base;
            } while (value);
            if (precision == 0 && count == 1 && reversed[0] == '0' && *f != 'p') {
                count = 0;
            }
            if (negative) digits[n++] = '-';
            else if (plus && is_signed) digits[n++] = '+';
            else if (space && is_signed) digits[n++] = ' ';
            bool nonzero = count > 1 || (count == 1 && reversed[0] != '0');
            if (*f == 'p' || (alt && nonzero && (*f == 'x' || *f == 'X'))) {
                digits[n++] = '0';
                digits[n++] = *f == 'X' ? 'X' : 'x';
            } else if (alt && *f == 'o' && (count == 0 || reversed[count - 1] != '0') &&
                       precision <= (int)count) {
                digits[n++] = '0';
            }
            size_t prefix = n;
            for (int i = (int)count; i < precision; i++) {
                digits[n++] = '0';
            }
            while (count) {
                digits[n++] = reversed[--count];
            }
            if (zero && !left && precision < 0) {
                /* Zero padding goes after the sign. */
                for (size_t i = 0; i < prefix; i++) {
                    emit(sink, digits[i]);
                }
                emit_padded(sink, digits + prefix, n - prefix, width - (int)prefix, false, '0');
            } else {
                emit_padded(sink, digits, n, width, left, ' ');
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
            long double value = long_double ? va_arg(args, long double) : va_arg(args, double);
            unsigned flags = (left ? LEFT_ADJ : 0) | (zero ? ZERO_PAD : 0) |
                             (plus ? MARK_POS : 0) | (space ? PAD_POS : 0) | (alt ? ALT_FORM : 0);
            __libvexa_fmt_fp(sink, value, width, precision, flags, *f);
            break;
        }
        case 'n':
            if (length >= 1) {
                *va_arg(args, long *) = (long)sink->count;
            } else {
                *va_arg(args, int *) = (int)sink->count;
            }
            break;
        case 'c':
            digits[0] = (char)va_arg(args, int);
            emit_padded(sink, digits, 1, width, left, ' ');
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) {
                s = "(null)";
            }
            size_t len = precision >= 0 ? strnlen(s, (size_t)precision) : strlen(s);
            emit_padded(sink, s, len, width, left, ' ');
            break;
        }
        case '%':
            emit(sink, '%');
            break;
        case '\0':
            f--;
            break;
        default:
            emit(sink, '%');
            emit(sink, *f);
            break;
        }
    }
    return (int)sink->count;
}

int vfprintf(FILE *file, const char *f, va_list args) {
    struct sink sink = {.file = file};
    vx_mutex_lock(&file->lock);
    int result = format(&sink, f, args);
    vx_mutex_unlock(&file->lock);
    return result;
}

int vprintf(const char *f, va_list args) {
    return vfprintf(stdout, f, args);
}

int vsnprintf(char *out, size_t size, const char *f, va_list args) {
    struct sink sink = {.out = out, .size = size};
    int count = format(&sink, f, args);
    if (size) {
        out[sink.count < size ? sink.count : size - 1] = '\0';
    }
    return count;
}

int printf(const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vfprintf(stdout, f, args);
    va_end(args);
    return n;
}

int fprintf(FILE *file, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vfprintf(file, f, args);
    va_end(args);
    return n;
}

int snprintf(char *out, size_t size, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vsnprintf(out, size, f, args);
    va_end(args);
    return n;
}

int sprintf(char *out, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vsnprintf(out, SIZE_MAX, f, args);
    va_end(args);
    return n;
}

int vsprintf(char *out, const char *f, va_list args) {
    return vsnprintf(out, SIZE_MAX, f, args);
}

int vdprintf(int fd, const char *f, va_list args) {
    char small[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(small, sizeof small, f, copy);
    va_end(copy);
    if (n < 0) {
        return n;
    }
    char *text = small;
    if ((size_t)n >= sizeof small) {
        text = malloc((size_t)n + 1);
        if (!text) {
            return -1;
        }
        vsnprintf(text, (size_t)n + 1, f, args);
    }
    for (int done = 0; done < n;) {
        long w = vx_write(fd, text + done, (size_t)(n - done));
        if (w <= 0) {
            n = -1;
            break;
        }
        done += (int)w;
    }
    if (text != small) {
        free(text);
    }
    return n;
}

int dprintf(int fd, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vdprintf(fd, f, args);
    va_end(args);
    return n;
}

int vasprintf(char **out, const char *f, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, f, copy);
    va_end(copy);
    *out = n < 0 ? NULL : malloc((size_t)n + 1);
    if (!*out) {
        return -1;
    }
    return vsnprintf(*out, (size_t)n + 1, f, args);
}

int asprintf(char **out, const char *f, ...) {
    va_list args;
    va_start(args, f);
    int n = vasprintf(out, f, args);
    va_end(args);
    return n;
}

/* ---- Positions ---- */

int fseeko(FILE *file, off_t offset, int whence) {
    vx_mutex_lock(&file->lock);
    if (file->write_used && flush_one(file)) {
        vx_mutex_unlock(&file->lock);
        return -1;
    }
    if (whence == SEEK_CUR) { /* Relative to what the program has read. */
        offset -= (off_t)(file->read_len - file->read_pos + (file->has_ungot ? 1 : 0));
    }
    file->read_pos = file->read_len = 0;
    file->has_ungot = false;
    long result = raw_seek(file, (long)offset, whence);
    if (result >= 0) {
        file->eof = false;
    }
    vx_mutex_unlock(&file->lock);
    return result < 0 ? (int)__vx_errno_result(result) : 0;
}

int fseek(FILE *file, long offset, int whence) {
    return fseeko(file, offset, whence);
}

off_t ftello(FILE *file) {
    vx_mutex_lock(&file->lock);
    long position = raw_seek(file, 0, VX_SEEK_CURRENT);
    if (position >= 0) {
        position += (long)file->write_used;
        position -= (long)(file->read_len - file->read_pos + (file->has_ungot ? 1 : 0));
    }
    vx_mutex_unlock(&file->lock);
    return position < 0 ? __vx_errno_result(position) : position;
}

long ftell(FILE *file) {
    return ftello(file);
}

void rewind(FILE *file) {
    fseeko(file, 0, SEEK_SET);
    file->error = false;
}

int fgetpos(FILE *file, fpos_t *position) {
    off_t at = ftello(file);
    if (at < 0) {
        return -1;
    }
    *position = at;
    return 0;
}

int fsetpos(FILE *file, const fpos_t *position) {
    return fseeko(file, *position, SEEK_SET);
}

int ungetc(int c, FILE *file) {
    if (c == EOF) {
        return EOF;
    }
    vx_mutex_lock(&file->lock);
    int result = EOF;
    if (file->read_pos > 0 && !file->has_ungot &&
        (unsigned char)file->buffer[file->read_pos - 1] == (unsigned char)c) {
        file->read_pos--; /* The usual case: put back what was just read. */
        result = (unsigned char)c;
    } else if (!file->has_ungot) {
        file->has_ungot = true;
        file->ungot = (unsigned char)c;
        result = (unsigned char)c;
    }
    if (result != EOF) {
        file->eof = false;
    }
    vx_mutex_unlock(&file->lock);
    return result;
}

void clearerr(FILE *file) {
    file->eof = false;
    file->error = false;
}

int setvbuf(FILE *file, char *buffer, int mode, size_t size) {
    vx_mutex_lock(&file->lock);
    if (file->write_used) {
        flush_one(file);
    }
    if (mode == _IONBF) {
        size = 1;
    } else if (size == 0) {
        size = BUFSIZ;
    }
    if (!buffer || mode == _IONBF) {
        buffer = malloc(size);
        if (!buffer) {
            vx_mutex_unlock(&file->lock);
            return -1;
        }
        if (file->own_buffer) {
            free(file->buffer);
        }
        file->own_buffer = true;
    } else {
        if (file->own_buffer) {
            free(file->buffer);
        }
        file->own_buffer = false;
    }
    /* The standard streams start with static buffers they don't own. */
    file->buffer = buffer;
    file->size = size;
    file->read_pos = file->read_len = 0;
    file->buffering = mode == _IONBF ? UNBUFFERED : mode == _IOLBF ? LINE_BUFFERED : FULLY_BUFFERED;
    vx_mutex_unlock(&file->lock);
    return 0;
}

void setbuf(FILE *file, char *buffer) {
    setvbuf(file, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
}

FILE *freopen(const char *path, const char *mode, FILE *file) {
    bool readable = false, writable = false;
    unsigned flags = mode_flags(mode, &readable, &writable);
    if (!flags || !path) {
        return NULL;
    }
    fflush(file);
    int handle = vx_open(path, flags);
    if (handle < 0) {
        __vx_errno_result(handle);
        return NULL;
    }
    vx_mutex_lock(&file->lock);
    vx_close(file->handle);
    file->handle = handle;
    file->readable = readable;
    file->writable = writable;
    file->eof = file->error = file->has_ungot = false;
    file->read_pos = file->read_len = file->write_used = 0;
    if (!file->buffer) { /* (stderr is unbuffered with none.) */
        file->buffering = UNBUFFERED;
    }
    vx_mutex_unlock(&file->lock);
    return file;
}

FILE *tmpfile(void) {
    char path[] = "/tmp/tmpfile-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }
    vx_remove(path); /* Gone once closed (or left behind if open files can't be removed). */
    FILE *file = new_file(fd, true, true);
    if (!file) {
        vx_close(fd);
    }
    return file;
}

void perror(const char *what) {
    const char *message = strerror(errno);
    if (what && *what) {
        fprintf(stderr, "%s: %s\n", what, message);
    } else {
        fprintf(stderr, "%s\n", message);
    }
}

ssize_t getdelim(char **line, size_t *size, int delimiter, FILE *file) {
    if (!line || !size) {
        errno = EINVAL;
        return -1;
    }
    size_t used = 0;
    vx_mutex_lock(&file->lock);
    for (;;) {
        int c = fgetc_unlocked(file);
        if (c == EOF) {
            if (used == 0) {
                vx_mutex_unlock(&file->lock);
                return -1;
            }
            break;
        }
        if (used + 2 > *size) {
            size_t grown = *size < 64 ? 128 : *size * 2;
            char *bigger = realloc(*line, grown);
            if (!bigger) {
                vx_mutex_unlock(&file->lock);
                errno = ENOMEM;
                return -1;
            }
            *line = bigger;
            *size = grown;
        }
        (*line)[used++] = (char)c;
        if (c == delimiter) {
            break;
        }
    }
    (*line)[used] = '\0';
    vx_mutex_unlock(&file->lock);
    return (ssize_t)used;
}

ssize_t getline(char **line, size_t *size, FILE *file) {
    return getdelim(line, size, '\n', file);
}

FILE *popen(const char *command, const char *mode) {
    bool reading = mode[0] == 'r';
    if (!reading && mode[0] != 'w') {
        errno = EINVAL;
        return NULL;
    }
    int ends[2];
    long result = vx_pipe(ends);
    if (result < 0) {
        __vx_errno_result(result);
        return NULL;
    }
    /* ends[0] reads, ends[1] writes; the program gets the other end. */
    int ours = reading ? ends[0] : ends[1];
    int theirs = reading ? ends[1] : ends[0];
    const char *argv[] = {"vsh", "-c", command, NULL};
    unsigned long envc = 0;
    while (environ && environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {.argv = argv, .argc = 3, .envp = (const char *const *)environ,
                             .envc = envc,
                             .handles = {reading ? 0 : theirs, reading ? theirs : 1, 2}};
    int process = vx_spawn("/bin/vsh", &spawn);
    vx_close(theirs);
    if (process < 0) {
        vx_close(ours);
        __vx_errno_result(process);
        return NULL;
    }
    FILE *file = new_file(ours, reading, !reading);
    if (!file) {
        vx_close(ours);
        vx_kill(vx_handle_process_id(process), 9);
        vx_wait(process, 0);
        vx_close(process);
        return NULL;
    }
    file->process = process;
    return file;
}

int pclose(FILE *file) {
    return fclose(file);
}

/* Each call already locks the file, and its lock isn't recursive, so these
 * only promise what a single call does. */
void flockfile(FILE *file) {
    (void)file;
}

void funlockfile(FILE *file) {
    (void)file;
}

/* ---- Files that aren't files: fopencookie, open_memstream, fmemopen ---- */

FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io) {
    bool readable = false, writable = false;
    if (!mode_flags(mode, &readable, &writable)) {
        errno = EINVAL;
        return NULL;
    }
    FILE *file = new_file(-1, readable, writable);
    if (!file) {
        errno = ENOMEM;
        return NULL;
    }
    file->cookie_file = true;
    file->cookie = cookie;
    file->io = io;
    return file;
}

/* open_memstream: what's written goes into a growing buffer, which *bufp
 * and *sizep show after each fflush (and fclose), with a 0 after it. */
struct memstream {
    char **bufp;
    size_t *sizep;
    char *data;
    size_t capacity, length, position;
};

static int memstream_reserve(struct memstream *m, size_t needed) {
    if (needed + 1 <= m->capacity) {
        return 0;
    }
    size_t capacity = m->capacity ? m->capacity : 256;
    while (capacity < needed + 1) {
        capacity *= 2;
    }
    char *data = realloc(m->data, capacity);
    if (!data) {
        return -1;
    }
    memset(data + m->capacity, 0, capacity - m->capacity);
    m->data = data;
    m->capacity = capacity;
    *m->bufp = data;
    return 0;
}

static ssize_t memstream_write(void *cookie, const char *data, size_t n) {
    struct memstream *m = cookie;
    if (memstream_reserve(m, m->position + n)) {
        return -1;
    }
    memcpy(m->data + m->position, data, n);
    m->position += n;
    if (m->position > m->length) {
        m->length = m->position;
    }
    m->data[m->length] = 0;
    *m->sizep = m->position;
    return (ssize_t)n;
}

static int memstream_seek(void *cookie, off_t *offset, int whence) {
    struct memstream *m = cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->position : (off_t)m->length;
    off_t position = base + *offset;
    if (position < 0 || memstream_reserve(m, (size_t)position)) {
        return -1;
    }
    m->position = (size_t)position;
    *m->sizep = m->position;
    *offset = position;
    return 0;
}

static int memstream_close(void *cookie) {
    free(cookie); /* (The buffer is the caller's now.) */
    return 0;
}

FILE *open_memstream(char **bufp, size_t *sizep) {
    struct memstream *m = calloc(1, sizeof(*m));
    if (!m) {
        errno = ENOMEM;
        return NULL;
    }
    m->bufp = bufp;
    m->sizep = sizep;
    *sizep = 0;
    *bufp = NULL;
    if (memstream_reserve(m, 0)) {
        free(m);
        errno = ENOMEM;
        return NULL;
    }
    FILE *file = fopencookie(m, "w", (cookie_io_functions_t){
                                         .write = memstream_write,
                                         .seek = memstream_seek,
                                         .close = memstream_close,
                                     });
    if (!file) {
        free(m->data);
        free(m);
    }
    return file;
}

/* fmemopen: a fixed buffer as a file (the caller's, or one of its own). */
struct memfile {
    char *data;
    size_t size, length, position;
    bool own;
};

static ssize_t memfile_read(void *cookie, char *data, size_t n) {
    struct memfile *m = cookie;
    size_t left = m->position < m->length ? m->length - m->position : 0;
    n = n < left ? n : left;
    memcpy(data, m->data + m->position, n);
    m->position += n;
    return (ssize_t)n;
}

static ssize_t memfile_write(void *cookie, const char *data, size_t n) {
    struct memfile *m = cookie;
    size_t room = m->position < m->size ? m->size - m->position : 0;
    n = n < room ? n : room;
    memcpy(m->data + m->position, data, n);
    m->position += n;
    if (m->position > m->length) {
        m->length = m->position;
        if (m->length < m->size) {
            m->data[m->length] = 0;
        }
    }
    return (ssize_t)n;
}

static int memfile_seek(void *cookie, off_t *offset, int whence) {
    struct memfile *m = cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->position : (off_t)m->length;
    off_t position = base + *offset;
    if (position < 0 || (size_t)position > m->size) {
        return -1;
    }
    m->position = (size_t)position;
    *offset = position;
    return 0;
}

static int memfile_close(void *cookie) {
    struct memfile *m = cookie;
    if (m->own) {
        free(m->data);
    }
    free(m);
    return 0;
}

FILE *fmemopen(void *buffer, size_t size, const char *mode) {
    struct memfile *m = calloc(1, sizeof(*m));
    if (!m || size == 0) {
        free(m);
        errno = m ? EINVAL : ENOMEM;
        return NULL;
    }
    m->size = size;
    m->data = buffer;
    if (!buffer) {
        m->data = calloc(1, size);
        m->own = true;
        if (!m->data) {
            free(m);
            errno = ENOMEM;
            return NULL;
        }
    }
    switch (mode[0]) {
    case 'r':
        m->length = size;
        break;
    case 'w':
        m->data[0] = 0;
        break;
    case 'a':
        m->length = strnlen(m->data, size);
        m->position = m->length;
        break;
    }
    FILE *file = fopencookie(m, mode, (cookie_io_functions_t){
                                          .read = memfile_read,
                                          .write = memfile_write,
                                          .seek = memfile_seek,
                                          .close = memfile_close,
                                      });
    if (!file) {
        memfile_close(m);
    }
    return file;
}

/* getc and friends as functions (stdio.h has them as macros too; C++'s
 * <cstdio> uses these). */
#undef getc
#undef putc
#undef getc_unlocked
#undef putc_unlocked
#undef getchar_unlocked
#undef putchar_unlocked

int getc(FILE *file) {
    return fgetc(file);
}

int putc(int c, FILE *file) {
    return fputc(c, file);
}

int getc_unlocked(FILE *file) {
    return fgetc(file);
}

int putc_unlocked(int c, FILE *file) {
    return fputc(c, file);
}

int getchar_unlocked(void) {
    return getchar();
}

int putchar_unlocked(int c) {
    return putchar(c);
}
