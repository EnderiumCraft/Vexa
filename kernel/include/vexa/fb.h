#ifndef VEXA_FB_H
#define VEXA_FB_H

#include <stdint.h>
#include <limine.h>

void fb_init(struct limine_framebuffer *fb);
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t rgb);
void fb_draw_splash(void);

#endif
