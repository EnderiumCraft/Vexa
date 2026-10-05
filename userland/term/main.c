/* term: a terminal window on the desktop, running vsh (or the program given).
 *
 * Each tab runs a shell on a pseudo-terminal (/dev/pts/N); term holds the
 * other side, draws what the shell writes (VT100/ANSI escape sequences with
 * 16, 256 and 24-bit colours, scroll regions and the alternate screen, so
 * vi, less and top work), and types the keys pressed in its window.
 *
 *     Ctrl+Shift+T, Ctrl+Shift+W     a new tab, close the tab
 *     Ctrl+PageUp/PageDown, Ctrl+Tab the previous, next tab
 *     Shift+PageUp/PageDown, wheel   scroll back through what went by
 *     drag (double click: a word)    select, which copies it
 *     Ctrl+Shift+C, Ctrl+Shift+V     copy, paste (the middle button pastes too)
 *     Ctrl+=, Ctrl+-, Ctrl+0         bigger, smaller, the usual size
 *
 * A right click opens a menu with the same.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

#define COLUMNS 80 /* To start with; the window can be resized. */
#define ROWS 24
#define MAX_COLUMNS 250
#define MAX_ROWS 120
#define HISTORY 800 /* Lines kept after they scroll off the top. */
#define MARGIN 4
#define MAX_PARAMS 16
#define TAB_WIDTH 8
#define MAX_TABS 8
#define TAB_BAR 28 /* Shown when there's more than one tab. */

#define COLOR_TEXT 0xe6e6e8
#define COLOR_BACKGROUND 0x1a1a1c
#define COLOR_CURSOR VX_COLOR_ACCENT /* The terminal stays dark; its cursor is the accent. */
#define COLOR_SELECTION 0x48484e

/* The console's 16 colours. */
static const uint32_t palette[16] = {
    0x1a1a1c, 0xff6b81, 0x7ee787, 0xf2cc60, 0x79a8ff, 0xb07cff, 0x56d4dd, 0xe6e6e8,
    0x6e6e74, 0xff8fa0, 0xa5f0ab, 0xffe08a, 0xa3c3ff, 0xcca6ff, 0x8de8ef, 0xffffff,
};

/* The 256 colours: the 16, a 6x6x6 cube, and 24 greys. */
static uint32_t color256(int n) {
    if (n < 16) {
        return palette[n];
    }
    if (n < 232) {
        n -= 16;
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        return (uint32_t)(levels[n / 36] << 16 | levels[n / 6 % 6] << 8 | levels[n % 6]);
    }
    int grey = 8 + (n - 232) * 10;
    return (uint32_t)(grey << 16 | grey << 8 | grey);
}

struct cell {
    uint32_t c; /* Unicode. */
    uint32_t fg, bg;
};

struct tab {
    int master, shell; /* The pty's master side; the shell's process handle. */
    struct cell *grid;   /* rows x MAX_COLUMNS */
    struct cell *saved_grid; /* The main screen, while the alternate one shows. */
    struct cell *history; /* HISTORY x MAX_COLUMNS, a ring */
    int history_start, history_count;
    int scroll; /* How many lines back the view is (0: the bottom). */
    int cursor_x, cursor_y, saved_x, saved_y;
    int top, bottom; /* The scroll region (rows). */
    uint32_t fg, bg;
    bool bold, inverse, cursor_hidden, alternate;
    enum { NORMAL, ESCAPE, CSI, OSC, OSC_ESCAPE, CHARSET } state;
    int params[MAX_PARAMS], param_count;
    bool private_sequence;
    char osc[128];
    int osc_length;
    uint32_t pending;
    int pending_left;
    char title[64];
};

static struct tab *tabs[MAX_TABS];
static int tab_count, current;
static int columns = COLUMNS, rows = ROWS;
static int cell_w = VX_CELL_WIDTH, cell_h = VX_LINE_HEIGHT;
static const struct vx_font *font;
static int font_size = VX_MONO_FONT_SIZE;
static bool focused = true, shift, ctrl;
static struct vx_window *window;
static bool all_dirty = true;
static long scrollbar_until; /* The scroll bar shows while scrolling, and a moment after. */

/* The selection, in "view" lines: history lines first, then the screen. */
static bool selecting, has_selection;
static int sel_x0, sel_y0, sel_x1, sel_y1;

/* Menu (right click). */
static bool menu_open;
static int menu_x, menu_y, menu_hot = -1;
enum { M_COPY, M_PASTE, M_NEW_TAB, M_CLOSE_TAB, M_BIGGER, M_SMALLER };
static const struct vx_menu_item menu_items[] = {
    {"Copy", "Ctrl+Shift+C", false}, {"Paste", "Ctrl+Shift+V", false}, {NULL, NULL, false},
    {"New Tab", "Ctrl+Shift+T", false}, {"Close Tab", "Ctrl+Shift+W", false},
    {NULL, NULL, false}, {"Bigger", "Ctrl+=", false}, {"Smaller", "Ctrl+-", false},
};
static const int menu_actions[] = {M_COPY, M_PASTE, -1, M_NEW_TAB, M_CLOSE_TAB, -1, M_BIGGER, M_SMALLER};
#define MENU_COUNT (int)(sizeof(menu_items) / sizeof(menu_items[0]))

static int top_bar(void) {
    return tab_count > 1 ? TAB_BAR : 0;
}

static struct tab *T(void) {
    return tabs[current];
}

/* ---- The screen ---- */

static struct cell *at(struct tab *t, int x, int y) {
    return &t->grid[y * MAX_COLUMNS + x];
}

static void blank_line(struct tab *t, int y) {
    for (int x = 0; x < MAX_COLUMNS; x++) {
        *at(t, x, y) = (struct cell){' ', t->fg, t->bg};
    }
}

static void blank(struct tab *t, int x, int y) {
    *at(t, x, y) = (struct cell){' ', t->fg, t->bg};
}

/* A line leaving the top of the main screen goes into the history. */
static void keep_line(struct tab *t, int y) {
    if (t->alternate || !t->history) {
        return;
    }
    int slot = (t->history_start + t->history_count) % HISTORY;
    if (t->history_count == HISTORY) {
        t->history_start = (t->history_start + 1) % HISTORY;
    } else {
        t->history_count++;
    }
    memcpy(t->history + (long)slot * MAX_COLUMNS, at(t, 0, y), MAX_COLUMNS * sizeof(struct cell));
    if (t->scroll) {
        t->scroll = t->scroll + 1 > t->history_count ? t->history_count : t->scroll + 1;
    }
}

static void scroll_region_up(struct tab *t, int n) {
    for (int i = 0; i < n; i++) {
        if (t->top == 0) {
            keep_line(t, 0);
        }
        memmove(at(t, 0, t->top), at(t, 0, t->top + 1),
                sizeof(struct cell) * MAX_COLUMNS * (size_t)(t->bottom - t->top));
        blank_line(t, t->bottom);
    }
    all_dirty = true;
}

static void scroll_region_down(struct tab *t, int n) {
    for (int i = 0; i < n; i++) {
        memmove(at(t, 0, t->top + 1), at(t, 0, t->top),
                sizeof(struct cell) * MAX_COLUMNS * (size_t)(t->bottom - t->top));
        blank_line(t, t->top);
    }
    all_dirty = true;
}

static void line_feed(struct tab *t) {
    if (t->cursor_y == t->bottom) {
        scroll_region_up(t, 1);
    } else if (t->cursor_y < rows - 1) {
        t->cursor_y++;
    }
}

static int param(struct tab *t, int index, int fallback) {
    return index < t->param_count && t->params[index] > 0 ? t->params[index] : fallback;
}

static int clamp(int value, int max) {
    return value < 0 ? 0 : value >= max ? max - 1 : value;
}

static void set_graphics(struct tab *t) {
    if (t->param_count == 0) {
        t->param_count = 1;
        t->params[0] = 0;
    }
    for (int i = 0; i < t->param_count; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->fg = COLOR_TEXT, t->bg = COLOR_BACKGROUND, t->bold = t->inverse = false;
        } else if (p == 1) {
            t->bold = true;
        } else if (p == 22) {
            t->bold = false;
        } else if (p == 7) {
            t->inverse = true;
        } else if (p == 27) {
            t->inverse = false;
        } else if (p >= 30 && p <= 37) {
            t->fg = palette[(p - 30) + (t->bold ? 8 : 0)];
        } else if (p == 39) {
            t->fg = COLOR_TEXT;
        } else if (p >= 40 && p <= 47) {
            t->bg = palette[p - 40];
        } else if (p == 49) {
            t->bg = COLOR_BACKGROUND;
        } else if (p >= 90 && p <= 97) {
            t->fg = palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            t->bg = palette[p - 100 + 8];
        } else if ((p == 38 || p == 48) && i + 1 < t->param_count) {
            /* 256 colours (5;n) and 24-bit (2;r;g;b). */
            uint32_t color;
            if (t->params[i + 1] == 5 && i + 2 < t->param_count) {
                color = color256(t->params[i + 2] & 255);
                i += 2;
            } else if (t->params[i + 1] == 2 && i + 4 < t->param_count) {
                color = (uint32_t)((t->params[i + 2] & 255) << 16 | (t->params[i + 3] & 255) << 8 |
                                   (t->params[i + 4] & 255));
                i += 4;
            } else {
                continue;
            }
            if (p == 38) {
                t->fg = color;
            } else {
                t->bg = color;
            }
        }
    }
}

static void erase_display(struct tab *t, int mode) {
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < columns; x++) {
            bool before = y < t->cursor_y || (y == t->cursor_y && x < t->cursor_x);
            if (mode == 2 || mode == 3 || (mode == 0 && !before) ||
                (mode == 1 && (before || (y == t->cursor_y && x == t->cursor_x)))) {
                blank(t, x, y);
            }
        }
    }
    if (mode == 3) {
        t->history_count = 0;
        t->scroll = 0;
    }
    all_dirty = true;
}

static void erase_line(struct tab *t, int mode) {
    for (int x = 0; x < columns; x++) {
        if (mode == 2 || (mode == 0 && x >= t->cursor_x) || (mode == 1 && x <= t->cursor_x)) {
            blank(t, x, t->cursor_y);
        }
    }
}

static void set_alternate(struct tab *t, bool on) {
    if (on == t->alternate || !t->saved_grid) {
        return;
    }
    if (on) {
        memcpy(t->saved_grid, t->grid, sizeof(struct cell) * MAX_COLUMNS * MAX_ROWS);
        t->saved_x = t->cursor_x, t->saved_y = t->cursor_y;
        for (int y = 0; y < MAX_ROWS; y++) {
            blank_line(t, y);
        }
    } else {
        memcpy(t->grid, t->saved_grid, sizeof(struct cell) * MAX_COLUMNS * MAX_ROWS);
        t->cursor_x = t->saved_x, t->cursor_y = t->saved_y;
    }
    t->alternate = on;
    all_dirty = true;
}

static void private_mode(struct tab *t, char final) {
    bool on = final == 'h';
    for (int i = 0; i < t->param_count; i++) {
        switch (t->params[i]) {
        case 25: t->cursor_hidden = !on; break;
        case 47:
        case 1047:
        case 1049: set_alternate(t, on); break;
        }
    }
}

static void control_sequence(struct tab *t, char final) {
    switch (final) {
    case 'A': t->cursor_y = clamp(t->cursor_y - param(t, 0, 1), rows); break;
    case 'B': t->cursor_y = clamp(t->cursor_y + param(t, 0, 1), rows); break;
    case 'C': t->cursor_x = clamp(t->cursor_x + param(t, 0, 1), columns); break;
    case 'D': t->cursor_x = clamp(t->cursor_x - param(t, 0, 1), columns); break;
    case 'E': t->cursor_y = clamp(t->cursor_y + param(t, 0, 1), rows), t->cursor_x = 0; break;
    case 'F': t->cursor_y = clamp(t->cursor_y - param(t, 0, 1), rows), t->cursor_x = 0; break;
    case 'G': t->cursor_x = clamp(param(t, 0, 1) - 1, columns); break;
    case 'd': t->cursor_y = clamp(param(t, 0, 1) - 1, rows); break;
    case 'H':
    case 'f':
        t->cursor_y = clamp(param(t, 0, 1) - 1, rows);
        t->cursor_x = clamp(param(t, 1, 1) - 1, columns);
        break;
    case 'J': erase_display(t, t->param_count ? t->params[0] : 0); break;
    case 'K': erase_line(t, t->param_count ? t->params[0] : 0); break;
    case 'm': set_graphics(t); break;
    case 's': t->saved_x = t->cursor_x, t->saved_y = t->cursor_y; break;
    case 'u': t->cursor_x = t->saved_x, t->cursor_y = t->saved_y; break;
    case 'r': { /* The scroll region. */
        int top = param(t, 0, 1) - 1, bottom = param(t, 1, rows) - 1;
        if (top < bottom && bottom < rows) {
            t->top = top, t->bottom = bottom;
            t->cursor_x = t->cursor_y = 0;
        }
        break;
    }
    case 'S': scroll_region_up(t, param(t, 0, 1)); break;
    case 'T': scroll_region_down(t, param(t, 0, 1)); break;
    case 'L':   /* Insert lines. */
    case 'M': { /* Delete lines. */
        if (t->cursor_y < t->top || t->cursor_y > t->bottom) {
            break;
        }
        int saved_top = t->top;
        t->top = t->cursor_y;
        if (final == 'L') {
            scroll_region_down(t, param(t, 0, 1));
        } else {
            scroll_region_up(t, param(t, 0, 1));
        }
        t->top = saved_top;
        break;
    }
    case 'P': { /* Delete characters. */
        int n = param(t, 0, 1);
        struct cell *line = at(t, 0, t->cursor_y);
        for (int x = t->cursor_x; x < columns; x++) {
            line[x] = x + n < columns ? line[x + n] : (struct cell){' ', t->fg, t->bg};
        }
        break;
    }
    case '@': { /* Insert blank characters. */
        int n = param(t, 0, 1);
        struct cell *line = at(t, 0, t->cursor_y);
        for (int x = columns - 1; x >= t->cursor_x; x--) {
            line[x] = x - n >= t->cursor_x ? line[x - n] : (struct cell){' ', t->fg, t->bg};
        }
        break;
    }
    case 'X': /* Erase characters. */
        for (int x = t->cursor_x; x < t->cursor_x + param(t, 0, 1) && x < columns; x++) {
            blank(t, x, t->cursor_y);
        }
        break;
    default: break;
    }
}

static bool escape(struct tab *t, char c) {
    switch (t->state) {
    case NORMAL:
        if (c == 0x1b) {
            t->state = ESCAPE;
            return true;
        }
        return false;
    case ESCAPE:
        t->state = NORMAL;
        if (c == '[') {
            t->state = CSI;
            t->param_count = 0;
            t->private_sequence = false;
            memset(t->params, 0, sizeof(t->params));
        } else if (c == ']') {
            t->state = OSC;
            t->osc_length = 0;
        } else if (c == '(' || c == ')') {
            t->state = CHARSET; /* A character set: ignored (UTF-8 is it). */
        } else if (c == '7') {
            t->saved_x = t->cursor_x, t->saved_y = t->cursor_y;
        } else if (c == '8') {
            t->cursor_x = t->saved_x, t->cursor_y = t->saved_y;
        } else if (c == 'M') { /* Reverse line feed. */
            if (t->cursor_y == t->top) {
                scroll_region_down(t, 1);
            } else if (t->cursor_y > 0) {
                t->cursor_y--;
            }
        } else if (c == 'D') {
            line_feed(t);
        } else if (c == 'c') {
            t->fg = COLOR_TEXT, t->bg = COLOR_BACKGROUND;
            t->cursor_x = t->cursor_y = 0;
            t->top = 0, t->bottom = rows - 1;
            erase_display(t, 2);
        }
        return true;
    case CHARSET:
        t->state = NORMAL;
        return true;
    case OSC: /* ESC ] n ; text BEL: the title. */
        if (c == 7 || c == 0x1b) {
            t->osc[t->osc_length] = '\0';
            if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') {
                snprintf(t->title, sizeof(t->title), "%s", t->osc + 2);
                all_dirty = true;
            }
            t->state = c == 7 ? NORMAL : OSC_ESCAPE;
        } else if (t->osc_length < (int)sizeof(t->osc) - 1) {
            t->osc[t->osc_length++] = c;
        }
        return true;
    case OSC_ESCAPE: /* The \ after ESC that ends it. */
        t->state = NORMAL;
        return true;
    case CSI:
        if (c == '?' || c == '>' || c == '=') {
            t->private_sequence = true;
        } else if (c >= '0' && c <= '9') {
            if (t->param_count == 0) {
                t->param_count = 1;
            }
            if (t->param_count <= MAX_PARAMS) {
                t->params[t->param_count - 1] = t->params[t->param_count - 1] * 10 + (c - '0');
            }
        } else if (c == ';' || c == ':') {
            if (t->param_count == 0) {
                t->param_count = 1;
            }
            if (t->param_count < MAX_PARAMS) {
                t->param_count++;
            }
        } else if (c >= 0x40 && c <= 0x7e) {
            if (t->private_sequence) {
                if (c == 'h' || c == 'l') {
                    private_mode(t, c);
                }
            } else {
                control_sequence(t, c);
            }
            t->state = NORMAL;
        }
        return true;
    }
    return false;
}

static void put(struct tab *t, char byte) {
    unsigned char b = (unsigned char)byte;
    uint32_t c = b;
    if (t->pending_left && (b & 0xc0) == 0x80) {
        t->pending = t->pending << 6 | (b & 0x3f);
        if (--t->pending_left) {
            return;
        }
        c = t->pending;
    } else if (b >= 0xc0 && b < 0xf8 && t->state == NORMAL) {
        t->pending_left = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
        t->pending = b & (b >= 0xf0 ? 0x07 : b >= 0xe0 ? 0x0f : 0x1f);
        return;
    } else {
        t->pending_left = 0;
        if (b >= 0x80) {
            c = 0xfffd; /* Not UTF-8. */
        }
    }
    if (c < 0x80 && escape(t, (char)c)) {
        return;
    }
    switch (c) {
    case '\n':
    case 0x0b:
    case 0x0c: line_feed(t); break;
    case '\r': t->cursor_x = 0; break;
    case '\b':
        if (t->cursor_x > 0) {
            t->cursor_x--;
        }
        break;
    case '\t':
        do {
            t->cursor_x++;
        } while (t->cursor_x % TAB_WIDTH != 0 && t->cursor_x < columns);
        if (t->cursor_x >= columns) {
            t->cursor_x = columns - 1;
        }
        break;
    default:
        if (c < ' ' || c == 0x7f) {
            break;
        }
        if (t->cursor_x >= columns) { /* Wrap before writing past the edge. */
            t->cursor_x = 0;
            line_feed(t);
        }
        uint32_t fg = t->fg, bg = t->bg;
        if (t->inverse) {
            fg = t->bg, bg = t->fg;
        }
        *at(t, t->cursor_x, t->cursor_y) = (struct cell){c, fg, bg};
        t->cursor_x++;
        break;
    }
}

/* ---- Selection: in view lines (the history's, then the screen's) ---- */

static const struct cell *view_line(struct tab *t, int line) {
    if (line < t->history_count) {
        return t->history + (long)((t->history_start + line) % HISTORY) * MAX_COLUMNS;
    }
    int y = line - t->history_count;
    return y >= 0 && y < rows ? at(t, 0, y) : NULL;
}

static bool selected_cell(int x, int line) {
    if (!has_selection) {
        return false;
    }
    int ax = sel_x0, ay = sel_y0, bx = sel_x1, by = sel_y1;
    if (ay > by || (ay == by && ax > bx)) {
        int tx = ax, ty = ay;
        ax = bx, ay = by, bx = tx, by = ty;
    }
    if (line < ay || line > by) {
        return false;
    }
    if (line == ay && x < ax) {
        return false;
    }
    if (line == by && x > bx) {
        return false;
    }
    return true;
}

static void copy_selection(void) {
    if (!has_selection) {
        return;
    }
    struct tab *t = T();
    int ay = sel_y0 < sel_y1 || (sel_y0 == sel_y1 && sel_x0 <= sel_x1) ? sel_y0 : sel_y1;
    int by = ay == sel_y0 ? sel_y1 : sel_y0;
    size_t capacity = (size_t)(by - ay + 1) * (MAX_COLUMNS * 4 + 1) + 1, n = 0;
    char *text = malloc(capacity);
    if (!text) {
        return;
    }
    for (int line = ay; line <= by; line++) {
        const struct cell *cells = view_line(t, line);
        if (!cells) {
            continue;
        }
        size_t line_start = n;
        for (int x = 0; x < columns; x++) {
            if (selected_cell(x, line)) {
                n += (size_t)vx_utf8_encode(cells[x].c ? cells[x].c : ' ', text + n);
            }
        }
        while (n > line_start && text[n - 1] == ' ') {
            n--; /* Not the spaces at the end of a line. */
        }
        if (line != by) {
            text[n++] = '\n';
        }
    }
    vx_clipboard_set(text, n);
    printf("term: copied %zu bytes\n", n);
    fflush(stdout);
    free(text);
}

static void paste(void) {
    char *text = vx_clipboard_get();
    if (!text) {
        return;
    }
    struct tab *t = T();
    for (char *p = text; *p; p++) {
        if (*p == '\n') {
            *p = '\r';
        }
    }
    vx_write(t->master, text, strlen(text));
    t->scroll = 0;
    all_dirty = true;
    free(text);
}

/* ---- Drawing ---- */

static void draw_tab_bar(struct vx_surface *s) {
    if (tab_count < 2) {
        return;
    }
    /* (The terminal stays dark: a dark toolbar, the current tab a gel of
     * the accent.) */
    for (int row = 0; row < TAB_BAR; row++) {
        vx_fill(s, 0, row, s->width, 1, vx_mix(0x2c2c2f, 0x151517, row * 255 / (TAB_BAR - 1)));
    }
    vx_fill(s, 0, TAB_BAR - 1, s->width, 1, 0x05030a);
    int w = (s->width - 36) / tab_count;
    w = w > 200 ? 200 : w;
    for (int i = 0; i < tab_count; i++) {
        int x = 4 + i * w;
        bool on = i == current;
        if (on) {
            vx_draw_gel(s, x, 4, w - 4, TAB_BAR - 8, 7, VX_COLOR_ACCENT);
        } else {
            vx_fill_rounded(s, x, 4, w - 4, TAB_BAR - 8, 7, 0xffffff, 16);
        }
        const char *title = tabs[i]->title[0] ? tabs[i]->title : "vsh";
        vx_draw_text_fit(s, x + 8, 7, w - 32, title, on ? 0xffffff : 0xb8aed0, VX_TRANSPARENT);
        vx_draw_text(s, x + w - 20, 7, "x", on ? 0xffffff : 0x9c90b8, VX_TRANSPARENT);
    }
    vx_draw_text(s, 4 + tab_count * w + 6, 7, "+", COLOR_TEXT, VX_TRANSPARENT);
}

static void redraw(void) {
    struct vx_surface *s = &window->surface;
    struct tab *t = T();
    int top = top_bar();
    vx_fill(s, 0, top, s->width, s->height - top, COLOR_BACKGROUND);
    draw_tab_bar(s);
    int first_line = t->history_count - t->scroll; /* The view line at the top. */
    for (int y = 0; y < rows; y++) {
        const struct cell *cells = view_line(t, first_line + y);
        if (!cells) {
            continue;
        }
        int py = top + MARGIN + y * cell_h;
        for (int x = 0; x < columns; x++) {
            uint32_t fg = cells[x].fg, bg = cells[x].bg;
            if (selected_cell(x, first_line + y)) {
                bg = COLOR_SELECTION;
            }
            if (bg == COLOR_BACKGROUND && (cells[x].c == ' ' || !cells[x].c)) {
                continue;
            }
            vx_draw_cell(s, font, MARGIN + x * cell_w, py, cell_w, cell_h, cells[x].c, fg, bg);
        }
    }
    if (!t->scroll && !t->cursor_hidden && t->cursor_x < columns) {
        int cx = MARGIN + t->cursor_x * cell_w, cy = top + MARGIN + t->cursor_y * cell_h;
        if (focused) {
            vx_fill(s, cx, cy + cell_h - 3, cell_w, 2, COLOR_CURSOR);
        } else {
            vx_fill(s, cx, cy + cell_h - 1, cell_w, 1, COLOR_CURSOR);
        }
    }
    /* The scroll bar, while scrolling back. */
    int total = t->history_count + rows;
    if (t->history_count && (t->scroll || vx_uptime() < scrollbar_until)) {
        int area = s->height - top - 2 * MARGIN;
        int bar = area * rows / total;
        bar = bar < 16 ? 16 : bar;
        int y = top + MARGIN + (area - bar) * (t->history_count - t->scroll) / t->history_count;
        vx_fill(s, s->width - 7, y, 4, bar, 0x6e6e74);
    }
    if (menu_open) {
        vx_draw_menu(s, menu_x, menu_y, menu_items, MENU_COUNT, menu_hot);
    }
    vx_window_present(window, 0, 0, s->width, s->height);
    all_dirty = false;
    /* The window's title: the tab's. */
    static char shown[80];
    char title[80];
    snprintf(title, sizeof(title), "%s", t->title[0] ? t->title : "Terminal");
    if (strcmp(title, shown)) {
        snprintf(shown, sizeof(shown), "%s", title);
        vx_window_set_title(window, title);
    }
}

/* ---- Sizes ---- */

static void tell_size(struct tab *t) {
    struct vx_tty_size size = {(unsigned short)rows, (unsigned short)columns};
    vx_control(t->master, VX_TTY_SET_SIZE, &size, sizeof(size));
}

/* As many rows and columns as fit the window; text stays where it was (the
 * bottom rows, if there are fewer), and the programs hear of it (SIGWINCH). */
static void fit_grid(int width, int height) {
    int new_columns = (width - 2 * MARGIN) / cell_w;
    int new_rows = (height - top_bar() - 2 * MARGIN) / cell_h;
    new_columns = new_columns < 10 ? 10 : new_columns > MAX_COLUMNS ? MAX_COLUMNS : new_columns;
    new_rows = new_rows < 2 ? 2 : new_rows > MAX_ROWS ? MAX_ROWS : new_rows;
    for (int i = 0; i < tab_count; i++) {
        struct tab *t = tabs[i];
        int lost = t->cursor_y - (new_rows - 1); /* Lines to drop off the top. */
        if (lost > 0) {
            for (int y = 0; y < lost; y++) {
                keep_line(t, y);
            }
            memmove(t->grid, at(t, 0, lost), sizeof(struct cell) * MAX_COLUMNS * (size_t)(rows - lost));
            for (int y = rows - lost; y < rows; y++) {
                blank_line(t, y);
            }
            t->cursor_y -= lost;
            t->saved_y = t->saved_y >= lost ? t->saved_y - lost : 0;
        }
        for (int y = rows; y < new_rows; y++) {
            blank_line(t, y);
        }
        t->cursor_x = clamp(t->cursor_x, new_columns + 1);
        t->saved_x = clamp(t->saved_x, new_columns);
        t->saved_y = clamp(t->saved_y, new_rows);
        t->top = 0, t->bottom = new_rows - 1;
    }
    columns = new_columns;
    rows = new_rows;
    for (int i = 0; i < tab_count; i++) {
        tell_size(tabs[i]);
    }
    all_dirty = true;
}

/* A new size from the desktop: the window becomes whole rows and columns. */
static void resize(int width, int height) {
    int new_columns = (width - 2 * MARGIN) / cell_w;
    int new_rows = (height - top_bar() - 2 * MARGIN) / cell_h;
    new_columns = new_columns < 10 ? 10 : new_columns > MAX_COLUMNS ? MAX_COLUMNS : new_columns;
    new_rows = new_rows < 2 ? 2 : new_rows > MAX_ROWS ? MAX_ROWS : new_rows;
    if (vx_window_resize(window, 2 * MARGIN + new_columns * cell_w,
                         top_bar() + 2 * MARGIN + new_rows * cell_h)) {
        return;
    }
    fit_grid(window->surface.width, window->surface.height);
}

static void set_font_size(int size) {
    size = size < 9 ? 9 : size > 28 ? 28 : size;
    font_size = size;
    font = vx_font(VX_FACE_MONO, size);
    cell_w = vx_font_cell_width(font);
    cell_h = vx_font_height(font);
    if (size == VX_MONO_FONT_SIZE) {
        cell_w = VX_CELL_WIDTH, cell_h = VX_LINE_HEIGHT;
    }
    fit_grid(window->surface.width, window->surface.height);
    printf("term: font size %d (%dx%d)\n", size, columns, rows);
    fflush(stdout);
}

/* ---- Tabs ---- */

static const char *program = "/bin/vsh";

static int start_shell(struct tab *t) {
    int master = vx_open("/dev/ptmx", VX_OPEN_READ | VX_OPEN_WRITE);
    if (master < 0) {
        return master;
    }
    int number;
    struct vx_tty_size size = {(unsigned short)rows, (unsigned short)columns};
    if (vx_control(master, VX_TTY_PTY_NUMBER, &number, sizeof(number)) ||
        vx_control(master, VX_TTY_SET_SIZE, &size, sizeof(size))) {
        vx_close(master);
        return -VX_EIO;
    }
    char path[32];
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    int terminal = vx_open(path, VX_OPEN_READ | VX_OPEN_WRITE);
    if (terminal < 0) {
        vx_close(master);
        return terminal;
    }
    const char *argv[] = {program};
    char home[300], user[64];
    snprintf(home, sizeof(home), "HOME=%s", vx_home());
    const char *name = getenv("USER");
    snprintf(user, sizeof(user), "USER=%s", name ? name : "root");
    const char *envp[] = {"TERM=vt100", "COLORTERM=truecolor",
                          "PATH=/bin:/linux/bin:/linux/usr/bin:/linux/sbin:/linux/usr/sbin",
                          "LANG=C.UTF-8", home, user};
    struct vx_spawn spawn = {
        .argv = argv, .argc = 1, .envp = envp, .envc = 6,
        .handles = {terminal, terminal, terminal}, .flags = VX_SPAWN_NEW_GROUP,
    };
    int process = vx_spawn(program, &spawn);
    vx_close(terminal); /* The shell has it now. */
    if (process < 0) {
        vx_close(master);
        return process;
    }
    t->master = master;
    t->shell = process;
    return 0;
}

static bool new_tab(void) {
    if (tab_count == MAX_TABS) {
        return false;
    }
    struct tab *t = calloc(1, sizeof(*t));
    if (!t) {
        return false;
    }
    t->grid = malloc(sizeof(struct cell) * MAX_COLUMNS * MAX_ROWS);
    t->saved_grid = malloc(sizeof(struct cell) * MAX_COLUMNS * MAX_ROWS);
    t->history = malloc(sizeof(struct cell) * MAX_COLUMNS * HISTORY);
    if (!t->grid) {
        free(t->saved_grid);
        free(t->history);
        free(t);
        return false;
    }
    t->fg = COLOR_TEXT, t->bg = COLOR_BACKGROUND;
    for (int y = 0; y < MAX_ROWS; y++) {
        blank_line(t, y);
    }
    bool bar_before = tab_count > 1;
    tabs[tab_count++] = t;
    current = tab_count - 1;
    if (bar_before != (tab_count > 1)) {
        fit_grid(window->surface.width, window->surface.height); /* The tab bar appeared. */
    }
    t->top = 0, t->bottom = rows - 1;
    int error = start_shell(t);
    if (error) {
        fprintf(stderr, "term: can't start %s: %s\n", program, vx_strerror(error));
        tabs[--tab_count] = NULL;
        current = tab_count ? tab_count - 1 : 0;
        free(t->grid);
        free(t->saved_grid);
        free(t->history);
        free(t);
        return false;
    }
    printf("term: tab %d of %d\n", current + 1, tab_count);
    fflush(stdout);
    all_dirty = true;
    return true;
}

static void close_tab(int i) {
    struct tab *t = tabs[i];
    vx_kill(vx_handle_process_id(t->shell), VX_SIGHUP);
    vx_close(t->master); /* Hangs up anything still on the terminal. */
    vx_wait(t->shell, 0);
    vx_close(t->shell);
    free(t->grid);
    free(t->saved_grid);
    free(t->history);
    free(t);
    memmove(tabs + i, tabs + i + 1, sizeof(tabs[0]) * (size_t)(tab_count - i - 1));
    tab_count--;
    if (current >= tab_count) {
        current = tab_count - 1;
    }
    has_selection = false;
    if (tab_count == 1) {
        fit_grid(window->surface.width, window->surface.height); /* No tab bar now. */
    }
    all_dirty = true;
}

/* ---- Keys ---- */

static void scroll_view(int lines) {
    struct tab *t = T();
    t->scroll += lines;
    t->scroll = t->scroll < 0 ? 0 : t->scroll > t->history_count ? t->history_count : t->scroll;
    scrollbar_until = vx_uptime() + 1200;
    all_dirty = true;
}

static void act(int action) {
    switch (action) {
    case M_COPY: copy_selection(); break;
    case M_PASTE: paste(); break;
    case M_NEW_TAB: new_tab(); break;
    case M_CLOSE_TAB: close_tab(current); break;
    case M_BIGGER: set_font_size(font_size + 1); break;
    case M_SMALLER: set_font_size(font_size - 1); break;
    }
}

static void type_key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTSHIFT || e->key == VX_KEY_RIGHTSHIFT) {
        shift = e->value != 0;
        return;
    }
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
        return;
    }
    if (e->value == 0) {
        return; /* Only presses and repeats type. */
    }
    struct tab *t = T();
    /* The terminal's own keys. */
    if (ctrl && shift) {
        switch (e->key) {
        case 20: act(M_NEW_TAB); return;   /* T */
        case 17: act(M_CLOSE_TAB); return; /* W */
        case 46: act(M_COPY); return;      /* C */
        case 47: act(M_PASTE); return;     /* V */
        }
    }
    if (ctrl && !shift) {
        if (e->key == 13) { /* = */
            act(M_BIGGER);
            return;
        }
        if (e->key == 12) { /* - */
            act(M_SMALLER);
            return;
        }
        if (e->key == 11) { /* 0 */
            set_font_size(VX_MONO_FONT_SIZE);
            return;
        }
        if (e->key == VX_KEY_PAGEUP || e->key == VX_KEY_PAGEDOWN || e->key == VX_KEY_TAB) {
            if (tab_count > 1) {
                current = (current + (e->key == VX_KEY_PAGEUP ? tab_count - 1 : 1)) % tab_count;
                has_selection = false;
                all_dirty = true;
            }
            return;
        }
    }
    if (shift && (e->key == VX_KEY_PAGEUP || e->key == VX_KEY_PAGEDOWN)) {
        scroll_view((e->key == VX_KEY_PAGEUP ? 1 : -1) * (rows - 1));
        return;
    }
    const char *sequence = NULL;
    switch (e->key) {
    case VX_KEY_UP: sequence = "\x1b[A"; break;
    case VX_KEY_DOWN: sequence = "\x1b[B"; break;
    case VX_KEY_RIGHT: sequence = "\x1b[C"; break;
    case VX_KEY_LEFT: sequence = "\x1b[D"; break;
    case VX_KEY_HOME: sequence = "\x1b[H"; break;
    case VX_KEY_END: sequence = "\x1b[F"; break;
    case VX_KEY_DELETE: sequence = "\x1b[3~"; break;
    case VX_KEY_PAGEUP: sequence = "\x1b[5~"; break;
    case VX_KEY_PAGEDOWN: sequence = "\x1b[6~"; break;
    }
    if (t->scroll || has_selection) { /* Typing comes back to the bottom. */
        t->scroll = 0;
        has_selection = false;
        all_dirty = true;
    }
    if (sequence) {
        vx_write(t->master, sequence, strlen(sequence));
        return;
    }
    uint32_t c = (uint32_t)e->character;
    if (c == '\b') {
        c = 0x7f; /* Backspace sends DEL, like other terminals. */
    } else if (c == '\n') {
        c = '\r';
    }
    if (c) {
        char bytes[4]; /* UTF-8. */
        vx_write(t->master, bytes, (size_t)vx_utf8_encode(c, bytes));
    }
}

/* ---- The pointer ---- */

static int tab_width(void) {
    int w = (window->surface.width - 36) / tab_count;
    return w > 200 ? 200 : w;
}

/* Where a point is, in view cells. */
static void cell_at(int px, int py, int *x, int *line) {
    struct tab *t = T();
    *x = clamp((px - MARGIN) / cell_w, columns);
    int y = (py - top_bar() - MARGIN) / cell_h;
    y = y < 0 ? 0 : y >= rows ? rows - 1 : y;
    *line = t->history_count - t->scroll + y;
}

static bool word_char(uint32_t c) {
    return c > ' ' && !strchr("\"'`()[]{}<>|;,", c < 128 ? (int)c : 'a');
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool press = (e->buttons & 1) && !(*held & 1);
    bool release = !(e->buttons & 1) && (*held & 1);
    bool right = (e->buttons & 2) && !(*held & 2);
    bool middle = (e->buttons & 4) && !(*held & 4);
    *held = e->buttons;
    if (menu_open) {
        menu_hot = vx_menu_item_at(menu_items, MENU_COUNT, menu_x, menu_y, e->x, e->y);
        if (press || right) {
            menu_open = false;
            if (menu_hot >= 0) {
                act(menu_actions[menu_hot]);
            }
        }
        all_dirty = true;
        return;
    }
    if (e->wheel) {
        scroll_view(e->wheel * 3);
    }
    if (right) {
        int w, h;
        vx_menu_size(menu_items, MENU_COUNT, &w, &h);
        menu_x = e->x + w > window->surface.width ? window->surface.width - w : e->x;
        menu_y = e->y + h > window->surface.height ? window->surface.height - h : e->y;
        menu_open = true;
        menu_hot = -1;
        all_dirty = true;
        return;
    }
    if (middle) {
        paste();
        return;
    }
    if (press && e->y < top_bar()) {
        int w = tab_width(), i = (e->x - 4) / w;
        if (i >= 0 && i < tab_count) {
            if (e->x >= 4 + i * w + w - 24) {
                close_tab(i);
            } else {
                current = i;
                has_selection = false;
            }
        } else if (e->x >= 4 + tab_count * w) {
            new_tab();
        }
        all_dirty = true;
        return;
    }
    static long last_press;
    if (press) {
        int x, line;
        cell_at(e->x, e->y, &x, &line);
        long now = vx_uptime();
        if (now - last_press < 400) {
            /* A double click: the word there. */
            const struct cell *cells = view_line(T(), line);
            if (cells && word_char(cells[x].c)) {
                sel_x0 = sel_x1 = x;
                while (sel_x0 > 0 && word_char(cells[sel_x0 - 1].c)) {
                    sel_x0--;
                }
                while (sel_x1 + 1 < columns && word_char(cells[sel_x1 + 1].c)) {
                    sel_x1++;
                }
                sel_y0 = sel_y1 = line;
                has_selection = true;
                copy_selection();
            }
            selecting = false;
            last_press = 0;
            all_dirty = true;
            return;
        }
        last_press = now;
        sel_x0 = sel_x1 = x;
        sel_y0 = sel_y1 = line;
        selecting = true;
        has_selection = false;
        all_dirty = true;
        return;
    }
    if (selecting && (e->buttons & 1)) {
        cell_at(e->x, e->y, &sel_x1, &sel_y1);
        has_selection = sel_x1 != sel_x0 || sel_y1 != sel_y0;
        if (e->y < top_bar() + MARGIN) {
            scroll_view(1);
        } else if (e->y >= window->surface.height - MARGIN) {
            scroll_view(-1);
        }
        all_dirty = true;
    }
    if (selecting && release) {
        selecting = false;
        if (has_selection) {
            copy_selection(); /* Selecting copies. */
        }
    }
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    if (argc > 1) {
        program = argv[1];
    }
    font = vx_font(VX_FACE_MONO, font_size);
    window = vx_window_create_flags("Terminal", 2 * MARGIN + columns * cell_w,
                                    2 * MARGIN + rows * cell_h, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "term: no desktop to open a window on\n");
        return 1;
    }
    vx_window_set_cursor(window, VX_CURSOR_TEXT);
    if (!new_tab()) {
        return 1;
    }
    int held = 0;
    while (tab_count > 0) {
        if (all_dirty) {
            redraw();
        }
        struct vx_poll polls[MAX_TABS + 1];
        int count = 0;
        polls[count++] = (struct vx_poll){vx_gui_handle(), VX_POLL_READ, 0};
        for (int i = 0; i < tab_count; i++) {
            polls[count++] = (struct vx_poll){tabs[i]->master, VX_POLL_READ, 0};
        }
        long wait = vx_uptime() < scrollbar_until ? scrollbar_until - vx_uptime() + 10 : -1;
        int ready = (int)vx_poll(polls, (size_t)count, wait);
        if (ready <= 0) {
            all_dirty = true; /* (The scroll bar goes.) */
            continue;
        }
        for (int i = tab_count - 1; i >= 0; i--) {
            if (!polls[i + 1].ready) {
                continue;
            }
            char buffer[4096];
            long n = vx_read(tabs[i]->master, buffer, sizeof(buffer));
            if (n <= 0) {
                close_tab(i); /* The shell and everything on its terminal are gone. */
                continue;
            }
            for (long k = 0; k < n; k++) {
                put(tabs[i], buffer[k]);
            }
            if (i == current) {
                all_dirty = true;
            }
        }
        if (tab_count == 0) {
            break;
        }
        if (polls[0].ready) {
            struct vx_gui_event e;
            int got = vx_gui_wait(&e, 0);
            if (got < 0) {
                break; /* The desktop is gone. */
            }
            if (got == 0) {
                continue;
            }
            switch (e.type) {
            case VX_GUI_CLOSE:
                while (tab_count) {
                    close_tab(tab_count - 1);
                }
                break;
            case VX_GUI_KEY:
                if (menu_open && e.key == VX_KEY_ESC) {
                    menu_open = false;
                    all_dirty = true;
                } else {
                    type_key(&e);
                }
                break;
            case VX_GUI_POINTER: pointer(&e, &held); break;
            case VX_GUI_RESIZE: resize(e.width, e.height); break;
            case VX_GUI_FOCUS:
                focused = e.value;
                if (!focused) {
                    shift = ctrl = false;
                }
                all_dirty = true;
                break;
            case VX_GUI_THEME: all_dirty = true; break;
            }
        }
    }
    vx_window_destroy(window);
    return 0;
}
