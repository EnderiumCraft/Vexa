#include <stdbool.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/font.h>
#include <vexa/string.h>

/*
 * Text console on the framebuffer. Understands the common ANSI/VT100 escape
 * sequences programs use to move the cursor, erase, and set colours
 * (ESC [ ... A B C D G H J K d f m s u), so full-screen programs work.
 */

/* Large enough for a 2560x1600 screen; bigger screens use the top-left part. */
#define MAX_COLS 320
#define MAX_ROWS 100
#define TAB_WIDTH 8
#define MAX_PARAMS 8

struct cell {
    char c;
    uint32_t fg;
    uint32_t bg;
};

/* The 16 ANSI colours, tuned to sit on Vexa's dark purple background. */
static const uint32_t palette[16] = {
    0x160d26, 0xff6b81, 0x7ee787, 0xf2cc60, 0x79a8ff, 0xb07cff, 0x56d4dd, 0xe4dcf2,
    0x6e6485, 0xff8fa0, 0xa5f0ab, 0xffe08a, 0xa3c3ff, 0xcca6ff, 0x8de8ef, 0xffffff,
};

/* The text is kept in memory so scrolling only writes to video memory, which is
 * slow to read back from on real hardware. */
static struct cell cells[MAX_ROWS][MAX_COLS];
static uint32_t cols, rows;
static uint32_t cursor_x, cursor_y, saved_x, saved_y;
static uint32_t current_fg = CONSOLE_COLOR_TEXT;
static uint32_t current_bg = CONSOLE_COLOR_BACKGROUND;
static bool bold;
static bool ready;

enum { STATE_NORMAL, STATE_ESCAPE, STATE_CSI } state;
static int params[MAX_PARAMS];
static int param_count;
static bool private_sequence;

static void draw_cell(uint32_t x, uint32_t y) {
    struct cell *cell = &cells[y][x];
    char c = cell->c;
    if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_GLYPH_COUNT) {
        c = ' ';
    }
    fb_draw_bitmap8(x * FONT_WIDTH, y * FONT_HEIGHT, font_glyphs[c - FONT_FIRST_CHAR],
                    FONT_HEIGHT, cell->fg, cell->bg);
}

static void draw_cursor(bool visible) {
    if (visible) {
        fb_fill_rect(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT + FONT_HEIGHT - 3,
                     FONT_WIDTH, 2, CONSOLE_COLOR_ACCENT);
    } else {
        draw_cell(cursor_x, cursor_y);
    }
}

static void blank(uint32_t x, uint32_t y) {
    cells[y][x] = (struct cell){.c = 0, .fg = current_fg, .bg = current_bg};
    draw_cell(x, y);
}

void console_clear(void) {
    if (!ready) {
        return;
    }
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            cells[y][x] = (struct cell){.c = 0, .fg = current_fg, .bg = CONSOLE_COLOR_BACKGROUND};
        }
    }
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
    for (uint32_t x = 0; x < cols; x++) {
        cells[rows - 1][x] = (struct cell){.c = 0, .fg = current_fg, .bg = current_bg};
    }
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

static int param(int index, int fallback) {
    return index < param_count && params[index] > 0 ? params[index] : fallback;
}

static void set_graphics(void) {
    if (param_count == 0) {
        param_count = 1;
        params[0] = 0;
    }
    for (int i = 0; i < param_count; i++) {
        int p = params[i];
        if (p == 0) {
            current_fg = CONSOLE_COLOR_TEXT;
            current_bg = CONSOLE_COLOR_BACKGROUND;
            bold = false;
        } else if (p == 1) {
            bold = true;
        } else if (p == 22) {
            bold = false;
        } else if (p == 7) {
            uint32_t t = current_fg; /* Reverse video. */
            current_fg = current_bg;
            current_bg = t;
        } else if (p >= 30 && p <= 37) {
            current_fg = palette[(p - 30) + (bold ? 8 : 0)];
        } else if (p == 39) {
            current_fg = CONSOLE_COLOR_TEXT;
        } else if (p >= 40 && p <= 47) {
            current_bg = palette[p - 40];
        } else if (p == 49) {
            current_bg = CONSOLE_COLOR_BACKGROUND;
        } else if (p >= 90 && p <= 97) {
            current_fg = palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            current_bg = palette[p - 100 + 8];
        }
    }
}

static void erase_display(int mode) {
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            bool before = y < cursor_y || (y == cursor_y && x < cursor_x);
            if (mode == 2 || mode == 3 || (mode == 0 && !before) || (mode == 1 && (before || (y == cursor_y && x == cursor_x)))) {
                blank(x, y);
            }
        }
    }
}

static void erase_line(int mode) {
    for (uint32_t x = 0; x < cols; x++) {
        if (mode == 2 || (mode == 0 && x >= cursor_x) || (mode == 1 && x <= cursor_x)) {
            blank(x, cursor_y);
        }
    }
}

static uint32_t clamp(int value, uint32_t max) {
    return value < 0 ? 0 : (uint32_t)value >= max ? max - 1 : (uint32_t)value;
}

static void control_sequence(char final) {
    switch (final) {
    case 'A': cursor_y = clamp((int)cursor_y - param(0, 1), rows); break;
    case 'B': cursor_y = clamp((int)cursor_y + param(0, 1), rows); break;
    case 'C': cursor_x = clamp((int)cursor_x + param(0, 1), cols); break;
    case 'D': cursor_x = clamp((int)cursor_x - param(0, 1), cols); break;
    case 'G': cursor_x = clamp(param(0, 1) - 1, cols); break;
    case 'd': cursor_y = clamp(param(0, 1) - 1, rows); break;
    case 'H':
    case 'f':
        cursor_y = clamp(param(0, 1) - 1, rows);
        cursor_x = clamp(param(1, 1) - 1, cols);
        break;
    case 'J': erase_display(param_count ? params[0] : 0); break;
    case 'K': erase_line(param_count ? params[0] : 0); break;
    case 'm': set_graphics(); break;
    case 's': saved_x = cursor_x, saved_y = cursor_y; break;
    case 'u': cursor_x = saved_x, cursor_y = saved_y; break;
    default: break; /* Modes (h/l), scroll regions (r) and such: not supported, ignored. */
    }
}

/* Returns true if `c` was part of an escape sequence. */
static bool escape(char c) {
    switch (state) {
    case STATE_NORMAL:
        if (c == 0x1b) {
            state = STATE_ESCAPE;
            return true;
        }
        return false;
    case STATE_ESCAPE:
        if (c == '[') {
            state = STATE_CSI;
            param_count = 0;
            private_sequence = false;
            memset(params, 0, sizeof(params));
            return true;
        }
        if (c == '7') {
            saved_x = cursor_x, saved_y = cursor_y;
        } else if (c == '8') {
            cursor_x = saved_x, cursor_y = saved_y;
        } else if (c == 'c') {
            current_fg = CONSOLE_COLOR_TEXT;
            current_bg = CONSOLE_COLOR_BACKGROUND;
            console_clear();
        }
        state = STATE_NORMAL;
        return true;
    case STATE_CSI:
        if (c == '?' || c == '>' || c == '=') {
            private_sequence = true;
        } else if (c >= '0' && c <= '9') {
            if (param_count == 0) {
                param_count = 1;
            }
            if (param_count <= MAX_PARAMS) {
                params[param_count - 1] = params[param_count - 1] * 10 + (c - '0');
            }
        } else if (c == ';') {
            if (param_count == 0) {
                param_count = 1;
            }
            if (param_count < MAX_PARAMS) {
                param_count++;
            }
        } else if (c >= 0x40 && c <= 0x7e) {
            if (!private_sequence) {
                control_sequence(c);
            }
            state = STATE_NORMAL;
        }
        return true;
    }
    return false;
}

void console_putc(char c) {
    if (!ready) {
        return;
    }
    draw_cursor(false);
    if (escape(c)) {
        draw_cursor(true);
        return;
    }
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
            cells[cursor_y][cursor_x] = (struct cell){.c = ' ', .fg = current_fg, .bg = current_bg};
            draw_cell(cursor_x, cursor_y);
            cursor_x++;
        } while (cursor_x % TAB_WIDTH != 0 && cursor_x < cols);
        if (cursor_x >= cols) {
            newline();
        }
        break;
    case '\a':
        break;
    default:
        if ((uint8_t)c < ' ') {
            break;
        }
        cells[cursor_y][cursor_x] = (struct cell){.c = c, .fg = current_fg, .bg = current_bg};
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
