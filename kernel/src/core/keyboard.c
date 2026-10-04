/*
 * Every keyboard's shared part. Drivers (PS/2, USB) report keys with Linux's
 * key codes through keyboard_key: they go out as input events, and become
 * text for the console (a US layout; the desktop does its own layouts) unless
 * a program has the keyboards to itself. Keyboards that don't repeat held
 * keys themselves (USB ones) get repeats from here, at the rate the desktop
 * asked for.
 */
#include <vexa/arch.h>
#include <vexa/input.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>

#define BUFFER_SIZE 128

/* Key codes 0 to 0x39 are the US layout's keys in order (as scancode set 1). */
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

#define KEY_KPENTER 96
#define KEY_KPSLASH 98

static struct spinlock lock = SPINLOCK_INIT;
static bool shift_left, shift_right, ctrl_left, ctrl_right, caps_lock;

/* Text for whoever reads the console: a consumer (the terminal), or a ring
 * for keyboard_read (the kernel monitor). */
static volatile int buffer[BUFFER_SIZE];
static volatile uint32_t buffer_head, buffer_tail;
static struct wait_queue key_waiters = WAIT_QUEUE_INIT;
static void (*consumer)(int key);

/* How held keys repeat (VX_INPUT_SET_REPEAT on any keyboard). */
static unsigned repeat_delay_ms = 500, repeat_rate = 20;

void keyboard_set_consumer(void (*function)(int key)) {
    consumer = function;
}

static void push_key(int key) {
    if (consumer) {
        consumer(key);
        return;
    }
    uint32_t next = (buffer_head + 1) % BUFFER_SIZE;
    if (next != buffer_tail) { /* Drop keys when the buffer is full. */
        buffer[buffer_head] = key;
        buffer_head = next;
        wait_queue_wake_all(&key_waiters);
    }
}

static bool alt_down;

static bool modifier(uint16_t keycode, bool down) {
    switch (keycode) {
    case VX_KEY_LEFTSHIFT: shift_left = down; return true;
    case VX_KEY_RIGHTSHIFT: shift_right = down; return true;
    case VX_KEY_LEFTCTRL: ctrl_left = down; return true;
    case VX_KEY_RIGHTCTRL: ctrl_right = down; return true;
    case VX_KEY_CAPSLOCK:
        if (down) {
            caps_lock = !caps_lock;
        }
        return true;
    case VX_KEY_LEFTALT:
    case VX_KEY_RIGHTALT:
        alt_down = down;
        return true;
    case VX_KEY_LEFTMETA:
    case 126: /* Right Meta */
        return true;
    default:
        return false;
    }
}

/* What the key types at the console (with the lock held). */
static void console_text(uint16_t keycode) {
    switch (keycode) {
    case VX_KEY_UP: push_key(KEY_UP); return;
    case VX_KEY_DOWN: push_key(KEY_DOWN); return;
    case VX_KEY_LEFT: push_key(KEY_LEFT); return;
    case VX_KEY_RIGHT: push_key(KEY_RIGHT); return;
    case KEY_KPENTER: push_key('\n'); return;
    case KEY_KPSLASH: push_key('/'); return;
    }
    if (keycode >= sizeof(keymap_normal)) {
        return;
    }
    char c = (shift_left || shift_right) ? keymap_shift[keycode] : keymap_normal[keycode];
    if (!c) {
        return;
    }
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (caps_lock && letter) {
        c ^= 0x20; /* Caps Lock flips the case of letters only. */
    }
    if ((ctrl_left || ctrl_right) && letter) {
        c &= 0x1f; /* Ctrl+A = 1 ... Ctrl+Z = 26, as on a terminal. */
    }
    push_key(c);
}

void keyboard_key(struct input_device *device, uint16_t keycode, int value) {
    if (!keycode || keycode > 255) {
        return;
    }
    input_report(device, VX_EV_KEY, keycode, value);
    input_sync(device);

    uint64_t flags = spin_lock_irqsave(&lock);
    bool is_modifier = modifier(keycode, value != 0);
    if (device->soft_repeat) {
        if (value == 1 && !is_modifier) {
            device->repeat_key = keycode;
            device->repeat_at = timer_ms() + repeat_delay_ms;
            device->held_seen = timer_ms();
        } else if (value == 0 && device->repeat_key == keycode) {
            device->repeat_key = 0;
        }
    }
    /* A program (the desktop) with the keyboards to itself: no console text. */
    if (value && !is_modifier && !input_grabbed(device) && !input_grabbed(input_all_keyboards())) {
        console_text(keycode);
    }
    bool dump = value == 1 && keycode == 99 && alt_down; /* Alt+SysRq */
    spin_unlock_irqrestore(&lock, flags);
    if (dump) {
        sched_dump();
    }
}

/* Repeats for the keyboards that don't (see keyboard_key). */
static void repeat_thread(void *unused) {
    (void)unused;
    for (;;) {
        thread_sleep_ms(10);
        uint64_t now = timer_ms();
        for (int i = 0; input_device_at(i); i++) {
            struct input_device *device = input_device_at(i);
            uint16_t key = device->repeat_key;
            if (device->held_reports && now > device->held_seen + 300) {
                continue; /* Not heard from since: the key may be up already. */
            }
            if (device->soft_repeat && device->connected && key && now >= device->repeat_at) {
                device->repeat_at = now + 1000 / repeat_rate;
                keyboard_key(device, key, 2);
            }
        }
    }
}

void keyboard_still_held(struct input_device *device) {
    device->held_seen = timer_ms();
}

void keyboard_soft_repeat(struct input_device *device) {
    static bool started;
    device->soft_repeat = true;
    if (!started) {
        started = true;
        if (!thread_create("keyrepeat", repeat_thread, NULL)) {
            kprintf("[kbd] can't start key repeat\n");
        }
    }
}

int keyboard_set_repeat(const struct vx_key_repeat *repeat) {
    if (repeat->delay_ms < 250 || repeat->delay_ms > 1000 || repeat->rate < 2 ||
        repeat->rate > 30) {
        return -VX_EINVAL;
    }
    repeat_delay_ms = repeat->delay_ms;
    repeat_rate = repeat->rate;
    /* And the keyboards that repeat by themselves. */
    for (int i = 0; input_device_at(i); i++) {
        struct input_device *device = input_device_at(i);
        if (device != input_all_keyboards() && device->connected && device->set_repeat) {
            device->set_repeat(repeat);
        }
    }
    return 0;
}

static bool key_waiting(void *unused) {
    (void)unused;
    return buffer_head != buffer_tail;
}

int keyboard_read(void) {
    if (buffer_tail == buffer_head) {
        return -1;
    }
    int key = buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    return key;
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
