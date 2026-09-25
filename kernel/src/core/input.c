#include <vexa/arch.h>
#include <vexa/fs.h>
#include <vexa/input.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/*
 * Input devices and their readers. Each open /dev/input/eventN is a client
 * with its own ring of events; drivers queue into every client (or only the
 * one that grabbed the device) from their interrupt handlers. A client that
 * doesn't keep up loses its oldest events.
 */

#define MAX_DEVICES 16
#define CLIENT_EVENTS 512

struct input_client {
    struct input_client *next;
    struct input_device *device;
    struct vx_input_event events[CLIENT_EVENTS];
    uint32_t head, tail; /* head == tail: empty */
    struct wait_queue readers;
};

static struct input_device *devices[MAX_DEVICES];
static int device_count;

struct input_device *input_device_at(int index) {
    return index >= 0 && index < device_count ? devices[index] : NULL;
}

static void push(struct input_client *client, const struct vx_input_event *event) {
    uint32_t next = (client->head + 1) % CLIENT_EVENTS;
    if (next == client->tail) {
        client->tail = (client->tail + 1) % CLIENT_EVENTS; /* Full: drop the oldest. */
    }
    client->events[client->head] = *event;
    client->head = next;
}

void input_report(struct input_device *device, uint16_t type, uint16_t code, int32_t value) {
    struct vx_input_event event = {timer_ms(), type, code, value};
    uint64_t flags = spin_lock_irqsave(&device->lock);
    struct input_client *woken[8];
    int count = 0;
    for (struct input_client *c = device->clients; c; c = c->next) {
        if (device->grab && device->grab != c) {
            continue;
        }
        push(c, &event);
        if (type == VX_EV_SYN && count < 8) {
            woken[count++] = c; /* Readers wake once the group is complete. */
        }
    }
    spin_unlock_irqrestore(&device->lock, flags);
    for (int i = 0; i < count; i++) {
        wait_queue_wake_all(&woken[i]->readers);
    }
}

void input_sync(struct input_device *device) {
    input_report(device, VX_EV_SYN, 0, 0);
}

bool input_grabbed(struct input_device *device) {
    return __atomic_load_n(&device->grab, __ATOMIC_ACQUIRE) != NULL;
}

/* ---- /dev/input/eventN ---- */

static int event_open(struct file *file) {
    struct input_device *device = devfs_data(file->vnode);
    struct input_client *client = kzalloc(sizeof(*client));
    if (!client) {
        return -VX_ENOMEM;
    }
    client->device = device;
    uint64_t flags = spin_lock_irqsave(&device->lock);
    client->next = device->clients;
    device->clients = client;
    spin_unlock_irqrestore(&device->lock, flags);
    file->private = client;
    return 0;
}

static void event_close(struct file *file) {
    struct input_client *client = file->private;
    struct input_device *device = client->device;
    uint64_t flags = spin_lock_irqsave(&device->lock);
    struct input_client **p = &device->clients;
    while (*p && *p != client) {
        p = &(*p)->next;
    }
    if (*p) {
        *p = client->next;
    }
    if (device->grab == client) {
        device->grab = NULL;
    }
    spin_unlock_irqrestore(&device->lock, flags);
    kfree(client);
}

static bool has_events(void *arg) {
    struct input_client *client = arg;
    return client->head != client->tail;
}

static int64_t event_read(struct file *file, void *buffer, size_t size) {
    struct input_client *client = file->private;
    size_t max = size / sizeof(struct vx_input_event);
    if (max == 0) {
        return -VX_EINVAL;
    }
    if (!has_events(client)) {
        if (file->object.flags & OBJECT_NONBLOCK) {
            return -VX_EAGAIN;
        }
        int error = wait_queue_wait_interruptible(&client->readers, has_events, client);
        if (error) {
            return error;
        }
    }
    struct input_device *device = client->device;
    uint64_t flags = spin_lock_irqsave(&device->lock);
    size_t n = 0;
    struct vx_input_event *out = buffer;
    while (n < max && client->tail != client->head) {
        out[n++] = client->events[client->tail];
        client->tail = (client->tail + 1) % CLIENT_EVENTS;
    }
    spin_unlock_irqrestore(&device->lock, flags);
    return (int64_t)(n * sizeof(struct vx_input_event));
}

static uint32_t event_poll(struct file *file) {
    return has_events(file->private) ? OBJECT_READABLE : 0;
}

static int event_control(struct file *file, uint32_t request, void *arg, size_t size) {
    struct input_client *client = file->private;
    struct input_device *device = client->device;
    switch (request) {
    case VX_INPUT_INFO: {
        if (size < sizeof(struct vx_input_info)) {
            return -VX_EINVAL;
        }
        struct vx_input_info *info = arg;
        memset(info, 0, sizeof(*info));
        memcpy(info->name, device->name, sizeof(info->name));
        info->capabilities = device->capabilities;
        return 0;
    }
    case VX_INPUT_GRAB: {
        if (size < sizeof(int)) {
            return -VX_EINVAL;
        }
        int on = *(int *)arg;
        int error = 0;
        uint64_t flags = spin_lock_irqsave(&device->lock);
        if (on && device->grab && device->grab != client) {
            error = -VX_EBUSY;
        } else if (on) {
            device->grab = client;
        } else if (device->grab == client) {
            device->grab = NULL;
        }
        spin_unlock_irqrestore(&device->lock, flags);
        return error;
    }
    default:
        return -VX_ENOTTY;
    }
}

static const struct vnode_ops event_ops = {
    .open = event_open,
    .close = event_close,
    .file_read = event_read,
    .file_poll = event_poll,
    .control = event_control,
};

void input_register(struct input_device *device) {
    if (device_count == MAX_DEVICES) {
        return;
    }
    device->index = device_count;
    devices[device_count++] = device;
    char path[32];
    ksnprintf(path, sizeof(path), "input/event%d", device->index);
    devfs_add(path, &event_ops, device);
}
