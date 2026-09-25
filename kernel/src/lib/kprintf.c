#include <stdbool.h>
#include <stdint.h>
#include <vexa/console.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/serial.h>
#include <vexa/spinlock.h>

/* Keeps messages from different CPUs from interleaving mid-line. */
static struct spinlock output_lock = SPINLOCK_INIT;

void smp_stop_other_cpus(void); /* smp.c */

/* Every kernel message goes to both the serial port and the screen. */
static void kputc(char c) {
    serial_putc(c);
    console_putc(c);
}

static void kputs(const char *s) {
    while (*s) {
        kputc(*s++);
    }
}

/* The formatter writes through a sink: the console, or a buffer (ksnprintf). */
struct sink {
    void (*put)(struct sink *sink, char c);
    char *buffer;
    size_t size, length;
};

static void console_sink(struct sink *sink, char c) {
    (void)sink;
    kputc(c);
}

static void buffer_sink(struct sink *sink, char c) {
    if (sink->length + 1 < sink->size) {
        sink->buffer[sink->length] = c;
    }
    sink->length++;
}

static void put_string(struct sink *sink, const char *s) {
    while (*s) {
        sink->put(sink, *s++);
    }
}

static void put_unsigned(struct sink *sink, uint64_t value, unsigned base, int min_digits) {
    static const char digits[] = "0123456789abcdef0123456789ABCDEF";
    unsigned offset = base > 16 ? 16 : 0; /* Base 32 here means "16, in capitals". */
    base -= offset;
    char buf[32];
    int i = 0;
    do {
        buf[i++] = digits[offset + value % base];
        value /= base;
    } while (value);
    while (i < min_digits) {
        buf[i++] = '0';
    }
    while (i--) {
        sink->put(sink, buf[i]);
    }
}

static void format(struct sink *sink, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink->put(sink, *fmt);
            continue;
        }
        fmt++;
        /* A zero-padded width, for numbers: %02u, %08lx. */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        bool is_long = false;
        while (*fmt == 'l') {
            is_long = true;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            put_string(sink, s ? s : "(null)");
            break;
        }
        case 'c':
            sink->put(sink, (char)va_arg(args, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = is_long ? va_arg(args, int64_t) : va_arg(args, int);
            if (v < 0) {
                sink->put(sink, '-');
                put_unsigned(sink, (uint64_t)-v, 10, width ? width : 1);
            } else {
                put_unsigned(sink, (uint64_t)v, 10, width ? width : 1);
            }
            break;
        }
        case 'u':
            put_unsigned(sink, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 10,
                         width ? width : 1);
            break;
        case 'x':
            put_unsigned(sink, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 16,
                         width ? width : 1);
            break;
        case 'X':
            put_unsigned(sink, is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 32,
                         width ? width : 1);
            break;
        case 'p':
            put_string(sink, "0x");
            put_unsigned(sink, (uint64_t)va_arg(args, void *), 16, 16);
            break;
        case '%':
            sink->put(sink, '%');
            break;
        case '\0':
            return;
        default:
            sink->put(sink, '%');
            sink->put(sink, *fmt);
            break;
        }
    }
}

void kvprintf(const char *fmt, va_list args) {
    struct sink sink = {.put = console_sink};
    format(&sink, fmt, args);
}

size_t kvsnprintf(char *buffer, size_t size, const char *fmt, va_list args) {
    struct sink sink = {.put = buffer_sink, .buffer = buffer, .size = size};
    format(&sink, fmt, args);
    if (size) {
        buffer[sink.length < size ? sink.length : size - 1] = '\0';
    }
    return sink.length;
}

size_t ksnprintf(char *buffer, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    size_t length = kvsnprintf(buffer, size, fmt, args);
    va_end(args);
    return length;
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    uint64_t flags = spin_lock_irqsave(&output_lock);
    kvprintf(fmt, args);
    spin_unlock_irqrestore(&output_lock, flags);
    va_end(args);
}

void kwrite(const char *text, size_t length) {
    uint64_t flags = spin_lock_irqsave(&output_lock);
    for (size_t i = 0; i < length; i++) {
        kputc(text[i]);
    }
    spin_unlock_irqrestore(&output_lock, flags);
}

void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __asm__ volatile("cli");
    smp_stop_other_cpus();
    /* Whoever held the output lock is stopped now; take it over. */
    spin_unlock(&output_lock);
    console_set_color(CONSOLE_COLOR_ERROR);
    kputs("\n*** VEXA KERNEL PANIC ***\n");
    kvprintf(fmt, args);
    kputs("\nSystem halted.\n");
    va_end(args);
    cpu_halt_forever();
}
