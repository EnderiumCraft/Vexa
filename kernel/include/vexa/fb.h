#ifndef VEXA_FB_H
#define VEXA_FB_H

#include <stdbool.h>
#include <stdint.h>
#include <limine.h>

/* Returns false if the framebuffer format is not supported (only 32 bpp is). */
bool fb_init(struct limine_framebuffer *fb);
uint64_t fb_width(void);
uint64_t fb_height(void);
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb);
/* Draws an 8-pixel-wide bitmap, one byte per row, MSB on the left. */
void fb_draw_bitmap8(uint64_t x, uint64_t y, const uint8_t *rows, uint64_t height,
                     uint32_t fg_rgb, uint32_t bg_rgb);

#endif
