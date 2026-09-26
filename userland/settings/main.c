/* settings: the desktop's settings window: the wallpaper (a gradient or an
 * image), the clock (24 or 12 hours) and the time zone (an offset from
 * UTC). Apply writes /etc/desktop.conf and tells the desktop to reload it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/desktop.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 460
#define HEIGHT 330
#define LEFT 20
#define SWATCH_W 76
#define SWATCH_H 44

static struct vx_window *window;
static char wallpaper[32] = "image";
static char image[256] = DESKTOP_DEFAULT_WALLPAPER;
static int clock_hours = 24;
static int utc_offset; /* Minutes. */
static bool typing;    /* The image field has the keyboard. */
static int hot = -1;   /* The control under the pointer. */
static char message[128];

enum { SWATCHES = 0, IMAGE_FIELD = 10, CLOCK_24, CLOCK_12, ZONE_MINUS, ZONE_PLUS, APPLY };

static void read_config(void) {
    int handle = vx_open(DESKTOP_CONFIG, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    char text[2048];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *value = strchr(line, '=');
        if (!value) {
            continue;
        }
        *value++ = '\0';
        if (!strcmp(line, "wallpaper")) {
            strncpy(wallpaper, value, sizeof(wallpaper) - 1);
        } else if (!strcmp(line, "wallpaper_image")) {
            strncpy(image, value, sizeof(image) - 1);
        } else if (!strcmp(line, "clock")) {
            clock_hours = atoi(value) == 12 ? 12 : 24;
        } else if (!strcmp(line, "utc_offset")) {
            utc_offset = atoi(value);
        }
    }
}

static void apply(void) {
    char text[512];
    int n = snprintf(text, sizeof(text), "wallpaper=%s\nwallpaper_image=%s\nclock=%d\nutc_offset=%d\n",
                     image[0] && !strcmp(wallpaper, "image") ? "image" : wallpaper, image,
                     clock_hours, utc_offset);
    int handle = vx_open(DESKTOP_CONFIG, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0 || vx_write(handle, text, (size_t)n) != n) {
        snprintf(message, sizeof(message), "Can't write %s", DESKTOP_CONFIG);
    } else {
        snprintf(message, sizeof(message), "Saved.");
        vx_desktop_reload();
    }
    if (handle >= 0) {
        vx_close(handle);
    }
}

/* Where each control is: fills in its rectangle; false if there's none. */
static bool control(int id, int *x, int *y, int *w, int *h) {
    if (id >= SWATCHES && id < SWATCHES + DESKTOP_WALLPAPER_COUNT) {
        *x = LEFT + (id - SWATCHES) * (SWATCH_W + 8);
        *y = 44, *w = SWATCH_W, *h = SWATCH_H;
        return true;
    }
    switch (id) {
    case IMAGE_FIELD: *x = LEFT + 7 * FONT_WIDTH, *y = 118, *w = WIDTH - 2 * LEFT - 7 * FONT_WIDTH, *h = FONT_HEIGHT + 8; return true;
    case CLOCK_24: *x = LEFT, *y = 180, *w = 96, *h = 26; return true;
    case CLOCK_12: *x = LEFT + 104, *y = 180, *w = 96, *h = 26; return true;
    case ZONE_MINUS: *x = LEFT, *y = 246, *w = 30, *h = 26; return true;
    case ZONE_PLUS: *x = LEFT + 130, *y = 246, *w = 30, *h = 26; return true;
    case APPLY: *x = WIDTH - LEFT - 90, *y = HEIGHT - 42, *w = 90, *h = 28; return true;
    }
    return false;
}

static int control_at(int px, int py) {
    for (int id = 0; id <= APPLY; id++) {
        int x, y, w, h;
        if (control(id, &x, &y, &w, &h) && vx_inside(px, py, x, y, w, h)) {
            return id;
        }
    }
    return -1;
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int x, y, w, h;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
    vx_draw_text(s, LEFT, 20, "Wallpaper", VX_COLOR_ACCENT, VX_TRANSPARENT);
    for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
        const struct desktop_wallpaper *wp = &desktop_wallpapers[i];
        control(SWATCHES + i, &x, &y, &w, &h);
        for (int row = 0; row < h; row++) {
            uint32_t color = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                int a = (wp->top >> shift) & 0xff, b = (wp->bottom >> shift) & 0xff;
                color |= (uint32_t)(a + (b - a) * row / h) << shift;
            }
            vx_fill(s, x, y + row, w, 1, color);
        }
        bool chosen = !strcmp(wallpaper, wp->name);
        vx_draw_outline(s, x, y, w, h, chosen ? VX_COLOR_ACCENT : VX_COLOR_LINE);
        if (chosen) {
            vx_draw_outline(s, x + 1, y + 1, w - 2, h - 2, VX_COLOR_ACCENT);
        }
        vx_draw_text(s, x, y + h + 4, wp->label, chosen ? VX_COLOR_TEXT : VX_COLOR_DIM,
                     VX_TRANSPARENT);
    }
    control(IMAGE_FIELD, &x, &y, &w, &h);
    vx_draw_text(s, LEFT, y + 4, "Image", VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_field(s, x, y, w, image, typing);
    vx_draw_text(s, LEFT, y + h + 4, "A PNG, BMP or PPM file; clear it for a gradient.",
                 VX_COLOR_DIM, VX_TRANSPARENT);

    vx_draw_text(s, LEFT, 160, "Clock", VX_COLOR_ACCENT, VX_TRANSPARENT);
    control(CLOCK_24, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, clock_hours == 24 ? "[24-hour]" : "24-hour", hot == CLOCK_24);
    control(CLOCK_12, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, clock_hours == 12 ? "[12-hour]" : "12-hour", hot == CLOCK_12);

    vx_draw_text(s, LEFT, 226, "Time zone", VX_COLOR_ACCENT, VX_TRANSPARENT);
    control(ZONE_MINUS, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, "-", hot == ZONE_MINUS);
    char zone[32];
    int offset = utc_offset < 0 ? -utc_offset : utc_offset;
    snprintf(zone, sizeof(zone), "UTC%c%d:%02d", utc_offset < 0 ? '-' : '+', offset / 60, offset % 60);
    vx_draw_text(s, LEFT + 40, y + 5, zone, VX_COLOR_TEXT, VX_TRANSPARENT);
    control(ZONE_PLUS, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, "+", hot == ZONE_PLUS);

    control(APPLY, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, "Apply", hot == APPLY);
    vx_draw_text(s, LEFT, y + 6, message, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_window_present(window, 0, 0, s->width, s->height);
}

static void click(int id) {
    typing = id == IMAGE_FIELD;
    if (id >= SWATCHES && id < SWATCHES + DESKTOP_WALLPAPER_COUNT) {
        strcpy(wallpaper, desktop_wallpapers[id - SWATCHES].name);
        image[0] = '\0';
    } else if (id == CLOCK_24) {
        clock_hours = 24;
    } else if (id == CLOCK_12) {
        clock_hours = 12;
    } else if (id == ZONE_MINUS && utc_offset > -12 * 60) {
        utc_offset -= 30;
    } else if (id == ZONE_PLUS && utc_offset < 14 * 60) {
        utc_offset += 30;
    } else if (id == APPLY) {
        if (image[0]) {
            strcpy(wallpaper, "image");
        }
        apply();
    }
}

int main(void) {
    read_config();
    window = vx_window_create("Settings", WIDTH, HEIGHT);
    if (!window) {
        fprintf(stderr, "settings: no desktop to open a window on\n");
        return 1;
    }
    int buttons = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            return 0;
        }
        if (e.type == VX_GUI_CLOSE) {
            vx_window_destroy(window);
            return 0;
        }
        if (e.type == VX_GUI_POINTER) {
            hot = control_at(e.x, e.y);
            if ((e.buttons & 1) && !(buttons & 1)) {
                click(hot);
            }
            buttons = e.buttons;
        } else if (e.type == VX_GUI_KEY && typing) {
            if (e.key == VX_KEY_ENTER && e.value) {
                typing = false;
                click(APPLY);
            } else {
                vx_field_key(image, sizeof(image), &e);
            }
        }
    }
}
