#include <stdbool.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/font.h>
#include <vexa/splash_logo.h>
#include <vexa/string.h>
#include <vexa/version.h>

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

/* The 16 ANSI colours, tuned to sit on Vexa's dark teal background. */
static const uint32_t palette[16] = {
    0x062f2d, 0xff6b81, 0x7ee787, 0xf2cc60, 0x79a8ff, 0xc38cff, 0x2ec4b6, 0xe2f6f3,
    0x4f7f7a, 0xff8fa0, 0xa5f0ab, 0xffe08a, 0xa3c3ff, 0xd9b8ff, 0x7fe9dd, 0xffffff,
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
/* The loading screen: while it shows, text is kept but not drawn. */
static bool splash;
static uint32_t splash_lines;

enum { STATE_NORMAL, STATE_ESCAPE, STATE_CSI } state;
static int params[MAX_PARAMS];
static int param_count;
static bool private_sequence;

static void draw_cell(uint32_t x, uint32_t y) {
    if (splash) {
        return;
    }
    struct cell *cell = &cells[y][x];
    char c = cell->c;
    if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_GLYPH_COUNT) {
        c = ' ';
    }
    fb_draw_bitmap8(x * FONT_WIDTH, y * FONT_HEIGHT, font_glyphs[c - FONT_FIRST_CHAR],
                    FONT_HEIGHT, cell->fg, cell->bg);
}

static void draw_cursor(bool visible) {
    if (splash) {
        return;
    }
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

void console_redraw(void) {
    if (!ready) {
        return;
    }
    splash = false;
    fb_fill_rect(0, 0, fb_width(), fb_height(), CONSOLE_COLOR_BACKGROUND);
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            draw_cell(x, y);
        }
    }
    draw_cursor(true);
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
    if (!splash) {
        fb_fill_rect(0, 0, fb_width(), fb_height(), CONSOLE_COLOR_BACKGROUND);
    }
    cursor_x = cursor_y = 0;
    draw_cursor(true);
}

void console_init(bool with_splash) {
    cols = fb_width() / FONT_WIDTH;
    rows = fb_height() / FONT_HEIGHT;
    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }
    ready = cols > 0 && rows > 0;
    if (with_splash) {
        console_splash(true); /* Instead of clearing: the screen is drawn once. */
    }
    console_clear();
}

void console_splash(bool on);

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

static void draw_splash_bar(void);

static void newline(void) {
    if (splash) {
        splash_lines++;
        draw_splash_bar();
    }
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

/* ---- The loading screen ----
 *
 * The desktop's teal (the wallpaper's, from top to bottom), the Vexa logo in
 * the middle, the version under it, and a glossy bar like the desktop's gel
 * buttons that fills as boot goes on (with each kernel message, closer to full
 * but never there). */

#define SPLASH_TOP 0x44a89e    /* The wallpaper's teal at the top, */
#define SPLASH_BOTTOM 0x04302f /* and at the bottom. */
#define SPLASH_TEXT 0xcdf3ee
#define SPLASH_SHADOW 0x0b3b38
#define BAR_WIDTH 280
#define BAR_HEIGHT 10
#define BAR_TRACK 0x0a4a47
#define BAR_EDGE 0x1d8f85
#define BAR_LIGHT 0x8ff1e5
#define BAR_DARK 0x1a8f85

/* a to b by t (0 to 255), a channel at a time. */
static uint32_t splash_mix(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
        out |= (uint32_t)(ca + (cb - ca) * (int)t / 255) << shift;
    }
    return out;
}

static uint32_t splash_background(uint64_t y, uint64_t height) {
    return splash_mix(SPLASH_TOP, SPLASH_BOTTOM, height > 1 ? y * 255 / (height - 1) : 0);
}

static void draw_big_text(const char *text, uint64_t x, uint64_t y, uint32_t scale, uint32_t rgb) {
    for (; *text; text++, x += FONT_WIDTH * scale) {
        const uint8_t *glyph = font_glyphs[*text - FONT_FIRST_CHAR];
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            for (uint32_t column = 0; column < FONT_WIDTH; column++) {
                if (glyph[row] & (0x80 >> column)) {
                    fb_fill_rect(x + column * scale, y + row * scale, scale, scale, rgb);
                }
            }
        }
    }
}

static uint64_t logo_x(void) {
    return (fb_width() - SPLASH_LOGO_WIDTH) / 2;
}

/* The logo sits a little above the middle, as the eye expects. */
static uint64_t logo_y(void) {
    return fb_height() * 42 / 100 - SPLASH_LOGO_HEIGHT / 2;
}

static uint64_t version_y(void) {
    return logo_y() + SPLASH_LOGO_HEIGHT + 24;
}

static uint64_t bar_x(void) {
    return (fb_width() - BAR_WIDTH) / 2;
}

static uint64_t bar_y(void) {
    return version_y() + FONT_HEIGHT * 2 + 40;
}

/* The logo over the background, a row at a time (each pixel's alpha says how
 * much of the logo it is: a transparent one is the background). */
static void draw_logo(void) {
    uint32_t row[SPLASH_LOGO_WIDTH];
    for (uint64_t r = 0; r < SPLASH_LOGO_HEIGHT; r++) {
        uint32_t background = splash_background(logo_y() + r, fb_height());
        for (uint64_t c = 0; c < SPLASH_LOGO_WIDTH; c++) {
            uint32_t p = splash_logo[r][c];
            row[c] = splash_mix(background, p & 0xffffff, p >> 24);
        }
        fb_draw_row(logo_x(), logo_y() + r, SPLASH_LOGO_WIDTH, row);
    }
}

/* The bar's ends are round (as the desktop's gel buttons are): how far each row
 * of it is in from the end. */
static const uint8_t bar_inset[BAR_HEIGHT] = {3, 1, 0, 0, 0, 0, 0, 0, 1, 3};

static void draw_splash_track(void) {
    for (uint64_t r = 0; r < BAR_HEIGHT; r++) {
        uint64_t inset = bar_inset[r];
        fb_fill_rect(bar_x() + inset, bar_y() + r, BAR_WIDTH - 2 * inset, 1,
                     r == 0 ? BAR_EDGE : BAR_TRACK);
    }
}

/* The gel: light in its top half, the accent below, and a darker last line. */
static void draw_splash_bar(void) {
    uint64_t filled = (uint64_t)BAR_WIDTH * splash_lines / (splash_lines + 60);
    for (uint64_t r = 0; r < BAR_HEIGHT; r++) {
        uint64_t inset = bar_inset[r];
        uint64_t end = filled >= BAR_WIDTH ? BAR_WIDTH - inset : filled;
        if (end <= inset) {
            continue;
        }
        uint32_t color = r < BAR_HEIGHT / 2       ? BAR_LIGHT
                         : r + 1 < BAR_HEIGHT     ? CONSOLE_COLOR_ACCENT
                                                  : BAR_DARK;
        fb_fill_rect(bar_x() + inset, bar_y() + r, end - inset, 1, color);
    }
}

void console_splash(bool on) {
    if (!ready || on == splash) {
        return;
    }
    splash = on;
    if (!on) {
        console_redraw();
        return;
    }
    uint64_t width = fb_width(), height = fb_height();
    for (uint64_t y = 0; y < height; y++) {
        fb_fill_rect(0, y, width, 1, splash_background(y, height));
    }
    draw_logo();
    const char *version = VEXA_VERSION;
    uint64_t version_width = strlen(version) * FONT_WIDTH * 2;
    uint64_t version_x = (width - version_width) / 2;
    draw_big_text(version, version_x + 2, version_y() + 2, 2, SPLASH_SHADOW);
    draw_big_text(version, version_x, version_y(), 2, SPLASH_TEXT);
    draw_splash_track();
    draw_splash_bar();
}
