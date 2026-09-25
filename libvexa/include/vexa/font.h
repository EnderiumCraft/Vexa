#ifndef LIBVEXA_FONT_H
#define LIBVEXA_FONT_H

#include <stdint.h>

/* The console's bitmap font (Spleen 8x16), for drawing text: printable ASCII
 * 0x20-0x7e, one byte per row, most significant bit on the left. */
#define FONT_WIDTH 8
#define FONT_HEIGHT 16
#define FONT_FIRST_CHAR 0x20
#define FONT_GLYPH_COUNT 95

extern const uint8_t font_glyphs[FONT_GLYPH_COUNT][FONT_HEIGHT];

#endif
