#include <stdint.h>
#include <vexa/device.h>
#include <vexa/arch.h>
#include <vexa/input.h>
#include <vexa/io.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/sched.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02
#define STATUS_MOUSE_DATA 0x20 /* The waiting byte is from the mouse. */

#define CMD_READ_CONFIG 0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_DISABLE_PORT2 0xa7
#define CMD_DISABLE_PORT1 0xad
#define CMD_ENABLE_PORT1 0xae

#define CONFIG_PORT1_IRQ 0x01
#define CONFIG_PORT2_IRQ 0x02
#define CONFIG_TRANSLATION 0x40

#define KBD_ENABLE_SCANNING 0xf4
#define KBD_ACK 0xfa
#define KBD_RESEND 0xfe

#define TIMEOUT 100000

#define SC_EXTENDED 0xe0
#define SC_RELEASED 0x80

static bool extended;

/* Keys go to the keyboards' shared part (core/keyboard.c) with Linux's key
 * codes: for the keys of scancode set 1, the code is the scancode itself;
 * extended (E0) keys have their own. */
static int set_repeat(const struct vx_key_repeat *repeat);

static struct input_device keyboard_device = {
    .name = "PS/2 keyboard",
    .capabilities = VX_INPUT_KEYS,
    .set_repeat = set_repeat,
};
static uint8_t keys_down[256 / 8];

static uint16_t extended_keycode(uint8_t code) {
    switch (code) {
    case 0x1c: return 96;  /* Keypad Enter */
    case 0x1d: return VX_KEY_RIGHTCTRL;
    case 0x35: return 98;  /* Keypad / */
    case 0x37: return 99;  /* PrintScreen (SysRq) */
    case 0x38: return VX_KEY_RIGHTALT;
    case 0x47: return VX_KEY_HOME;
    case 0x48: return VX_KEY_UP;
    case 0x49: return VX_KEY_PAGEUP;
    case 0x4b: return VX_KEY_LEFT;
    case 0x4d: return VX_KEY_RIGHT;
    case 0x4f: return VX_KEY_END;
    case 0x50: return VX_KEY_DOWN;
    case 0x51: return VX_KEY_PAGEDOWN;
    case 0x52: return VX_KEY_INSERT;
    case 0x53: return VX_KEY_DELETE;
    case 0x5b: return VX_KEY_LEFTMETA;
    case 0x5c: return 126; /* Right Meta */
    case 0x5d: return 127; /* Compose (menu) */
    default: return 0;
    }
}

static void report_key(uint16_t keycode, bool released) {
    if (!keycode || keycode > 255) {
        return;
    }
    bool down = keys_down[keycode / 8] & (1 << (keycode % 8));
    int value = released ? 0 : down ? 2 : 1; /* 2: the key repeats while held. */
    if (released) {
        keys_down[keycode / 8] &= (uint8_t)~(1 << (keycode % 8));
    } else {
        keys_down[keycode / 8] |= (uint8_t)(1 << (keycode % 8));
    }
    keyboard_key(&keyboard_device, keycode, value);
}

static bool wait_input_empty(void) {
    for (int i = 0; i < TIMEOUT; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) {
            return true;
        }
    }
    return false;
}

bool ps2_wait_output(void) {
    for (int i = 0; i < TIMEOUT; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            return true;
        }
    }
    return false;
}

bool ps2_command(uint8_t command) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_COMMAND, command);
    return true;
}

bool ps2_write(uint8_t value) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_DATA, value);
    return true;
}

/* The keyboard's typematic rate and delay (command 0xF3): the rate, in
 * tenths of a key a second, for each of the 32 settings. */
static const uint16_t typematic_rates[32] = {
    300, 267, 240, 218, 207, 185, 171, 160, 150, 133, 120, 109, 100, 92, 86, 80,
    75, 67, 60, 55, 50, 46, 43, 40, 37, 33, 30, 27, 25, 23, 21, 20,
};

static int set_repeat(const struct vx_key_repeat *repeat) {
    if (repeat->delay_ms < 250 || repeat->delay_ms > 1000 || repeat->rate < 2 ||
        repeat->rate > 30) {
        return -VX_EINVAL;
    }
    uint8_t delay = (uint8_t)((repeat->delay_ms - 125) / 250); /* 250, 500, 750, 1000 */
    uint8_t rate = 31;
    for (uint8_t i = 0; i < 32; i++) {
        if (typematic_rates[i] <= repeat->rate * 10) {
            rate = i; /* The fastest that isn't faster than asked. */
            break;
        }
    }
    /* The keyboard acknowledges each byte; the interrupt handler drops the
     * acknowledgements. */
    if (!ps2_write(0xf3) || !ps2_write((uint8_t)(delay << 5 | rate))) {
        return -VX_EIO;
    }
    return 0;
}

struct device *ps2_controller_node(void) {
    static struct device *node;
    if (!node) {
        node = device_add(NULL, VX_BUS_PLATFORM, VX_DEVICE_SYSTEM, "PS/2 controller");
        device_set_driver(node, "ps2");
        device_set_location(node, "ports 0x60, 0x64");
    }
    return node;
}

static void handle_scancode(uint8_t scancode) {
    if (scancode == KBD_ACK || scancode == KBD_RESEND) {
        return;
    }
    if (scancode == SC_EXTENDED) {
        extended = true;
        return;
    }
    bool released = scancode & SC_RELEASED;
    uint8_t code = scancode & ~SC_RELEASED;
    if (extended) {
        extended = false;
        if (code != 0x2a && code != 0x36) { /* Fake shifts around some E0 keys. */
            report_key(extended_keycode(code), released);
        }
        return;
    }
    /* 0x54: SysRq, PrintScreen with Alt held. */
    report_key(code == 0x54 ? 99 : code < 0x59 ? code : 0, released);
}

/* Both PS/2 interrupts drain the controller: a byte goes to the keyboard or
 * the mouse by where it came from. */
void ps2_drain(void) {
    uint8_t status;
    while ((status = inb(PS2_STATUS)) & STATUS_OUTPUT_FULL) {
        uint8_t byte = inb(PS2_DATA);
        if (status & STATUS_MOUSE_DATA) {
            ps2_mouse_byte(byte);
        } else {
            handle_scancode(byte);
        }
    }
}

static void keyboard_irq(struct interrupt_frame *frame) {
    (void)frame;
    ps2_drain();
}

bool keyboard_init(void) {
    if (inb(PS2_STATUS) == 0xff) {
        kprintf("[kbd] no PS/2 controller\n");
        return false;
    }
    if (!ps2_command(CMD_DISABLE_PORT1) || !ps2_command(CMD_DISABLE_PORT2)) {
        kprintf("[kbd] PS/2 controller not responding\n");
        return false;
    }
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++) {
        inb(PS2_DATA); /* Flush stale bytes. */
    }

    ps2_command(CMD_READ_CONFIG);
    if (!ps2_wait_output()) {
        kprintf("[kbd] could not read PS/2 controller configuration\n");
        return false;
    }
    uint8_t config = inb(PS2_DATA);
    config |= CONFIG_PORT1_IRQ | CONFIG_TRANSLATION;
    config &= ~CONFIG_PORT2_IRQ;
    ps2_command(CMD_WRITE_CONFIG);
    ps2_write(config);
    ps2_command(CMD_ENABLE_PORT1);

    ps2_write(KBD_ENABLE_SCANNING);
    if (ps2_wait_output()) {
        inb(PS2_DATA); /* The keyboard's acknowledgement. */
    }

    isa_irq_enable(1, keyboard_irq);
    kprintf("[kbd] PS/2 keyboard ready\n");
    keyboard_device.parent = ps2_controller_node();
    input_register(&keyboard_device);
    ps2_mouse_init();
    return true;
}
