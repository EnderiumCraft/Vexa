#include <stddef.h>
#include <vexa/fb.h>

/* A copy: the bootloader's structure goes away when its memory is reclaimed. */
static struct limine_framebuffer fb_info;
static struct limine_framebuffer *fb;
static volatile bool hidden; /* A program has the screen: the kernel doesn't draw. */

const struct limine_framebuffer *fb_limine(void) {
    return fb;
}

void fb_set_hidden(bool value) {
    hidden = value;
}

bool fb_init(struct limine_framebuffer *framebuffer) {
    if (framebuffer->bpp != 32) {
        return false;
    }
    fb_info = *framebuffer;
    fb_info.edid = NULL;
    fb_info.modes = NULL;
    fb = &fb_info;
    return true;
}

uint64_t fb_width(void) {
    return fb ? fb->width : 0;
}

uint64_t fb_height(void) {
    return fb ? fb->height : 0;
}

static uint32_t pack_color(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (r >> (8 - fb->red_mask_size)) << fb->red_mask_shift |
           (g >> (8 - fb->green_mask_size)) << fb->green_mask_shift |
           (b >> (8 - fb->blue_mask_size)) << fb->blue_mask_shift;
}

static uint32_t *row_ptr(uint64_t y) {
    return (uint32_t *)((uint8_t *)fb->address + y * fb->pitch);
}

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb) {
    if (!fb || hidden) {
        return;
    }
    uint32_t color = pack_color(rgb);
    for (uint64_t row = y; row < y + h && row < fb->height; row++) {
        uint32_t *line = row_ptr(row);
        for (uint64_t col = x; col < x + w && col < fb->width; col++) {
            line[col] = color;
        }
    }
}

void fb_draw_bitmap8(uint64_t x, uint64_t y, const uint8_t *rows, uint64_t height,
                     uint32_t fg_rgb, uint32_t bg_rgb) {
    if (!fb || hidden || x + 8 > fb->width || y + height > fb->height) {
        return;
    }
    uint32_t fg = pack_color(fg_rgb), bg = pack_color(bg_rgb);
    for (uint64_t r = 0; r < height; r++) {
        uint32_t *line = row_ptr(y + r) + x;
        for (int c = 0; c < 8; c++) {
            line[c] = (rows[r] & (0x80 >> c)) ? fg : bg;
        }
    }
}
