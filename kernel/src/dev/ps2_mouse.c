#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/input.h>
#include <vexa/io.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/spinlock.h>

/*
 * PS/2 mouse, on the second port of the keyboard controller (IRQ 12). It
 * sends 3-byte packets (buttons, X, Y), or 4 bytes with a scroll wheel once
 * switched to the IntelliMouse protocol, which become input events on
 * /dev/input/event1.
 */

#define PS2_DATA 0x60
#define PS2_STATUS 0x64

#define CMD_READ_CONFIG 0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_ENABLE_PORT2 0xa8
#define CMD_WRITE_PORT2 0xd4

#define CONFIG_PORT2_IRQ 0x02
#define CONFIG_PORT2_CLOCK_OFF 0x20

#define MOUSE_SET_DEFAULTS 0xf6
#define MOUSE_ENABLE 0xf4
#define MOUSE_SAMPLE_RATE 0xf3
#define MOUSE_GET_ID 0xf2
#define MOUSE_ACK 0xfa

static struct input_device mouse_device = {
    .name = "PS/2 mouse",
    .capabilities = VX_INPUT_POINTER,
};

static bool ready, wheel;
static uint8_t packet[4];
static int packet_length;
static uint8_t buttons;

/* Sends a byte to the mouse and waits for its acknowledgement. */
static bool mouse_send(uint8_t value) {
    if (!ps2_command(CMD_WRITE_PORT2) || !ps2_write(value) || !ps2_wait_output()) {
        return false;
    }
    return inb(PS2_DATA) == MOUSE_ACK;
}

static int mouse_reply(void) {
    return ps2_wait_output() ? inb(PS2_DATA) : -1;
}

static void report_button(uint8_t now, uint8_t bit, uint16_t code) {
    if ((now ^ buttons) & bit) {
        input_report(&mouse_device, VX_EV_KEY, code, (now & bit) ? 1 : 0);
    }
}

static void handle_packet(void) {
    uint8_t flags = packet[0];
    if (flags & 0xc0) {
        return; /* Overflow: the motion is meaningless. */
    }
    int dx = packet[1] - ((flags << 4) & 0x100);
    int dy = packet[2] - ((flags << 3) & 0x100);
    int dz = wheel ? (int8_t)(packet[3] << 4) >> 4 : 0; /* 4-bit signed */
    uint8_t now = flags & 0x7;
    report_button(now, 0x1, VX_BTN_LEFT);
    report_button(now, 0x2, VX_BTN_RIGHT);
    report_button(now, 0x4, VX_BTN_MIDDLE);
    buttons = now;
    if (dx) {
        input_report(&mouse_device, VX_EV_REL, VX_REL_X, dx);
    }
    if (dy) {
        input_report(&mouse_device, VX_EV_REL, VX_REL_Y, -dy); /* PS/2 counts up as positive. */
    }
    if (dz) {
        input_report(&mouse_device, VX_EV_REL, VX_REL_WHEEL, -dz);
    }
    input_sync(&mouse_device);
}

void ps2_mouse_byte(uint8_t byte) {
    if (!ready) {
        return;
    }
    if (packet_length == 0 && !(byte & 0x08)) {
        return; /* Bit 3 of the first byte is always set: resynchronize. */
    }
    packet[packet_length++] = byte;
    if (packet_length == (wheel ? 4 : 3)) {
        packet_length = 0;
        handle_packet();
    }
}

static void mouse_irq(struct interrupt_frame *frame) {
    (void)frame;
    ps2_drain();
}

void ps2_mouse_init(void) {
    /* Talk to the mouse with interrupts off, so replies aren't taken for
     * packets by the keyboard's interrupt handler. */
    static struct spinlock lock = SPINLOCK_INIT;
    uint64_t flags = spin_lock_irqsave(&lock);
    bool ok = ps2_command(CMD_ENABLE_PORT2) && ps2_command(CMD_READ_CONFIG);
    int config = ok ? mouse_reply() : -1;
    if (config >= 0) {
        config = (config | CONFIG_PORT2_IRQ) & ~CONFIG_PORT2_CLOCK_OFF;
        ok = ps2_command(CMD_WRITE_CONFIG) && ps2_write((uint8_t)config);
    }
    ok = ok && config >= 0 && mouse_send(MOUSE_SET_DEFAULTS);
    if (ok) {
        /* The IntelliMouse knock: sample rates 200, 100, 80 turn the wheel on. */
        static const uint8_t knock[] = {200, 100, 80};
        bool knocked = true;
        for (int i = 0; i < 3 && knocked; i++) {
            knocked = mouse_send(MOUSE_SAMPLE_RATE) && mouse_send(knock[i]);
        }
        wheel = knocked && mouse_send(MOUSE_GET_ID) && mouse_reply() == 3;
        ok = mouse_send(MOUSE_ENABLE);
    }
    spin_unlock_irqrestore(&lock, flags);
    if (!ok) {
        kprintf("[mouse] no PS/2 mouse\n");
        return;
    }
    ready = true;
    isa_irq_enable(12, mouse_irq);
    input_register(&mouse_device);
    kprintf("[mouse] PS/2 mouse ready%s\n", wheel ? ", with a scroll wheel" : "");
}
