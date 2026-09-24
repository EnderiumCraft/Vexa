#include <stdbool.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/font.h>
#include <vexa/string.h>

/* Large enough for a 2560x1600 screen; bigger screens use the top-left part. */
#define MAX_COLS 320
#define MAX_ROWS 100
#define TAB_WIDTH 8

struct cell {
    char c;
    uint32_t fg;
};

/* The text is kept in memory so scrolling only writes to video memory, which is
 * slow to read back from on real hardware. */
static struct cell cells[MAX_ROWS][MAX_COLS];
static uint32_t cols, rows;
static uint32_t cursor_x, cursor_y;
static uint32_t current_fg = CONSOLE_COLOR_TEXT;
static bool ready;

static void draw_cell(uint32_t x, uint32_t y) {
    struct cell *cell = &cells[y][x];
    char c = cell->c;
    if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_GLYPH_COUNT) {
        c = ' ';
    }
    fb_draw_bitmap8(x * FONT_WIDTH, y * FONT_HEIGHT, font_glyphs[c - FONT_FIRST_CHAR],
                    FONT_HEIGHT, cell->fg, CONSOLE_COLOR_BACKGROUND);
}

static void draw_cursor(bool visible) {
    struct cell *cell = &cells[cursor_y][cursor_x];
    if (visible) {
        fb_fill_rect(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT + FONT_HEIGHT - 3,
                     FONT_WIDTH, 2, CONSOLE_COLOR_ACCENT);
    } else if (cell->c) {
        draw_cell(cursor_x, cursor_y);
    } else {
        fb_fill_rect(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT,
                     CONSOLE_COLOR_BACKGROUND);
    }
}

void console_clear(void) {
    if (!ready) {
        return;
    }
    memset(cells, 0, sizeof(cells));
    fb_fill_rect(0, 0, fb_width(), fb_height(), CONSOLE_COLOR_BACKGROUND);
    cursor_x = cursor_y = 0;
    draw_cursor(true);
}

void console_init(void) {
    cols = fb_width() / FONT_WIDTH;
    rows = fb_height() / FONT_HEIGHT;
    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }
    ready = cols > 0 && rows > 0;
    console_clear();
}

static void scroll(void) {
    memmove(cells[0], cells[1], sizeof(cells[0]) * (rows - 1));
    memset(cells[rows - 1], 0, sizeof(cells[0]));
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            draw_cell(x, y);
        }
    }
}

static void newline(void) {
    cursor_x = 0;
    if (++cursor_y == rows) {
        scroll();
        cursor_y = rows - 1;
    }
}

void console_putc(char c) {
    if (!ready) {
        return;
    }
    draw_cursor(false);
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = cols - 1;
        }
        break;
    case '\t':
        do {
            console_putc(' ');
        } while (cursor_x % TAB_WIDTH != 0);
        return; /* The recursive calls already drew the cursor. */
    default:
        cells[cursor_y][cursor_x] = (struct cell){.c = c, .fg = current_fg};
        draw_cell(cursor_x, cursor_y);
        if (++cursor_x == cols) {
            newline();
        }
        break;
    }
    draw_cursor(true);
}

void console_write(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}

void console_set_color(uint32_t fg_rgb) {
    current_fg = fg_rgb;
}

void console_reset_color(void) {
    current_fg = CONSOLE_COLOR_TEXT;
}
