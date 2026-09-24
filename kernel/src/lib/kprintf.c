#include <stdbool.h>
#include <stdint.h>
#include <vexa/console.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/serial.h>

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

static void put_unsigned(uint64_t value, unsigned base, int min_digits) {
    static const char digits[] = "0123456789abcdef";
    char buf[32];
    int i = 0;
    do {
        buf[i++] = digits[value % base];
        value /= base;
    } while (value);
    while (i < min_digits) {
        buf[i++] = '0';
    }
    while (i--) {
        kputc(buf[i]);
    }
}

void kvprintf(const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputc(*fmt);
            continue;
        }
        fmt++;
        bool is_long = false;
        while (*fmt == 'l') {
            is_long = true;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'c':
            kputc((char)va_arg(args, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = is_long ? va_arg(args, int64_t) : va_arg(args, int);
            if (v < 0) {
                kputc('-');
                put_unsigned((uint64_t)-v, 10, 1);
            } else {
                put_unsigned((uint64_t)v, 10, 1);
            }
            break;
        }
        case 'u':
            put_unsigned(is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 10, 1);
            break;
        case 'x':
            put_unsigned(is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned), 16, 1);
            break;
        case 'p':
            kputs("0x");
            put_unsigned((uint64_t)va_arg(args, void *), 16, 16);
            break;
        case '%':
            kputc('%');
            break;
        case '\0':
            return;
        default:
            kputc('%');
            kputc(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}

void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    console_set_color(CONSOLE_COLOR_ERROR);
    kputs("\n*** VEXA KERNEL PANIC ***\n");
    kvprintf(fmt, args);
    kputs("\nSystem halted.\n");
    va_end(args);
    cpu_halt_forever();
}
