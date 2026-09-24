#ifndef VEXA_FONT_H
#define VEXA_FONT_H

#include <stdint.h>

/* Bitmap font for the kernel console: printable ASCII 0x20-0x7e. */
#define FONT_WIDTH 8
#define FONT_HEIGHT 16
#define FONT_FIRST_CHAR 0x20
#define FONT_GLYPH_COUNT 95

extern const uint8_t font_glyphs[FONT_GLYPH_COUNT][FONT_HEIGHT];

#endif
