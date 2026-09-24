#include <vexa/io.h>
#include <vexa/serial.h>

/* 16550 UART on COM1. Used as the kernel log until we have a real console. */
#define COM1 0x3f8

static bool serial_ok;

bool serial_init(void) {
    outb(COM1 + 1, 0x00); /* Disable interrupts. */
    outb(COM1 + 3, 0x80); /* Enable DLAB to set the baud divisor. */
    outb(COM1 + 0, 0x01); /* Divisor 1 = 115200 baud. */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit. */
    outb(COM1 + 2, 0xc7); /* Enable and clear FIFOs. */
    outb(COM1 + 4, 0x1e); /* Loopback mode for self-test. */
    outb(COM1 + 0, 0xae);
    if (inb(COM1 + 0) != 0xae) {
        return false;
    }
    outb(COM1 + 4, 0x0f); /* Normal operation. */
    serial_ok = true;
    return true;
}

void serial_putc(char c) {
    if (!serial_ok) {
        return;
    }
    if (c == '\n') {
        serial_putc('\r');
    }
    while (!(inb(COM1 + 5) & 0x20)) {
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}
