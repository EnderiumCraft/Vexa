#ifndef VEXA_FB_H
#define VEXA_FB_H

#include <stdbool.h>
#include <stdint.h>
#include <limine.h>

/* Returns false if the framebuffer format is not supported (only 32 bpp is). */
bool fb_init(struct limine_framebuffer *fb);
/* The framebuffer as the bootloader described it (NULL if none). */
const struct limine_framebuffer *fb_limine(void);
/* While hidden, drawing does nothing: a program owns the screen. */
void fb_set_hidden(bool hidden);
uint64_t fb_width(void);
/* Adds /dev/display0 for programs (dev/display.c). */
void display_init(void);

/* The display for kernel code (the Linux subsystem's DRM device), as a
 * program has it through /dev/display0: one owner at a time (`who`). */
struct vx_display_info;
struct vx_display_modes;
int display_claim(const void *who);   /* -VX_EBUSY if someone else has it. */
void display_release(const void *who); /* The console comes back. */
void *display_frame(struct vx_display_info *info); /* The frame buffer, and its shape. */
void display_modes(struct vx_display_modes *modes);
int display_set_mode(const void *who, unsigned width, unsigned height);
uint64_t fb_height(void);
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb);
/* Draws an 8-pixel-wide bitmap, one byte per row, MSB on the left. */
void fb_draw_bitmap8(uint64_t x, uint64_t y, const uint8_t *rows, uint64_t height,
                     uint32_t fg_rgb, uint32_t bg_rgb);

#endif
