#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/io.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/sched.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL 0x02

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

#define BUFFER_SIZE 128
#define TIMEOUT 100000

/* Scancode set 1 (what the controller produces with translation on), US layout. */
static const char keymap_normal[0x3a] =
    "\0\x1b" "1234567890-=\b"
    "\tqwertyuiop[]\n"
    "\0" "asdfghjkl;'`"
    "\0" "\\zxcvbnm,./"
    "\0" "*" "\0" " ";
static const char keymap_shift[0x3a] =
    "\0\x1b" "!@#$%^&*()_+\b"
    "\tQWERTYUIOP{}\n"
    "\0" "ASDFGHJKL:\"~"
    "\0" "|ZXCVBNM<>?"
    "\0" "*" "\0" " ";

#define SC_LCTRL 0x1d
#define SC_LSHIFT 0x2a
#define SC_RSHIFT 0x36
#define SC_CAPSLOCK 0x3a
#define SC_EXTENDED 0xe0
#define SC_RELEASED 0x80

/* Ring buffer filled by the interrupt handler, drained by keyboard_read(). */
static volatile int buffer[BUFFER_SIZE];
static volatile uint32_t buffer_head, buffer_tail;

static bool shift_left, shift_right, ctrl, caps_lock, extended;
static struct wait_queue key_waiters = WAIT_QUEUE_INIT;

static bool wait_input_empty(void) {
    for (int i = 0; i < TIMEOUT; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) {
            return true;
        }
    }
    return false;
}

static bool wait_output_full(void) {
    for (int i = 0; i < TIMEOUT; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            return true;
        }
    }
    return false;
}

static bool controller_command(uint8_t command) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_COMMAND, command);
    return true;
}

static bool write_data(uint8_t value) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_DATA, value);
    return true;
}

static void push_key(int key) {
    uint32_t next = (buffer_head + 1) % BUFFER_SIZE;
    if (next != buffer_tail) { /* Drop keys when the buffer is full. */
        buffer[buffer_head] = key;
        buffer_head = next;
    }
}

static void handle_extended(uint8_t code, bool released) {
    if (code == SC_LCTRL) { /* Right Ctrl. */
        ctrl = !released;
        return;
    }
    if (released) {
        return;
    }
    switch (code) {
    case 0x48: push_key(KEY_UP); break;
    case 0x50: push_key(KEY_DOWN); break;
    case 0x4b: push_key(KEY_LEFT); break;
    case 0x4d: push_key(KEY_RIGHT); break;
    case 0x1c: push_key('\n'); break; /* Keypad Enter. */
    case 0x35: push_key('/'); break;  /* Keypad slash. */
    }
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
        handle_extended(code, released);
        return;
    }

    switch (code) {
    case SC_LSHIFT: shift_left = !released; return;
    case SC_RSHIFT: shift_right = !released; return;
    case SC_LCTRL: ctrl = !released; return;
    case SC_CAPSLOCK:
        if (!released) {
            caps_lock = !caps_lock;
        }
        return;
    }
    if (released || code >= sizeof(keymap_normal)) {
        return;
    }

    bool shift = shift_left || shift_right;
    char c = shift ? keymap_shift[code] : keymap_normal[code];
    if (!c) {
        return;
    }
    if (caps_lock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c ^= 0x20; /* Caps Lock flips the case of letters only. */
    }
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c &= 0x1f; /* Ctrl+A = 1 ... Ctrl+Z = 26, as on a terminal. */
    }
    push_key(c);
}

static void keyboard_irq(struct interrupt_frame *frame) {
    (void)frame;
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
        handle_scancode(inb(PS2_DATA));
    }
    if (buffer_head != buffer_tail) {
        wait_queue_wake_all(&key_waiters);
    }
}

bool keyboard_init(void) {
    if (inb(PS2_STATUS) == 0xff) {
        kprintf("[kbd] no PS/2 controller\n");
        return false;
    }
    if (!controller_command(CMD_DISABLE_PORT1) || !controller_command(CMD_DISABLE_PORT2)) {
        kprintf("[kbd] PS/2 controller not responding\n");
        return false;
    }
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & STATUS_OUTPUT_FULL); i++) {
        inb(PS2_DATA); /* Flush stale bytes. */
    }

    controller_command(CMD_READ_CONFIG);
    if (!wait_output_full()) {
        kprintf("[kbd] could not read PS/2 controller configuration\n");
        return false;
    }
    uint8_t config = inb(PS2_DATA);
    config |= CONFIG_PORT1_IRQ | CONFIG_TRANSLATION;
    config &= ~CONFIG_PORT2_IRQ;
    controller_command(CMD_WRITE_CONFIG);
    write_data(config);
    controller_command(CMD_ENABLE_PORT1);

    write_data(KBD_ENABLE_SCANNING);
    if (wait_output_full()) {
        inb(PS2_DATA); /* The keyboard's acknowledgement. */
    }

    isa_irq_enable(1, keyboard_irq);
    kprintf("[kbd] PS/2 keyboard ready\n");
    return true;
}

static bool key_waiting(void *unused) {
    (void)unused;
    return buffer_head != buffer_tail;
}

int keyboard_read_blocking(void) {
    for (;;) {
        int key = keyboard_read();
        if (key >= 0) {
            return key;
        }
        wait_queue_wait(&key_waiters, key_waiting, NULL);
    }
}

int keyboard_read(void) {
    if (buffer_tail == buffer_head) {
        return -1;
    }
    int key = buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    return key;
}
