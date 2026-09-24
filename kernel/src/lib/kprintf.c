#include <stdbool.h>
#include <stdint.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>
#include <vexa/serial.h>

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
        serial_putc(buf[i]);
    }
}

void kvprintf(const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            serial_putc(*fmt);
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
            serial_write(s ? s : "(null)");
            break;
        }
        case 'c':
            serial_putc((char)va_arg(args, int));
            break;
        case 'd':
        case 'i': {
            int64_t v = is_long ? va_arg(args, int64_t) : va_arg(args, int);
            if (v < 0) {
                serial_putc('-');
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
            serial_write("0x");
            put_unsigned((uint64_t)va_arg(args, void *), 16, 16);
            break;
        case '%':
            serial_putc('%');
            break;
        case '\0':
            return;
        default:
            serial_putc('%');
            serial_putc(*fmt);
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
    serial_write("\n*** VEXA KERNEL PANIC ***\n");
    kvprintf(fmt, args);
    serial_write("\nSystem halted.\n");
    va_end(args);
    cpu_halt_forever();
}
