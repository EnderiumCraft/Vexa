#include <stddef.h>
#include <vexa/fb.h>

static struct limine_framebuffer *fb;

void fb_init(struct limine_framebuffer *framebuffer) {
    fb = framebuffer;
}

static uint32_t pack_color(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (r >> (8 - fb->red_mask_size)) << fb->red_mask_shift |
           (g >> (8 - fb->green_mask_size)) << fb->green_mask_shift |
           (b >> (8 - fb->blue_mask_size)) << fb->blue_mask_shift;
}

void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb) {
    if (!fb || fb->bpp != 32) {
        return;
    }
    uint32_t color = pack_color(rgb);
    for (uint64_t row = y; row < y + h && row < fb->height; row++) {
        uint32_t *line = (uint32_t *)((uint8_t *)fb->address + row * fb->pitch);
        for (uint64_t col = x; col < x + w && col < fb->width; col++) {
            line[col] = color;
        }
    }
}

/* A "V" built from blocks, so we can see the kernel is alive without a font. */
void fb_draw_splash(void) {
    if (!fb) {
        return;
    }
    for (uint64_t row = 0; row < fb->height; row++) {
        uint32_t shade = (uint32_t)(0x10 + row * 0x30 / fb->height);
        fb_fill_rect(0, row, fb->width, 1, shade << 16 | 0x08 << 8 | (shade + 0x20));
    }

    uint64_t cell = fb->height / 24;
    uint64_t cx = fb->width / 2;
    uint64_t top = fb->height / 2 - cell * 4;
    for (uint64_t i = 0; i < 8; i++) {
        uint64_t offset = (7 - i) * cell / 2 + cell / 2;
        fb_fill_rect(cx - offset - cell / 2, top + i * cell, cell, cell, 0xb07cff);
        fb_fill_rect(cx + offset - cell / 2, top + i * cell, cell, cell, 0xb07cff);
    }
}
