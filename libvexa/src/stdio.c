#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

/* ---- Files ---- */

enum buffering { UNBUFFERED, LINE_BUFFERED, FULLY_BUFFERED };

struct vx_file {
    int handle;
    bool readable, writable, eof, error;
    enum buffering buffering;
    char *buffer;
    size_t size;
    size_t write_used;          /* Bytes waiting to be written. */
    size_t read_pos, read_len;  /* Buffered input. */
    struct vx_file *next;
};

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

static int flush_one(FILE *file) {
    size_t done = 0;
    while (done < file->write_used) {
        long n = vx_write(file->handle, file->buffer + done, file->write_used - done);
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
        return file->writable ? flush_one(file) : 0;
    }
    int result = 0;
    for (FILE *f = open_files; f; f = f->next) {
        if (f->writable && flush_one(f)) {
            result = EOF;
        }
    }
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
    file->next = open_files;
    open_files = file;
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
    for (FILE **link = &open_files; *link; link = &(*link)->next) {
        if (*link == file) {
            *link = file->next;
            break;
        }
    }
    vx_close(file->handle);
    free(file->buffer);
    free(file);
    return result;
}

int fileno(FILE *file) {
    return file->handle;
}

int feof(FILE *file) {
    return file->eof;
}

int ferror(FILE *file) {
    return file->error;
}

int fputc(int c, FILE *file) {
    if (!file->writable) {
        file->error = true;
        return EOF;
    }
    if (file->buffering == UNBUFFERED) {
        char ch = (char)c;
        return vx_write(file->handle, &ch, 1) == 1 ? (unsigned char)c : EOF;
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

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *file) {
    const char *p = buffer;
    size_t total = size * count;
    if (file->buffering == UNBUFFERED || (file->write_used == 0 && total >= file->size)) {
        size_t done = 0;
        while (done < total) {
            long n = vx_write(file->handle, p + done, total - done);
            if (n <= 0) {
                file->error = true;
                break;
            }
            done += n;
        }
        return size ? done / size : 0;
    }
    for (size_t i = 0; i < total; i++) {
        if (fputc(p[i], file) == EOF) {
            return size ? i / size : 0;
        }
    }
    return count;
}

int fputs(const char *text, FILE *file) {
    size_t n = strlen(text);
    return fwrite(text, 1, n, file) == n ? 0 : EOF;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *text) {
    return fputs(text, stdout) == 0 && fputc('\n', stdout) != EOF ? 0 : EOF;
}

int fgetc(FILE *file) {
    if (!file->readable) {
        file->error = true;
        return EOF;
    }
    if (file->read_pos == file->read_len) {
        if (file == stdin) {
            fflush(stdout); /* Show any prompt before waiting for input. */
        }
        long n = vx_read(file->handle, file->buffer, file->size);
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

int getchar(void) {
    return fgetc(stdin);
}

char *fgets(char *line, int size, FILE *file) {
    int n = 0;
    while (n < size - 1) {
        int c = fgetc(file);
        if (c == EOF) {
            break;
        }
        line[n++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (n == 0) {
        return NULL;
    }
    line[n] = '\0';
    return line;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *file) {
    char *p = buffer;
    size_t total = size * count, done = 0;
    while (done < total) {
        int c = fgetc(file);
        if (c == EOF) {
            break;
        }
        p[done++] = (char)c;
    }
    return size ? done / size : 0;
}

/* ---- Formatting ---- */

struct sink {
    FILE *file;     /* Either a file... */
    char *out;      /* ...or a buffer of `size` bytes. */
    size_t size;
    size_t count;   /* Characters produced (even past the end of the buffer). */
};

static void emit(struct sink *sink, char c) {
    if (sink->file) {
        fputc(c, sink->file);
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
        bool left = false, zero = false, plus = false, space = false;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') zero = true;
            else if (*f == '+') plus = true;
            else if (*f == ' ') space = true;
            else if (*f == '#') ;
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
        for (;; f++) {
            if (*f == 'l') length++;
            else if (*f == 'h') length--;
            else if (*f == 'z' || *f == 'j' || *f == 't') length = 1;
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
            if (*f == 'p') {
                digits[n++] = '0';
                digits[n++] = 'x';
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
    return format(&sink, f, args);
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
