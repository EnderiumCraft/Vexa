/*
 * Linux's device interfaces for graphics and input, over Vexa's own:
 *
 * - evdev: /dev/input/eventN as Linux programs see it. Vexa's input devices
 *   already use Linux's event types and codes; here reads become Linux's
 *   struct input_event and the EVIOC* requests describe the device.
 *
 * - DRM: /dev/dri/card0, a KMS device with one CRTC, one encoder and one
 *   connector (the screen), and "dumb buffers": frame buffers in memory that
 *   programs map and draw into. The buffer being shown is copied to the real
 *   frame buffer when it's set (SETCRTC), flipped to (PAGE_FLIP) or marked
 *   dirty (DIRTYFB), and a few dozen times a second while it's on screen, for
 *   programs that just draw. Showing a buffer takes the display from the
 *   console (or fails with EBUSY while the desktop has it); closing the card
 *   gives it back. No atomic modesetting, planes, cursors or PRIME.
 */
#include <vexa/abi.h>
#include <vexa/arch.h>
#include <vexa/fb.h>
#include <vexa/fs.h>
#include <vexa/input.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/mutex.h>
#include <vexa/object.h>
#include <vexa/sched.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/vfs.h>
#include "linux.h"

/* Linux's ioctl request numbers: direction, size, type and number. */
#define IOC_NR(r) ((r)&0xff)
#define IOC_TYPE(r) (((r) >> 8) & 0xff)
#define IOC_SIZE(r) (((r) >> 16) & 0x3fff)

/* ======================================================================
 * evdev
 * ====================================================================== */

struct linux_input_event {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_REP 0x14
#define BUS_I8042 0x11

/* What a device can report, as Linux's bit arrays (EVIOCGBIT). */
static size_t device_bits(struct input_device *device, unsigned type, uint8_t *bits,
                          size_t size) {
    memset(bits, 0, size);
    bool keys = device->capabilities & VX_INPUT_KEYS;
    bool pointer = device->capabilities & VX_INPUT_POINTER;
#define SET(n)                                                                                     \
    do {                                                                                           \
        if ((n) / 8 < size) {                                                                      \
            bits[(n) / 8] |= (uint8_t)(1u << ((n) % 8));                                           \
        }                                                                                          \
    } while (0)
    switch (type) {
    case 0: /* The event types. */
        SET(EV_SYN);
        SET(EV_KEY);
        if (pointer) {
            SET(EV_REL);
        }
        if (keys) {
            SET(EV_REP);
        }
        break;
    case EV_KEY:
        for (unsigned code = 1; keys && code < 0x100; code++) {
            SET(code);
        }
        if (pointer) {
            SET(VX_BTN_LEFT);
            SET(VX_BTN_RIGHT);
            SET(VX_BTN_MIDDLE);
        }
        break;
    case EV_REL:
        if (pointer) {
            SET(VX_REL_X);
            SET(VX_REL_Y);
            SET(VX_REL_WHEEL);
        }
        break;
    }
#undef SET
    return size;
}

static int64_t evdev_ioctl(struct file *file, struct input_device *device, uint32_t request,
                           uint64_t arg) {
    if (IOC_TYPE(request) != 'E') {
        return -LE_ENOTTY;
    }
    unsigned nr = IOC_NR(request), size = IOC_SIZE(request);
    switch (nr) {
    case 0x01: { /* EVIOCGVERSION */
        int version = 0x010001;
        return copy_to_user(arg, &version, sizeof(version)) ? 0 : -LE_EFAULT;
    }
    case 0x02: { /* EVIOCGID */
        uint16_t id[4] = {BUS_I8042, 0x0001, (uint16_t)(device->index + 1), 0x0100};
        return copy_to_user(arg, id, sizeof(id)) ? 0 : -LE_EFAULT;
    }
    case 0x03: /* EVIOCGREP / EVIOCSREP */
        if (!(device->capabilities & VX_INPUT_KEYS)) {
            return -LE_EINVAL;
        }
        if (request >> 30 == 1) { /* Set: delay and period (ms). */
            unsigned rep[2];
            if (!copy_from_user(rep, arg, sizeof(rep))) {
                return -LE_EFAULT;
            }
            struct vx_key_repeat repeat = {rep[0], rep[1] ? 1000 / rep[1] : 30};
            return linux_errno(vfs_control(file, VX_INPUT_SET_REPEAT, &repeat, sizeof(repeat)));
        } else {
            unsigned rep[2] = {500, 33};
            return copy_to_user(arg, rep, sizeof(rep)) ? 0 : -LE_EFAULT;
        }
    case 0x06: { /* EVIOCGNAME(len) */
        size_t n = strlen(device->name) + 1;
        n = n < size ? n : size;
        return copy_to_user(arg, device->name, n) ? (int64_t)n : -LE_EFAULT;
    }
    case 0x07: /* EVIOCGPHYS */
    case 0x08: /* EVIOCGUNIQ */
        return -LE_ENOENT;
    case 0x09: /* EVIOCGPROP: none */
    case 0x18: /* EVIOCGKEY: which keys are down (we don't keep that) */
    case 0x19: /* EVIOCGLED */
    case 0x1a: /* EVIOCGSND */
    case 0x1b: { /* EVIOCGSW */
        uint8_t zeros[64] = {0};
        size_t n = size < sizeof(zeros) ? size : sizeof(zeros);
        return copy_to_user(arg, zeros, n) ? (int64_t)n : -LE_EFAULT;
    }
    case 0x90: { /* EVIOCGRAB */
        int on = arg != 0;
        return linux_errno(vfs_control(file, VX_INPUT_GRAB, &on, sizeof(on)));
    }
    case 0x91: /* EVIOCREVOKE */
    case 0x92: /* EVIOCGMASK */
    case 0x93: /* EVIOCSMASK */
    case 0xa0: /* EVIOCSCLOCKID: times are always since boot (CLOCK_MONOTONIC) */
        return 0;
    }
    if (nr >= 0x20 && nr < 0x40) { /* EVIOCGBIT(type, len) */
        uint8_t bits[96];
        size_t n = size < sizeof(bits) ? size : sizeof(bits);
        device_bits(device, nr - 0x20, bits, n);
        return copy_to_user(arg, bits, n) ? (int64_t)n : -LE_EFAULT;
    }
    if (nr >= 0x40 && nr < 0x80) { /* EVIOCGABS: no absolute axes */
        return -LE_EINVAL;
    }
    return -LE_EINVAL;
}

/* Reads Vexa's events and hands them over as Linux's. */
static int64_t evdev_read(struct file *file, uint64_t buffer, uint64_t size) {
    size_t max = size / sizeof(struct linux_input_event);
    if (max == 0) {
        return -LE_EINVAL;
    }
    if (max > 64) {
        max = 64;
    }
    struct vx_input_event events[64];
    int64_t n = vfs_read(file, events, max * sizeof(events[0]));
    if (n < 0) {
        return linux_errno(n);
    }
    size_t count = (size_t)n / sizeof(events[0]);
    for (size_t i = 0; i < count; i++) {
        struct linux_input_event out = {
            (int64_t)(events[i].time_ms / 1000), (int64_t)(events[i].time_ms % 1000) * 1000,
            events[i].type, events[i].code, events[i].value};
        if (!copy_to_user(buffer + i * sizeof(out), &out, sizeof(out))) {
            return -LE_EFAULT;
        }
    }
    return (int64_t)(count * sizeof(struct linux_input_event));
}

/* ======================================================================
 * DRM
 * ====================================================================== */

struct drm_version {
    int32_t major, minor, patchlevel;
    uint64_t name_len, name, date_len, date, desc_len, desc;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

struct drm_get_cap {
    uint64_t capability, value;
};

struct drm_gem_close {
    uint32_t handle, pad;
};

struct drm_event_vblank {
    uint32_t type, length;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, crtc_id;
};

struct drm_wait_vblank {
    uint32_t type, sequence;
    int64_t sec, usec; /* The request's `signal` overlays sec. */
};

#define DRM_EVENT_VBLANK 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02
#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_VBLANK_EVENT 0x4000000
#define FOURCC_XRGB8888 0x34325258 /* 'XR24' */
#define FOURCC_ARGB8888 0x34325241 /* 'AR24' */

#define CRTC_ID 31
#define CONNECTOR_ID 32
#define ENCODER_ID 33
#define MAX_BUFFERS 64
#define MAX_FBS 64
#define MAX_EVENTS 16
#define REFRESH_MS 33

struct drm_file {
    struct drm_event_vblank events[MAX_EVENTS];
    unsigned head, count;
    struct wait_queue readers;
};

struct buffer {
    uint32_t handle;
    uint32_t width, height, pitch;
    uint64_t size;
    uint64_t *pages; /* Physical pages, each with a reference held here. */
    size_t page_count;
    unsigned refs; /* Its handle, and each frame buffer made from it. */
    struct drm_file *owner;
};

struct framebuffer {
    uint32_t id, width, height, pitch, format;
    struct buffer *buffer;
    struct drm_file *owner;
};

static struct mutex drm_lock = MUTEX_INIT;
static struct buffer *buffers[MAX_BUFFERS];
static struct framebuffer *fbs[MAX_FBS];
static uint32_t next_handle = 1, next_fb_id = 100;
/* What's on screen: the frame buffer, where in it, and who set it. */
static struct framebuffer *scanout;
static uint32_t scanout_x, scanout_y;
static struct drm_file *display_owner;
static bool refresher_started;
static uint64_t last_copy_ms;

static struct buffer *find_buffer(uint32_t handle) {
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i] && buffers[i]->handle == handle) {
            return buffers[i];
        }
    }
    return NULL;
}

static struct framebuffer *find_fb(uint32_t id) {
    for (int i = 0; i < MAX_FBS; i++) {
        if (fbs[i] && fbs[i]->id == id) {
            return fbs[i];
        }
    }
    return NULL;
}

static void buffer_put(struct buffer *b) {
    if (--b->refs) {
        return;
    }
    for (size_t i = 0; i < b->page_count; i++) {
        page_ref_put(b->pages[i]); /* Mappings keep their own references. */
    }
    kfree(b->pages);
    kfree(b);
}

static void buffer_remove(struct buffer *b) {
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i] == b) {
            buffers[i] = NULL;
            buffer_put(b);
            return;
        }
    }
}

static void fb_remove(struct framebuffer *fb) {
    for (int i = 0; i < MAX_FBS; i++) {
        if (fbs[i] == fb) {
            fbs[i] = NULL;
        }
    }
    if (scanout == fb) {
        scanout = NULL;
    }
    buffer_put(fb->buffer);
    kfree(fb);
}

/* With drm_lock: copies the frame buffer on screen to the display. */
static void show(void) {
    if (!scanout || !display_owner) {
        return;
    }
    struct vx_display_info info;
    uint8_t *frame = display_frame(&info);
    if (!frame) {
        return;
    }
    struct buffer *b = scanout->buffer;
    uint32_t width = scanout->width - scanout_x < info.width ? scanout->width - scanout_x
                                                              : info.width;
    uint32_t height = scanout->height - scanout_y < info.height ? scanout->height - scanout_y
                                                                 : info.height;
    for (uint32_t y = 0; y < height; y++) {
        uint64_t from = (uint64_t)(y + scanout_y) * scanout->pitch + scanout_x * 4;
        uint8_t *to = frame + (uint64_t)y * info.pitch;
        uint64_t left = (uint64_t)width * 4;
        while (left) {
            uint64_t page = from / PAGE_SIZE, within = from % PAGE_SIZE;
            if (page >= b->page_count) {
                break;
            }
            uint64_t n = PAGE_SIZE - within < left ? PAGE_SIZE - within : left;
            memcpy(to, (uint8_t *)phys_to_virt(b->pages[page]) + within, n);
            to += n, from += n, left -= n;
        }
    }
    last_copy_ms = timer_ms();
}

/* Keeps the screen up to date for programs that draw without saying so. */
static void refresher(void *arg) {
    (void)arg;
    for (;;) {
        thread_sleep_ms(REFRESH_MS);
        mutex_lock(&drm_lock);
        if (scanout && timer_ms() - last_copy_ms >= REFRESH_MS) {
            show();
        }
        mutex_unlock(&drm_lock);
    }
}

static void queue_event(struct drm_file *df, uint32_t type, uint64_t user_data) {
    uint64_t now = timer_ms();
    struct drm_event_vblank e = {type, sizeof(e), user_data, (uint32_t)(now / 1000),
                                 (uint32_t)(now % 1000) * 1000, (uint32_t)(now * 60 / 1000),
                                 CRTC_ID};
    if (df->count < MAX_EVENTS) {
        df->events[(df->head + df->count++) % MAX_EVENTS] = e;
    }
    wait_queue_wake_all(&df->readers);
}

/* A mode for a size: plausible timings at 60 Hz. */
static void make_mode(struct drm_mode_modeinfo *m, unsigned width, unsigned height,
                      bool preferred) {
    memset(m, 0, sizeof(*m));
    m->hdisplay = (uint16_t)width;
    m->hsync_start = (uint16_t)(width + 48);
    m->hsync_end = (uint16_t)(width + 80);
    m->htotal = (uint16_t)(width + 160);
    m->vdisplay = (uint16_t)height;
    m->vsync_start = (uint16_t)(height + 3);
    m->vsync_end = (uint16_t)(height + 9);
    m->vtotal = (uint16_t)(height + 30);
    m->vrefresh = 60;
    m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * 60 / 1000);
    m->type = 0x40 | (preferred ? 0x08 : 0); /* DRIVER, PREFERRED */
    ksnprintf(m->name, sizeof(m->name), "%ux%u", width, height);
}

static void current_mode(struct drm_mode_modeinfo *m) {
    struct vx_display_info info;
    display_frame(&info);
    make_mode(m, info.width, info.height, true);
}

static int64_t copy_string(uint64_t user, uint64_t *length, const char *text) {
    size_t n = strlen(text);
    if (user && *length && !copy_to_user(user, text, n < *length ? n : *length)) {
        return -LE_EFAULT;
    }
    *length = n;
    return 0;
}

static int64_t add_fb(struct drm_file *df, uint32_t width, uint32_t height, uint32_t pitch,
                      uint32_t format, uint32_t handle, uint32_t *id) {
    struct buffer *b = find_buffer(handle);
    if (!b || !width || !height || pitch < width * 4 ||
        (uint64_t)pitch * height > b->size) {
        return -LE_EINVAL;
    }
    int slot = -1;
    for (int i = 0; i < MAX_FBS && slot < 0; i++) {
        if (!fbs[i]) {
            slot = i;
        }
    }
    struct framebuffer *fb = slot >= 0 ? kzalloc(sizeof(*fb)) : NULL;
    if (!fb) {
        return -LE_ENOMEM;
    }
    *fb = (struct framebuffer){next_fb_id++, width, height, pitch, format, b, df};
    b->refs++;
    fbs[slot] = fb;
    *id = fb->id;
    return 0;
}

static int64_t create_dumb(struct drm_file *df, struct drm_mode_create_dumb *c) {
    if (!c->width || !c->height || c->width > 8192 || c->height > 8192 ||
        (c->bpp != 32 && c->bpp != 24 && c->bpp != 16 && c->bpp != 8)) {
        return -LE_EINVAL;
    }
    uint32_t pitch = (c->width * ((c->bpp + 7) / 8) + 63) & ~63u;
    uint64_t size = ((uint64_t)pitch * c->height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    int slot = -1;
    for (int i = 0; i < MAX_BUFFERS && slot < 0; i++) {
        if (!buffers[i]) {
            slot = i;
        }
    }
    if (slot < 0 || size / PAGE_SIZE > 0x10000) {
        return -LE_ENOMEM;
    }
    struct buffer *b = kzalloc(sizeof(*b));
    size_t count = size / PAGE_SIZE;
    uint64_t *pages = b ? kzalloc(count * sizeof(uint64_t)) : NULL;
    if (!pages) {
        kfree(b);
        return -LE_ENOMEM;
    }
    for (size_t i = 0; i < count; i++) {
        pages[i] = page_ref_new(); /* Zeroed. */
        if (!pages[i]) {
            while (i--) {
                page_ref_put(pages[i]);
            }
            kfree(pages);
            kfree(b);
            return -LE_ENOMEM;
        }
    }
    *b = (struct buffer){next_handle++, c->width, c->height, pitch, size, pages, count, 1, df};
    buffers[slot] = b;
    c->handle = b->handle;
    c->pitch = pitch;
    c->size = size;
    return 0;
}

static int64_t set_crtc(struct drm_file *df, struct drm_mode_crtc *c) {
    if (c->crtc_id != CRTC_ID) {
        return -LE_EINVAL;
    }
    if (!c->fb_id || !c->mode_valid) { /* Off: the console comes back. */
        if (display_owner == df) {
            scanout = NULL;
            display_owner = NULL;
            display_release(df);
        }
        return 0;
    }
    struct framebuffer *fb = find_fb(c->fb_id);
    if (!fb || c->x >= fb->width || c->y >= fb->height) {
        return -LE_EINVAL;
    }
    if (display_owner != df) {
        int error = display_claim(df);
        if (error) {
            return linux_errno(error);
        }
        display_owner = df;
    }
    int error = display_set_mode(df, c->mode.hdisplay, c->mode.vdisplay);
    if (error && error != -VX_EINVAL) {
        return linux_errno(error);
    }
    scanout = fb;
    scanout_x = c->x;
    scanout_y = c->y;
    show();
    if (!refresher_started) {
        refresher_started = true;
        thread_create("drm", refresher, NULL);
    }
    return 0;
}

#define USER_IN(type, var)                                                                         \
    type var;                                                                                      \
    if (IOC_SIZE(request) < sizeof(var)) {                                                         \
        return -LE_EINVAL;                                                                         \
    }                                                                                              \
    if (!copy_from_user(&var, arg, sizeof(var))) {                                                 \
        return -LE_EFAULT;                                                                         \
    }
#define USER_OUT(var) (copy_to_user(arg, &var, sizeof(var)) ? 0 : -LE_EFAULT)

static int64_t drm_ioctl_locked(struct drm_file *df, uint32_t request, uint64_t arg) {
    switch (IOC_NR(request)) {
    case 0x00: { /* VERSION */
        USER_IN(struct drm_version, v);
        v.major = 1, v.minor = 0, v.patchlevel = 0;
        int64_t error = copy_string(v.name, &v.name_len, "vexa");
        error = error ? error : copy_string(v.date, &v.date_len, "20261003");
        error = error ? error : copy_string(v.desc, &v.desc_len, "Vexa's display");
        return error ? error : USER_OUT(v);
    }
    case 0x01: { /* GET_UNIQUE */
        uint64_t unique[2];
        if (!copy_from_user(unique, arg, sizeof(unique))) {
            return -LE_EFAULT;
        }
        unique[0] = 0;
        return copy_to_user(arg, unique, sizeof(unique)) ? 0 : -LE_EFAULT;
    }
    case 0x02: { /* GET_MAGIC */
        uint32_t magic = 1;
        return copy_to_user(arg, &magic, sizeof(magic)) ? 0 : -LE_EFAULT;
    }
    case 0x11: /* AUTH_MAGIC */
    case 0x1e: /* SET_MASTER */
    case 0x1f: /* DROP_MASTER */
        return 0;
    case 0x09: { /* GEM_CLOSE */
        USER_IN(struct drm_gem_close, c);
        struct buffer *b = find_buffer(c.handle);
        if (!b) {
            return -LE_EINVAL;
        }
        buffer_remove(b);
        return 0;
    }
    case 0x0c: { /* GET_CAP */
        USER_IN(struct drm_get_cap, cap);
        uint64_t value;
        switch (cap.capability) {
        case 0x1: value = 1; break;   /* DUMB_BUFFER */
        case 0x3: value = 24; break;  /* DUMB_PREFERRED_DEPTH */
        case 0x4: value = 1; break;   /* DUMB_PREFER_SHADOW */
        case 0x5: value = 0; break;   /* PRIME */
        case 0x6: value = 1; break;   /* TIMESTAMP_MONOTONIC */
        case 0x7: value = 0; break;   /* ASYNC_PAGE_FLIP */
        case 0x8: case 0x9: value = 64; break; /* CURSOR_WIDTH, CURSOR_HEIGHT */
        case 0x10: value = 0; break;  /* ADDFB2_MODIFIERS */
        case 0x12: value = 1; break;  /* CRTC_IN_VBLANK_EVENT */
        default: return -LE_EINVAL;
        }
        cap.value = value;
        return USER_OUT(cap);
    }
    case 0x0d: { /* SET_CLIENT_CAP: no universal planes, atomic or writeback */
        USER_IN(struct drm_get_cap, cap);
        return cap.capability == 1 || cap.capability == 4 ? 0 : -LE_EOPNOTSUPP;
    }
    case 0x3a: { /* WAIT_VBLANK: answers at once */
        USER_IN(struct drm_wait_vblank, w);
        uint64_t now = timer_ms();
        if (w.type & DRM_VBLANK_EVENT) {
            queue_event(df, DRM_EVENT_VBLANK, (uint64_t)w.sec);
        }
        w.sequence = (uint32_t)(now * 60 / 1000);
        w.sec = (int64_t)(now / 1000);
        w.usec = (int64_t)(now % 1000) * 1000;
        return USER_OUT(w);
    }
    case 0xa0: { /* MODE_GETRESOURCES */
        USER_IN(struct drm_mode_card_res, r);
        uint32_t ids[MAX_FBS], count = 0;
        for (int i = 0; i < MAX_FBS; i++) {
            if (fbs[i]) {
                ids[count++] = fbs[i]->id;
            }
        }
        uint32_t crtc = CRTC_ID, connector = CONNECTOR_ID, encoder = ENCODER_ID;
        if ((r.count_fbs && r.fb_id_ptr &&
             !copy_to_user(r.fb_id_ptr, ids,
                           (r.count_fbs < count ? r.count_fbs : count) * sizeof(uint32_t))) ||
            (r.count_crtcs && r.crtc_id_ptr && !copy_to_user(r.crtc_id_ptr, &crtc, 4)) ||
            (r.count_connectors && r.connector_id_ptr &&
             !copy_to_user(r.connector_id_ptr, &connector, 4)) ||
            (r.count_encoders && r.encoder_id_ptr &&
             !copy_to_user(r.encoder_id_ptr, &encoder, 4))) {
            return -LE_EFAULT;
        }
        r.count_fbs = count;
        r.count_crtcs = r.count_connectors = r.count_encoders = 1;
        r.min_width = 640, r.min_height = 480, r.max_width = 4096, r.max_height = 4096;
        return USER_OUT(r);
    }
    case 0xa1: { /* MODE_GETCRTC */
        USER_IN(struct drm_mode_crtc, c);
        if (c.crtc_id != CRTC_ID) {
            return -LE_EINVAL;
        }
        c.fb_id = scanout ? scanout->id : 0;
        c.x = scanout_x, c.y = scanout_y;
        c.gamma_size = 0;
        c.mode_valid = 1;
        current_mode(&c.mode);
        return USER_OUT(c);
    }
    case 0xa2: { /* MODE_SETCRTC */
        USER_IN(struct drm_mode_crtc, c);
        return set_crtc(df, &c);
    }
    case 0xa3: /* MODE_CURSOR */
    case 0xbb: /* MODE_CURSOR2: no hardware cursor (programs draw their own) */
        return -LE_ENXIO;
    case 0xa4: /* MODE_GETGAMMA */
    case 0xa5: /* MODE_SETGAMMA */
        return 0;
    case 0xa6: { /* MODE_GETENCODER */
        USER_IN(struct drm_mode_get_encoder, e);
        if (e.encoder_id != ENCODER_ID) {
            return -LE_EINVAL;
        }
        e.encoder_type = 5; /* VIRTUAL */
        e.crtc_id = CRTC_ID;
        e.possible_crtcs = 1;
        e.possible_clones = 0;
        return USER_OUT(e);
    }
    case 0xa7: { /* MODE_GETCONNECTOR */
        USER_IN(struct drm_mode_get_connector, c);
        if (c.connector_id != CONNECTOR_ID) {
            return -LE_EINVAL;
        }
        struct vx_display_modes *modes = kzalloc(sizeof(*modes));
        if (!modes) {
            return -LE_ENOMEM;
        }
        display_modes(modes);
        for (uint32_t i = 0; c.modes_ptr && i < c.count_modes && i < modes->count; i++) {
            struct drm_mode_modeinfo m;
            make_mode(&m, modes->modes[i].width, modes->modes[i].height, i == modes->current);
            if (!copy_to_user(c.modes_ptr + i * sizeof(m), &m, sizeof(m))) {
                kfree(modes);
                return -LE_EFAULT;
            }
        }
        uint32_t encoder = ENCODER_ID;
        if (c.count_encoders && c.encoders_ptr && !copy_to_user(c.encoders_ptr, &encoder, 4)) {
            kfree(modes);
            return -LE_EFAULT;
        }
        struct vx_display_info info;
        display_frame(&info);
        c.count_modes = modes->count;
        kfree(modes);
        c.count_props = 0;
        c.count_encoders = 1;
        c.encoder_id = ENCODER_ID;
        c.connector_type = 15; /* VIRTUAL */
        c.connector_type_id = 1;
        c.connection = 1; /* Connected. */
        c.mm_width = info.width * 254 / 960; /* 96 dots per inch. */
        c.mm_height = info.height * 254 / 960;
        c.subpixel = 1; /* Unknown. */
        return USER_OUT(c);
    }
    case 0xaa: /* MODE_GETPROPERTY */
    case 0xab: /* MODE_SETPROPERTY */
    case 0xac: /* MODE_GETPROPBLOB */
        return -LE_EINVAL;
    case 0xad: { /* MODE_GETFB */
        USER_IN(struct drm_mode_fb_cmd, c);
        struct framebuffer *fb = find_fb(c.fb_id);
        if (!fb) {
            return -LE_EINVAL;
        }
        c.width = fb->width, c.height = fb->height, c.pitch = fb->pitch;
        c.bpp = 32, c.depth = fb->format == FOURCC_ARGB8888 ? 32 : 24;
        c.handle = fb->owner == df ? fb->buffer->handle : 0;
        return USER_OUT(c);
    }
    case 0xae: { /* MODE_ADDFB */
        USER_IN(struct drm_mode_fb_cmd, c);
        if (c.bpp != 32 || (c.depth != 24 && c.depth != 32)) {
            return -LE_EINVAL;
        }
        int64_t error = add_fb(df, c.width, c.height, c.pitch,
                               c.depth == 32 ? FOURCC_ARGB8888 : FOURCC_XRGB8888, c.handle,
                               &c.fb_id);
        return error ? error : USER_OUT(c);
    }
    case 0xb8: { /* MODE_ADDFB2 */
        USER_IN(struct drm_mode_fb_cmd2, c);
        if ((c.pixel_format != FOURCC_XRGB8888 && c.pixel_format != FOURCC_ARGB8888) ||
            c.offsets[0]) {
            return -LE_EINVAL;
        }
        int64_t error = add_fb(df, c.width, c.height, c.pitches[0], c.pixel_format,
                               c.handles[0], &c.fb_id);
        return error ? error : USER_OUT(c);
    }
    case 0xaf:   /* MODE_RMFB */
    case 0xd0: { /* MODE_CLOSEFB */
        uint32_t id;
        if (!copy_from_user(&id, arg, sizeof(id))) {
            return -LE_EFAULT;
        }
        struct framebuffer *fb = find_fb(id);
        if (!fb) {
            return -LE_EINVAL;
        }
        fb_remove(fb);
        return 0;
    }
    case 0xb0: { /* MODE_PAGE_FLIP */
        USER_IN(struct drm_mode_crtc_page_flip, p);
        struct framebuffer *fb = find_fb(p.fb_id);
        if (p.crtc_id != CRTC_ID || !fb) {
            return -LE_EINVAL;
        }
        if (display_owner != df || !scanout) {
            return -LE_EBUSY;
        }
        scanout = fb;
        show();
        if (p.flags & DRM_MODE_PAGE_FLIP_EVENT) {
            queue_event(df, DRM_EVENT_FLIP_COMPLETE, p.user_data);
        }
        return 0;
    }
    case 0xb1: { /* MODE_DIRTYFB */
        uint32_t id;
        if (!copy_from_user(&id, arg, sizeof(id))) {
            return -LE_EFAULT;
        }
        struct framebuffer *fb = find_fb(id);
        if (!fb) {
            return -LE_EINVAL;
        }
        if (fb == scanout) {
            show();
        }
        return 0;
    }
    case 0xb2: { /* MODE_CREATE_DUMB */
        USER_IN(struct drm_mode_create_dumb, c);
        int64_t error = create_dumb(df, &c);
        return error ? error : USER_OUT(c);
    }
    case 0xb3: { /* MODE_MAP_DUMB: the offset to mmap the card at */
        USER_IN(struct drm_mode_map_dumb, m);
        if (!find_buffer(m.handle)) {
            return -LE_EINVAL;
        }
        m.offset = (uint64_t)m.handle << 28;
        return USER_OUT(m);
    }
    case 0xb4: { /* MODE_DESTROY_DUMB */
        uint32_t handle;
        if (!copy_from_user(&handle, arg, sizeof(handle))) {
            return -LE_EFAULT;
        }
        struct buffer *b = find_buffer(handle);
        if (!b) {
            return -LE_EINVAL;
        }
        buffer_remove(b);
        return 0;
    }
    case 0xb5: { /* MODE_GETPLANERESOURCES: none */
        uint64_t r[2];
        if (!copy_from_user(r, arg, sizeof(r))) {
            return -LE_EFAULT;
        }
        r[1] = 0;
        return copy_to_user(arg, r, sizeof(r)) ? 0 : -LE_EFAULT;
    }
    case 0xb9: { /* MODE_OBJ_GETPROPERTIES: none */
        uint64_t p[3];
        if (!copy_from_user(p, arg, sizeof(p))) {
            return -LE_EFAULT;
        }
        p[2] &= ~0xffffffffull; /* count_props = 0 */
        return copy_to_user(arg, p, sizeof(p)) ? 0 : -LE_EFAULT;
    }
    default:
        return -LE_EINVAL;
    }
}

static int64_t drm_ioctl(struct file *file, uint32_t request, uint64_t arg) {
    if (IOC_TYPE(request) != 'd') {
        return -LE_ENOTTY;
    }
    mutex_lock(&drm_lock);
    int64_t result = drm_ioctl_locked(file->private, request, arg);
    mutex_unlock(&drm_lock);
    return result;
}

static int drm_open(struct file *file) {
    struct drm_file *df = kzalloc(sizeof(*df));
    if (!df) {
        return -VX_ENOMEM;
    }
    file->private = df;
    return 0;
}

static void drm_close(struct file *file) {
    struct drm_file *df = file->private;
    mutex_lock(&drm_lock);
    for (int i = 0; i < MAX_FBS; i++) {
        if (fbs[i] && fbs[i]->owner == df) {
            fb_remove(fbs[i]);
        }
    }
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i] && buffers[i]->owner == df) {
            buffer_remove(buffers[i]);
        }
    }
    if (display_owner == df) {
        scanout = NULL;
        display_owner = NULL;
        display_release(df);
    }
    mutex_unlock(&drm_lock);
    kfree(df);
}

static bool has_drm_events(void *arg) {
    return ((struct drm_file *)arg)->count != 0;
}

static int64_t drm_read(struct file *file, void *out, size_t size) {
    struct drm_file *df = file->private;
    if (!has_drm_events(df)) {
        if (file->object.flags & OBJECT_NONBLOCK) {
            return -VX_EAGAIN;
        }
        int error = wait_queue_wait_interruptible(&df->readers, has_drm_events, df);
        if (error) {
            return error;
        }
    }
    mutex_lock(&drm_lock);
    size_t done = 0;
    while (df->count && size - done >= sizeof(struct drm_event_vblank)) {
        memcpy((uint8_t *)out + done, &df->events[df->head], sizeof(struct drm_event_vblank));
        df->head = (df->head + 1) % MAX_EVENTS;
        df->count--;
        done += sizeof(struct drm_event_vblank);
    }
    mutex_unlock(&drm_lock);
    return done ? (int64_t)done : -VX_EINVAL;
}

static uint32_t drm_poll(struct file *file) {
    return (has_drm_events(file->private) ? OBJECT_READABLE : 0) | OBJECT_WRITABLE;
}

/* mmap at MAP_DUMB's offset: the handle, then the page within the buffer. */
static uint64_t drm_share_page(struct vnode *vnode, uint64_t index) {
    (void)vnode;
    mutex_lock(&drm_lock);
    struct buffer *b = find_buffer((uint32_t)(index >> 16));
    uint64_t page = index & 0xffff, phys = 0;
    if (b && page < b->page_count) {
        phys = b->pages[page];
        page_ref_get(phys);
    }
    mutex_unlock(&drm_lock);
    return phys;
}

static const struct vnode_ops drm_ops = {
    .open = drm_open,
    .close = drm_close,
    .file_read = drm_read,
    .file_poll = drm_poll,
    .share_page = drm_share_page,
};

/* ======================================================================
 * For the rest of the Linux subsystem
 * ====================================================================== */

void linux_devices_init(void) {
    if (display_frame(NULL)) {
        devfs_add("dri/card0", &drm_ops, NULL);
    }
}

bool linux_device_ioctl(struct file *file, uint32_t request, uint64_t arg, int64_t *result) {
    struct input_device *device = input_file_device(file);
    if (device) {
        *result = evdev_ioctl(file, device, request, arg);
        return true;
    }
    if (file->vnode->ops == &drm_ops) {
        *result = drm_ioctl(file, request, arg);
        return true;
    }
    return false;
}

bool linux_device_read(struct file *file, uint64_t buffer, uint64_t size, int64_t *result) {
    if (input_file_device(file)) {
        *result = evdev_read(file, buffer, size);
        return true;
    }
    return false;
}
