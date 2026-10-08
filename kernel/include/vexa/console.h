#ifndef VEXA_CONSOLE_H
#define VEXA_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

/* Text console drawn on the framebuffer. Call after fb_init(). */
void console_init(bool with_splash);
void console_putc(char c);
void console_write(const char *s);
void console_set_color(uint32_t fg_rgb);
void console_reset_color(void);
void console_clear(void);
/* Draws the whole screen again (after a program had it: see fb_set_hidden). */
void console_redraw(void);
/* The loading screen (the boot text shows again when it goes). */
void console_splash(bool on);

#define CONSOLE_COLOR_TEXT 0xe2f6f3
#define CONSOLE_COLOR_ACCENT 0x2ec4b6
#define CONSOLE_COLOR_DIM 0x7fa9a3
#define CONSOLE_COLOR_ERROR 0xff6b81
#define CONSOLE_COLOR_BACKGROUND 0x062f2d

#endif
