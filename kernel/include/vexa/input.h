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

struct input_device {
    char name[64];
    uint32_t capabilities; /* VX_INPUT_* */
    int index;             /* N in /dev/input/eventN */
    struct spinlock lock;
    struct input_client *clients;
    struct input_client *grab; /* Only this client gets events, if set. */
};

/* Adds the device and its /dev/input/eventN. */
void input_register(struct input_device *device);
/* Queues an event for every reader (drivers call it, also from interrupts).
 * After a group of events, report VX_EV_SYN (see input_sync). */
void input_report(struct input_device *device, uint16_t type, uint16_t code, int32_t value);
void input_sync(struct input_device *device);
/* True while a program has grabbed the device. */
bool input_grabbed(struct input_device *device);

/* Every registered device, for the Linux subsystem's evdev view. */
struct input_device *input_device_at(int index);

#endif
