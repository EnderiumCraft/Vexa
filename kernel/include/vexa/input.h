#ifndef VEXA_INPUT_H
#define VEXA_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <vexa/abi.h>
#include <vexa/spinlock.h>

/*
 * Input devices (keyboards, mice): drivers report events, and programs read
 * them from /dev/input/eventN, each open file getting its own copy. Event
 * types and codes are Linux's (see abi/vexa/abi.h), so the Linux subsystem's
 * evdev devices are a thin layer over these.
 */

struct input_client;
struct device;

struct input_device {
    char name[64];
    uint32_t capabilities; /* VX_INPUT_* */
    int index;             /* N in /dev/input/eventN */
    struct spinlock lock;
    struct input_client *clients;
    struct input_client *grab; /* Only this client gets events, if set. */
    /* In the device tree: under `parent` (its controller; NULL: the
     * computer), as `node` (input_register adds it). */
    struct device *parent;
    struct device *node;
    /* Optional: how a held key repeats (VX_INPUT_SET_REPEAT). */
    int (*set_repeat)(const struct vx_key_repeat *repeat);
    /* Set by input: false once unplugged (input_unregister). */
    bool connected;
    bool merged; /* One of the "all keyboards" / "all pointers" devices. */
    /* Keyboards that don't repeat held keys themselves (keyboard_soft_repeat). */
    bool soft_repeat;
    uint16_t repeat_key;
    uint64_t repeat_at;
    /* Keyboards that say again what's held every so often (USB ones, told
     * to with SET_IDLE): a key repeats only while that keeps coming, so a
     * release that arrives late (the system busy) doesn't add a repeat. */
    bool held_reports;
    uint64_t held_seen;
};

/* Adds the device and its /dev/input/eventN. The first two are "all
 * keyboards" and "all pointers": every keyboard's and pointer's events come
 * out of them too, so a program (the desktop) can read them all, whatever
 * is plugged in later. */
void input_register(struct input_device *device);
/* The device was unplugged: no more events, and gone from the device tree.
 * Its /dev/input/eventN stays (programs may have it open), for
 * input_reuse to give to the next device of its kind. */
void input_unregister(struct input_device *device);
/* An unplugged device with these capabilities, to register again (or NULL). */
struct input_device *input_reuse(uint32_t capabilities);
struct input_device *input_all_keyboards(void);
struct input_device *input_all_pointers(void);

/* Absolute pointers (tablets, touch screens) report VX_ABS_X and VX_ABS_Y
 * from 0 to VX_ABS_MAX across the screen. */

/* Keyboards report keys here (Linux key codes; value 1 pressed, 0 released,
 * 2 repeated): as input events, and as text for the console (core/keyboard.c). */
void keyboard_key(struct input_device *device, uint16_t keycode, int value);
/* For keyboards that don't repeat held keys themselves: repeats from the kernel. */
void keyboard_soft_repeat(struct input_device *device);
/* The keyboard said again which keys are held (see held_reports). */
void keyboard_still_held(struct input_device *device);
/* How held keys repeat, on every keyboard. */
int keyboard_set_repeat(const struct vx_key_repeat *repeat);
/* Queues an event for every reader (drivers call it, also from interrupts).
 * After a group of events, report VX_EV_SYN (see input_sync). */
void input_report(struct input_device *device, uint16_t type, uint16_t code, int32_t value);
void input_sync(struct input_device *device);
/* True while a program has grabbed the device. */
bool input_grabbed(struct input_device *device);

/* Every registered device, for the Linux subsystem's evdev view. */
struct input_device *input_device_at(int index);
/* The device an open /dev/input/eventN is, or NULL for other files. */
struct file;
struct input_device *input_file_device(struct file *file);

#endif
