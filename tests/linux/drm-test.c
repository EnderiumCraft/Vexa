/* Linux graphics and input interfaces on Vexa: DRM (KMS with dumb buffers)
 * on /dev/dri/card0, and evdev on /dev/input/eventN, through the kernel's own
 * headers' structures.
 *
 * Shows a picture with SETCRTC and checks that it reached the screen (Vexa's
 * /dev/display0 maps the real frame buffer), flips to a second one and waits
 * for the flip's event, then describes the input devices and waits for a key
 * on the grabbed keyboard. Prints "drm-test: passed" at the end. */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("drm-test: FAILED: %s (errno %d)\n", what, errno);
        failures++;
    }
}

struct dumb {
    uint32_t handle, pitch, fb;
    uint64_t size;
    uint32_t *pixels;
};

static int make_dumb(int card, uint32_t width, uint32_t height, struct dumb *d) {
    struct drm_mode_create_dumb create = {.width = width, .height = height, .bpp = 32};
    if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &create)) {
        return -1;
    }
    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (ioctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        return -1;
    }
    void *p = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, card, map.offset);
    if (p == MAP_FAILED) {
        return -1;
    }
    struct drm_mode_fb_cmd fb = {.width = width, .height = height, .pitch = create.pitch,
                                 .bpp = 32, .depth = 24, .handle = create.handle};
    if (ioctl(card, DRM_IOCTL_MODE_ADDFB, &fb)) {
        return -1;
    }
    *d = (struct dumb){create.handle, create.pitch, fb.fb_id, create.size, p};
    return 0;
}

static void paint(struct dumb *d, uint32_t width, uint32_t height, uint32_t tint) {
    for (uint32_t y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)d->pixels + (size_t)y * d->pitch);
        for (uint32_t x = 0; x < width; x++) {
            row[x] = tint | (x * 255 / width) << 8 | (y * 255 / height);
        }
    }
}

/* The pixel at (x, y) of the real screen: Vexa's /dev/display0 maps the
 * frame buffer (here, its rows down to y). */
static uint32_t screen_pixel(int display, uint32_t pitch, uint32_t x, uint32_t y) {
    static uint32_t *frame;
    if (!frame) {
        size_t size = ((size_t)pitch * (y + 1) + 4095) & ~(size_t)4095;
        frame = mmap(NULL, size, PROT_READ, MAP_SHARED, display, 0);
        if (frame == MAP_FAILED) {
            frame = NULL;
            return 0;
        }
    }
    return frame[(size_t)y * pitch / 4 + x];
}

static void test_drm(void) {
    int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    check(card >= 0, "open /dev/dri/card0");
    if (card < 0) {
        return;
    }
    char name[32] = {0};
    struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
    check(ioctl(card, DRM_IOCTL_VERSION, &version) == 0, "DRM_IOCTL_VERSION");
    printf("drm-test: driver %s %d.%d\n", name, version.version_major, version.version_minor);

    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    check(ioctl(card, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1, "dumb buffers");
    check(ioctl(card, DRM_IOCTL_SET_MASTER, 0) == 0, "SET_MASTER");

    uint32_t crtc_id = 0, connector_id = 0, encoder_id = 0;
    struct drm_mode_card_res res = {0};
    check(ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES (counts)");
    check(res.count_crtcs == 1 && res.count_connectors == 1, "one CRTC and one connector");
    res.crtc_id_ptr = (uintptr_t)&crtc_id;
    res.connector_id_ptr = (uintptr_t)&connector_id;
    res.encoder_id_ptr = (uintptr_t)&encoder_id;
    res.count_fbs = 0;
    check(ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0, "GETRESOURCES (ids)");

    struct drm_mode_get_connector conn = {.connector_id = connector_id};
    check(ioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0, "GETCONNECTOR (counts)");
    check(conn.connection == 1 && conn.count_modes > 0, "a connected screen with modes");
    struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(*modes));
    uint32_t encoder = 0;
    conn.modes_ptr = (uintptr_t)modes;
    conn.encoders_ptr = (uintptr_t)&encoder;
    conn.count_props = 0;
    check(ioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0, "GETCONNECTOR (modes)");
    struct drm_mode_modeinfo mode = modes[0];
    for (uint32_t i = 0; i < conn.count_modes; i++) {
        if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = modes[i];
        }
    }
    printf("drm-test: %u modes, using %s\n", conn.count_modes, mode.name);
    uint32_t width = mode.hdisplay, height = mode.vdisplay;

    struct dumb a, b;
    check(make_dumb(card, width, height, &a) == 0, "dumb buffer A");
    check(make_dumb(card, width, height, &b) == 0, "dumb buffer B");
    if (failures) {
        return;
    }
    paint(&a, width, height, 0x800000);
    paint(&b, width, height, 0x000000);

    struct drm_mode_crtc crtc = {.crtc_id = crtc_id, .fb_id = a.fb, .mode_valid = 1,
                                 .mode = mode, .count_connectors = 1,
                                 .set_connectors_ptr = (uintptr_t)&connector_id};
    check(ioctl(card, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0, "SETCRTC");
    printf("drm-test: showing %ux%u\n", width, height);

    /* What's on the screen now: the picture's middle pixel. */
    int display = open("/dev/display0", O_RDONLY);
    uint32_t pitch = width * 4;
    uint32_t want = a.pixels[(height / 2) * (a.pitch / 4) + width / 2];
    uint32_t got = display >= 0 ? screen_pixel(display, pitch, width / 2, height / 2) : 0;
    check((got & 0xffffff) == (want & 0xffffff), "the picture on the screen");
    printf("drm-test: screen pixel %06x (drew %06x)\n", got & 0xffffff, want & 0xffffff);

    /* Flip to B, with an event to say when. */
    struct drm_mode_crtc_page_flip flip = {.crtc_id = crtc_id, .fb_id = b.fb,
                                           .flags = DRM_MODE_PAGE_FLIP_EVENT,
                                           .user_data = 0x1234};
    check(ioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0, "PAGE_FLIP");
    struct pollfd pfd = {card, POLLIN, 0};
    check(poll(&pfd, 1, 2000) == 1, "the flip's event arrives");
    struct drm_event_vblank event = {0};
    check(read(card, &event, sizeof(event)) == sizeof(event) &&
              event.base.type == DRM_EVENT_FLIP_COMPLETE && event.user_data == 0x1234,
          "a flip-complete event");
    want = b.pixels[(height / 2) * (b.pitch / 4) + width / 2];
    got = display >= 0 ? screen_pixel(display, pitch, width / 2, height / 2) : 0;
    check((got & 0xffffff) == (want & 0xffffff), "the flipped picture on the screen");
    printf("drm-test: flipped, screen pixel %06x\n", got & 0xffffff);

    /* Drawing without saying so shows up too (the screen is refreshed). */
    paint(&b, width, height, 0x400000);
    usleep(200 * 1000);
    want = b.pixels[(height / 2) * (b.pitch / 4) + width / 2];
    got = display >= 0 ? screen_pixel(display, pitch, width / 2, height / 2) : 0;
    check((got & 0xffffff) == (want & 0xffffff), "drawing shows up by itself");

    struct drm_mode_crtc current = {.crtc_id = crtc_id};
    check(ioctl(card, DRM_IOCTL_MODE_GETCRTC, &current) == 0 && current.fb_id == b.fb,
          "GETCRTC");
    check(ioctl(card, DRM_IOCTL_MODE_RMFB, &a.fb) == 0, "RMFB");
    struct drm_mode_destroy_dumb destroy = {.handle = a.handle};
    check(ioctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) == 0, "DESTROY_DUMB");
    close(card); /* The console comes back. */
    if (display >= 0) {
        close(display);
    }
    free(modes);
}

static int has_bit(const uint8_t *bits, unsigned n) {
    return bits[n / 8] & (1 << (n % 8));
}

static void test_evdev(void) {
    int keyboard = -1, mouse = -1;
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char name[64] = "";
        uint8_t types[4] = {0};
        struct input_id id;
        check(ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0, "EVIOCGNAME");
        check(ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) > 0, "EVIOCGBIT");
        check(ioctl(fd, EVIOCGID, &id) == 0, "EVIOCGID");
        printf("drm-test: %s: \"%s\"%s%s\n", path, name, has_bit(types, EV_KEY) ? " keys" : "",
               has_bit(types, EV_REL) ? " motion" : "");
        if (has_bit(types, EV_REL) && mouse < 0) {
            mouse = fd;
        } else if (has_bit(types, EV_REP) && keyboard < 0) {
            keyboard = fd;
        } else {
            close(fd);
        }
    }
    check(keyboard >= 0 && mouse >= 0, "a keyboard and a mouse");
    if (keyboard < 0) {
        return;
    }
    uint8_t keys[KEY_MAX / 8 + 1] = {0};
    ioctl(keyboard, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    check(has_bit(keys, KEY_A) && has_bit(keys, KEY_ENTER), "the keyboard has A and Enter");
    check(ioctl(keyboard, EVIOCGRAB, 1) == 0, "EVIOCGRAB");
    printf("drm-test: press a key\n");
    fflush(stdout);
    struct pollfd pfd = {keyboard, POLLIN, 0};
    int pressed = 0;
    while (!pressed && poll(&pfd, 1, 20000) == 1) {
        struct input_event events[16];
        ssize_t n = read(keyboard, events, sizeof(events));
        check(n > 0 && n % sizeof(struct input_event) == 0, "whole input_events");
        for (ssize_t i = 0; i < n / (ssize_t)sizeof(events[0]); i++) {
            if (events[i].type == EV_KEY && events[i].value == 1) {
                printf("drm-test: key %u down\n", events[i].code);
                pressed = events[i].code;
            }
        }
    }
    check(pressed == KEY_A, "A pressed");
    usleep(300 * 1000); /* Its release goes to us too, not the shell. */
    ioctl(keyboard, EVIOCGRAB, 0);
    close(keyboard);
    close(mouse);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_drm();
    test_evdev();
    if (failures) {
        printf("drm-test: %d failures\n", failures);
        return 1;
    }
    printf("drm-test: passed\n");
    return 0;
}
