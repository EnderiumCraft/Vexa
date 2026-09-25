/* term: a terminal window on the desktop, running vsh (or the program given).
 *
 * The shell runs on a pseudo-terminal (/dev/pts/N); term holds the other
 * side, draws what the shell writes (with the common VT100/ANSI escape
 * sequences, like the console), and types the keys pressed in its window.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define COLUMNS 80 /* To start with; the window can be resized. */
#define ROWS 24
#define MAX_COLUMNS 250
#define MAX_ROWS 120
#define MARGIN 4
#define MAX_PARAMS 8
#define TAB_WIDTH 8

#define COLOR_TEXT 0xe4dcf2
#define COLOR_BACKGROUND 0x160d26
#define COLOR_CURSOR 0xb07cff

/* The console's 16 colours. */
static const uint32_t palette[16] = {
    0x160d26, 0xff6b81, 0x7ee787, 0xf2cc60, 0x79a8ff, 0xb07cff, 0x56d4dd, 0xe4dcf2,
    0x6e6485, 0xff8fa0, 0xa5f0ab, 0xffe08a, 0xa3c3ff, 0xcca6ff, 0x8de8ef, 0xffffff,
};

struct cell {
    char c;
    uint32_t fg, bg;
};

static struct cell cells[MAX_ROWS][MAX_COLUMNS];
static bool dirty[MAX_ROWS];
static int columns = COLUMNS, rows = ROWS;
static int cursor_x, cursor_y, saved_x, saved_y;
static uint32_t fg = COLOR_TEXT, bg = COLOR_BACKGROUND;
static bool bold, focused = true;
static enum { NORMAL, ESCAPE, CSI } state;
static int params[MAX_PARAMS], param_count;
static bool private_sequence;
static struct vx_window *window;

/* ---- The screen ---- */

static void blank(int x, int y) {
    cells[y][x] = (struct cell){' ', fg, bg};
    dirty[y] = true;
}

static void scroll_up(void) {
    memmove(cells[0], cells[1], sizeof(cells[0]) * (rows - 1));
    for (int x = 0; x < columns; x++) {
        cells[rows - 1][x] = (struct cell){' ', fg, bg};
    }
    for (int y = 0; y < rows; y++) {
        dirty[y] = true;
    }
}

static void line_feed(void) {
    if (++cursor_y == rows) {
        scroll_up();
        cursor_y = rows - 1;
    }
}

static int param(int index, int fallback) {
    return index < param_count && params[index] > 0 ? params[index] : fallback;
}

static int clamp(int value, int max) {
    return value < 0 ? 0 : value >= max ? max - 1 : value;
}

static void set_graphics(void) {
    if (param_count == 0) {
        param_count = 1;
        params[0] = 0;
    }
    for (int i = 0; i < param_count; i++) {
        int p = params[i];
        if (p == 0) {
            fg = COLOR_TEXT, bg = COLOR_BACKGROUND, bold = false;
        } else if (p == 1) {
            bold = true;
        } else if (p == 22) {
            bold = false;
        } else if (p == 7) {
            uint32_t t = fg;
            fg = bg, bg = t;
        } else if (p >= 30 && p <= 37) {
            fg = palette[(p - 30) + (bold ? 8 : 0)];
        } else if (p == 39) {
            fg = COLOR_TEXT;
        } else if (p >= 40 && p <= 47) {
            bg = palette[p - 40];
        } else if (p == 49) {
            bg = COLOR_BACKGROUND;
        } else if (p >= 90 && p <= 97) {
            fg = palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            bg = palette[p - 100 + 8];
        }
    }
}

static void erase_display(int mode) {
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < columns; x++) {
            bool before = y < cursor_y || (y == cursor_y && x < cursor_x);
            if (mode == 2 || mode == 3 || (mode == 0 && !before) ||
                (mode == 1 && (before || (y == cursor_y && x == cursor_x)))) {
                blank(x, y);
            }
        }
    }
}

static void erase_line(int mode) {
    for (int x = 0; x < columns; x++) {
        if (mode == 2 || (mode == 0 && x >= cursor_x) || (mode == 1 && x <= cursor_x)) {
            blank(x, cursor_y);
        }
    }
}

static void control_sequence(char final) {
    switch (final) {
    case 'A': cursor_y = clamp(cursor_y - param(0, 1), rows); break;
    case 'B': cursor_y = clamp(cursor_y + param(0, 1), rows); break;
    case 'C': cursor_x = clamp(cursor_x + param(0, 1), columns); break;
    case 'D': cursor_x = clamp(cursor_x - param(0, 1), columns); break;
    case 'G': cursor_x = clamp(param(0, 1) - 1, columns); break;
    case 'd': cursor_y = clamp(param(0, 1) - 1, rows); break;
    case 'H':
    case 'f':
        cursor_y = clamp(param(0, 1) - 1, rows);
        cursor_x = clamp(param(1, 1) - 1, columns);
        break;
    case 'J': erase_display(param_count ? params[0] : 0); break;
    case 'K': erase_line(param_count ? params[0] : 0); break;
    case 'm': set_graphics(); break;
    case 's': saved_x = cursor_x, saved_y = cursor_y; break;
    case 'u': cursor_x = saved_x, cursor_y = saved_y; break;
    default: break;
    }
}

static bool escape(char c) {
    switch (state) {
    case NORMAL:
        if (c == 0x1b) {
            state = ESCAPE;
            return true;
        }
        return false;
    case ESCAPE:
        if (c == '[') {
            state = CSI;
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
            fg = COLOR_TEXT, bg = COLOR_BACKGROUND;
            cursor_x = cursor_y = 0;
            erase_display(2);
        }
        state = NORMAL;
        return true;
    case CSI:
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
            state = NORMAL;
        }
        return true;
    }
    return false;
}

static void put(char c) {
    int old_y = cursor_y;
    if (!escape(c)) {
        switch (c) {
        case '\n': line_feed(); break;
        case '\r': cursor_x = 0; break;
        case '\b':
            if (cursor_x > 0) {
                cursor_x--;
            }
            break;
        case '\t':
            do {
                blank(cursor_x, cursor_y);
                cursor_x++;
            } while (cursor_x % TAB_WIDTH != 0 && cursor_x < columns);
            if (cursor_x >= columns) {
                cursor_x = columns - 1;
            }
            break;
        default:
            if ((unsigned char)c < ' ') {
                break;
            }
            if (cursor_x >= columns) { /* Wrap before writing past the edge. */
                cursor_x = 0;
                line_feed();
            }
            cells[cursor_y][cursor_x] = (struct cell){c, fg, bg};
            dirty[cursor_y] = true;
            cursor_x++;
            break;
        }
    }
    dirty[old_y] = dirty[cursor_y] = true; /* The cursor moved. */
}

/* Draws the rows that changed and shows them. */
static void redraw(void) {
    int first = -1, last = -1;
    struct vx_surface *s = &window->surface;
    for (int y = 0; y < rows; y++) {
        if (!dirty[y]) {
            continue;
        }
        dirty[y] = false;
        first = first < 0 ? y : first;
        last = y;
        int py = MARGIN + y * FONT_HEIGHT;
        for (int x = 0; x < columns; x++) {
            struct cell *cell = &cells[y][x];
            vx_draw_char(s, MARGIN + x * FONT_WIDTH, py, cell->c, cell->fg, cell->bg);
        }
        if (y == cursor_y && cursor_x < columns) {
            int cx = MARGIN + cursor_x * FONT_WIDTH;
            if (focused) {
                vx_fill(s, cx, py + FONT_HEIGHT - 3, FONT_WIDTH, 2, COLOR_CURSOR);
            } else {
                vx_fill(s, cx, py + FONT_HEIGHT - 1, FONT_WIDTH, 1, COLOR_CURSOR);
            }
        }
    }
    if (first >= 0) {
        vx_window_present(window, 0, MARGIN + first * FONT_HEIGHT, s->width,
                          (last - first + 1) * FONT_HEIGHT);
    }
}

/* A new size from the desktop: as many rows and columns as fit. Text stays
 * where it was (the bottom rows, if there are fewer), and the programs on
 * the terminal hear of it (SIGWINCH). */
static void resize(int master, int width, int height) {
    int new_columns = (width - 2 * MARGIN) / FONT_WIDTH;
    int new_rows = (height - 2 * MARGIN) / FONT_HEIGHT;
    new_columns = new_columns < 10 ? 10 : new_columns > MAX_COLUMNS ? MAX_COLUMNS : new_columns;
    new_rows = new_rows < 2 ? 2 : new_rows > MAX_ROWS ? MAX_ROWS : new_rows;
    if (vx_window_resize(window, 2 * MARGIN + new_columns * FONT_WIDTH,
                         2 * MARGIN + new_rows * FONT_HEIGHT)) {
        return;
    }
    int lost = cursor_y - (new_rows - 1); /* Lines to drop off the top. */
    if (lost > 0) {
        memmove(cells[0], cells[lost], sizeof(cells[0]) * (size_t)(rows - lost));
        cursor_y -= lost;
        saved_y = saved_y >= lost ? saved_y - lost : 0;
    }
    int kept = lost > 0 ? rows - lost : rows;
    for (int y = 0; y < new_rows; y++) {
        for (int x = y < kept ? columns : 0; x < new_columns; x++) {
            cells[y][x] = (struct cell){' ', COLOR_TEXT, COLOR_BACKGROUND};
        }
    }
    columns = new_columns;
    rows = new_rows;
    cursor_x = clamp(cursor_x, columns + 1);
    saved_x = clamp(saved_x, columns);
    saved_y = clamp(saved_y, rows);
    struct vx_surface *s = &window->surface;
    vx_fill(s, 0, 0, s->width, s->height, COLOR_BACKGROUND);
    for (int y = 0; y < rows; y++) {
        dirty[y] = true;
    }
    redraw();
    vx_window_present(window, 0, 0, s->width, s->height);
    struct vx_tty_size size = {(unsigned short)rows, (unsigned short)columns};
    vx_control(master, VX_TTY_SET_SIZE, &size, sizeof(size));
}

/* ---- Keys ---- */

static void type_key(int master, const struct vx_gui_event *e) {
    if (e->value == 0) {
        return; /* Only presses and repeats type. */
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
    if (sequence) {
        vx_write(master, sequence, strlen(sequence));
        return;
    }
    char c = (char)e->character;
    if (c == '\b') {
        c = 0x7f; /* Backspace sends DEL, like other terminals. */
    } else if (c == '\n') {
        c = '\r';
    }
    if (c) {
        vx_write(master, &c, 1);
    }
}

/* ---- Starting the shell ---- */

static int start_shell(int *master_out, const char *program) {
    int master = vx_open("/dev/ptmx", VX_OPEN_READ | VX_OPEN_WRITE);
    if (master < 0) {
        return master;
    }
    int number;
    struct vx_tty_size size = {ROWS, COLUMNS};
    if (vx_control(master, VX_TTY_PTY_NUMBER, &number, sizeof(number)) ||
        vx_control(master, VX_TTY_SET_SIZE, &size, sizeof(size))) {
        return -VX_EIO;
    }
    char path[32];
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    int terminal = vx_open(path, VX_OPEN_READ | VX_OPEN_WRITE);
    if (terminal < 0) {
        return terminal;
    }
    const char *argv[] = {program};
    const char *envp[] = {"TERM=vt100", "PATH=/bin:/linux/bin:/linux/usr/bin:/linux/sbin:/linux/usr/sbin"};
    struct vx_spawn spawn = {
        .argv = argv, .argc = 1, .envp = envp, .envc = 2,
        .handles = {terminal, terminal, terminal}, .flags = VX_SPAWN_NEW_GROUP,
    };
    int process = vx_spawn(program, &spawn);
    vx_close(terminal); /* The shell has it now. */
    *master_out = master;
    return process;
}

int main(int argc, char **argv) {
    const char *program = argc > 1 ? argv[1] : "/bin/vsh";
    window = vx_window_create_flags("Terminal", 2 * MARGIN + columns * FONT_WIDTH,
                                    2 * MARGIN + rows * FONT_HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "term: no desktop to open a window on\n");
        return 1;
    }
    vx_fill(&window->surface, 0, 0, window->surface.width, window->surface.height,
            COLOR_BACKGROUND);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < columns; x++) {
            cells[y][x] = (struct cell){' ', fg, bg};
        }
        dirty[y] = true;
    }
    vx_window_present(window, 0, 0, window->surface.width, window->surface.height);

    int master;
    int shell = start_shell(&master, program);
    if (shell < 0) {
        fprintf(stderr, "term: can't start %s: %s\n", program, vx_strerror(shell));
        return 1;
    }
    for (;;) {
        redraw();
        struct vx_poll polls[2] = {{master, VX_POLL_READ, 0}, {vx_gui_handle(), VX_POLL_READ, 0}};
        if (vx_poll(polls, 2, -1) < 0) {
            continue;
        }
        if (polls[0].ready) {
            char buffer[4096];
            long n = vx_read(master, buffer, sizeof(buffer));
            if (n <= 0) {
                break; /* The shell and everything on the terminal are gone. */
            }
            for (long i = 0; i < n; i++) {
                put(buffer[i]);
            }
        }
        if (polls[1].ready) {
            struct vx_gui_event e;
            int got = vx_gui_wait(&e, 0);
            if (got < 0) {
                break; /* The desktop is gone. */
            }
            if (got == 0) {
                continue;
            }
            if (e.type == VX_GUI_CLOSE) {
                vx_kill(vx_handle_process_id(shell), VX_SIGHUP);
                break;
            }
            if (e.type == VX_GUI_KEY) {
                type_key(master, &e);
            } else if (e.type == VX_GUI_RESIZE) {
                resize(master, e.width, e.height);
            } else if (e.type == VX_GUI_FOCUS) {
                focused = e.value;
                dirty[cursor_y] = true;
            }
        }
    }
    vx_window_destroy(window);
    vx_close(master); /* Hangs up anything still on the terminal. */
    vx_wait(shell, 0);
    return 0;
}
