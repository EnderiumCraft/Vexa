/* edit: a text editor window. `edit [file]` opens a file (or a new one).
 * Arrows, Home/End and Page Up/Down move; typing, Tab, Enter, Backspace and
 * Delete edit; a click puts the cursor there. Ctrl+S saves (asking for a
 * name the first time), Ctrl+Q or the close button quits.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 680
#define HEIGHT 460
#define GUTTER 5   /* Characters for line numbers. */
#define MARGIN 6
#define STATUS 22
#define TAB 4

struct line {
    char *text;
    int length, capacity;
};

static struct vx_window *window;
static struct line *lines;
static int line_count, line_capacity;
static int row, col, top, left; /* The cursor; the first row and column shown. */
static char path[512];
static bool modified, asking; /* asking: for a file name to save as. */
static char answer[512];
static char message[160];

/* ---- The text ---- */

static bool reserve(struct line *l, int length) {
    if (length + 1 <= l->capacity) {
        return true;
    }
    int capacity = l->capacity ? l->capacity : 16;
    while (capacity < length + 1) {
        capacity *= 2;
    }
    char *text = realloc(l->text, (size_t)capacity);
    if (!text) {
        return false;
    }
    l->text = text;
    l->capacity = capacity;
    return true;
}

/* Inserts an empty line before `at`. */
static bool insert_line(int at) {
    if (line_count == line_capacity) {
        int capacity = line_capacity ? line_capacity * 2 : 64;
        struct line *more = realloc(lines, (size_t)capacity * sizeof(*lines));
        if (!more) {
            return false;
        }
        lines = more;
        line_capacity = capacity;
    }
    memmove(lines + at + 1, lines + at, (size_t)(line_count - at) * sizeof(*lines));
    lines[at] = (struct line){NULL, 0, 0};
    line_count++;
    return reserve(&lines[at], 0) && ((lines[at].text[0] = '\0'), true);
}

static void remove_line(int at) {
    free(lines[at].text);
    memmove(lines + at, lines + at + 1, (size_t)(line_count - at - 1) * sizeof(*lines));
    line_count--;
}

static void append(struct line *l, const char *text, int length) {
    if (reserve(l, l->length + length)) {
        memcpy(l->text + l->length, text, (size_t)length);
        l->length += length;
        l->text[l->length] = '\0';
    }
}

static void load(const char *file) {
    line_count = 0;
    insert_line(0);
    int handle = vx_open(file, VX_OPEN_READ);
    if (handle < 0) {
        snprintf(message, sizeof(message), "New file");
        return;
    }
    char buffer[4096];
    long n;
    while ((n = vx_read(handle, buffer, sizeof(buffer))) > 0) {
        for (long i = 0; i < n; i++) {
            if (buffer[i] == '\n') {
                insert_line(line_count);
            } else if (buffer[i] == '\t') {
                append(&lines[line_count - 1], "    ", TAB);
            } else if (buffer[i] != '\r') {
                append(&lines[line_count - 1], &buffer[i], 1);
            }
        }
    }
    vx_close(handle);
    if (line_count > 1 && lines[line_count - 1].length == 0) {
        remove_line(line_count - 1); /* The newline at the end. */
    }
    snprintf(message, sizeof(message), "%d lines", line_count);
}

static bool save(const char *file) {
    int handle = vx_open(file, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0) {
        snprintf(message, sizeof(message), "Can't save %s: %s", file, vx_strerror(handle));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < line_count && ok; i++) {
        ok = vx_write(handle, lines[i].text, (size_t)lines[i].length) == lines[i].length &&
             vx_write(handle, "\n", 1) == 1;
    }
    vx_close(handle);
    if (!ok) {
        snprintf(message, sizeof(message), "Couldn't write all of %s", file);
        return false;
    }
    modified = false;
    snprintf(message, sizeof(message), "Saved %s", file);
    return true;
}

/* The title shows the file's name, with a * when it has unsaved changes. */
static void update_title(void) {
    static char shown[600];
    char title[600];
    const char *name = path[0] ? strrchr(path, '/') ? strrchr(path, '/') + 1 : path : "Untitled";
    snprintf(title, sizeof(title), "%s%s - Text Editor", modified ? "*" : "", name);
    if (strcmp(title, shown)) {
        strcpy(shown, title);
        vx_window_set_title(window, title);
    }
}

/* ---- Editing ---- */

static void type_text(const char *text, int length) {
    struct line *l = &lines[row];
    if (!reserve(l, l->length + length)) {
        return;
    }
    memmove(l->text + col + length, l->text + col, (size_t)(l->length - col + 1));
    memcpy(l->text + col, text, (size_t)length);
    l->length += length;
    col += length;
    modified = true;
}

static void new_line(void) {
    if (!insert_line(row + 1)) {
        return;
    }
    struct line *l = &lines[row];
    append(&lines[row + 1], l->text + col, l->length - col);
    l->length = col;
    l->text[col] = '\0';
    row++;
    col = 0;
    modified = true;
}

static void backspace(void) {
    if (col > 0) {
        struct line *l = &lines[row];
        memmove(l->text + col - 1, l->text + col, (size_t)(l->length - col + 1));
        l->length--;
        col--;
        modified = true;
    } else if (row > 0) {
        col = lines[row - 1].length;
        append(&lines[row - 1], lines[row].text, lines[row].length);
        remove_line(row);
        row--;
        modified = true;
    }
}

static void delete_forward(void) {
    if (col < lines[row].length) {
        col++;
        backspace();
    } else if (row + 1 < line_count) {
        append(&lines[row], lines[row + 1].text, lines[row + 1].length);
        remove_line(row + 1);
        modified = true;
    }
}

/* ---- Drawing ---- */

static int text_rows(void) {
    return (window->surface.height - STATUS - 2 * MARGIN) / FONT_HEIGHT;
}

static int text_columns(void) {
    return (window->surface.width - 2 * MARGIN) / FONT_WIDTH - GUTTER - 1;
}

static void keep_cursor_visible(void) {
    int rows = text_rows(), columns = text_columns();
    if (row < top) {
        top = row;
    } else if (row >= top + rows) {
        top = row - rows + 1;
    }
    if (col < left) {
        left = col;
    } else if (col >= left + columns) {
        left = col - columns + 1;
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height, rows = text_rows(), columns = text_columns();
    keep_cursor_visible();
    vx_fill(s, 0, 0, w, h, VX_COLOR_VIEW);
    int text_x = MARGIN + (GUTTER + 1) * FONT_WIDTH;
    for (int i = 0; i < rows && top + i < line_count; i++) {
        int y = MARGIN + i * FONT_HEIGHT;
        char number[16];
        snprintf(number, sizeof(number), "%*d", GUTTER - 1, top + i + 1);
        vx_draw_text(s, MARGIN, y, number, top + i == row ? VX_COLOR_ACCENT : VX_COLOR_LINE,
                     VX_TRANSPARENT);
        struct line *l = &lines[top + i];
        if (l->length > left) {
            char shown[512];
            int n = l->length - left < columns ? l->length - left : columns;
            n = n < (int)sizeof(shown) - 1 ? n : (int)sizeof(shown) - 1;
            memcpy(shown, l->text + left, (size_t)n);
            shown[n] = '\0';
            vx_draw_text(s, text_x, y, shown, VX_COLOR_TEXT, VX_TRANSPARENT);
        }
    }
    if (!asking) {
        vx_fill(s, text_x + (col - left) * FONT_WIDTH, MARGIN + (row - top) * FONT_HEIGHT, 2,
                FONT_HEIGHT, VX_COLOR_ACCENT);
    }
    /* The status line: where the cursor is, or the question. */
    vx_fill(s, 0, h - STATUS, w, STATUS, VX_COLOR_WINDOW);
    if (asking) {
        vx_draw_text(s, MARGIN, h - STATUS + 3, "Save as:", VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_field(s, MARGIN + 9 * FONT_WIDTH, h - STATUS - 1, w - 10 * FONT_WIDTH - 2 * MARGIN,
                      answer, true);
    } else {
        char where[64];
        snprintf(where, sizeof(where), "Ln %d, Col %d   Ctrl+S save", row + 1, col + 1);
        int x = w - MARGIN - (int)strlen(where) * FONT_WIDTH;
        vx_draw_text_fit(s, MARGIN, h - STATUS + 3, x - 2 * MARGIN, message, VX_COLOR_DIM,
                         VX_TRANSPARENT);
        vx_draw_text(s, x, h - STATUS + 3, where, VX_COLOR_DIM, VX_TRANSPARENT);
    }
    vx_window_present(window, 0, 0, w, h);
    update_title();
}

/* ---- Events ---- */

static bool ctrl_held;

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl_held = e->value != 0;
        return;
    }
    if (e->value == 0) {
        return;
    }
    if (asking) {
        if (e->key == VX_KEY_ENTER && answer[0]) {
            asking = false;
            if (save(answer)) {
                strcpy(path, answer);
            }
        } else if (e->key == VX_KEY_ESC) {
            asking = false;
            snprintf(message, sizeof(message), "Not saved");
        } else {
            vx_field_key(answer, sizeof(answer), e);
        }
        return;
    }
    if (ctrl_held && e->key == 31) { /* S */
        if (path[0]) {
            save(path);
        } else {
            asking = true;
            strcpy(answer, "/tmp/");
        }
        return;
    }
    if (ctrl_held && e->key == 16) { /* Q */
        vx_window_destroy(window);
        exit(0);
    }
    int rows = text_rows();
    switch (e->key) {
    case VX_KEY_UP: row = row > 0 ? row - 1 : 0; break;
    case VX_KEY_DOWN: row = row + 1 < line_count ? row + 1 : row; break;
    case VX_KEY_PAGEUP: row = row > rows ? row - rows : 0; break;
    case VX_KEY_PAGEDOWN: row = row + rows < line_count ? row + rows : line_count - 1; break;
    case VX_KEY_LEFT:
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = lines[row].length;
        }
        break;
    case VX_KEY_RIGHT:
        if (col < lines[row].length) {
            col++;
        } else if (row + 1 < line_count) {
            row++;
            col = 0;
        }
        break;
    case VX_KEY_HOME: col = 0; break;
    case VX_KEY_END: col = lines[row].length; break;
    case VX_KEY_ENTER: new_line(); break;
    case VX_KEY_BACKSPACE: backspace(); break;
    case VX_KEY_DELETE: delete_forward(); break;
    case VX_KEY_TAB: type_text("    ", TAB - col % TAB); break;
    default:
        if (e->character >= ' ' && e->character < 127 && !ctrl_held) {
            char c = (char)e->character;
            type_text(&c, 1);
        }
        return;
    }
    if (col > lines[row].length) {
        col = lines[row].length;
    }
}

static void pointer(const struct vx_gui_event *e, int *buttons) {
    bool click = (e->buttons & 1) && !(*buttons & 1);
    *buttons = e->buttons;
    if (e->wheel) {
        top -= e->wheel * 3;
        int max = line_count - text_rows();
        top = top > max ? max : top;
        top = top < 0 ? 0 : top;
        /* Keep the cursor in view without undoing the scroll. */
        if (row < top) {
            row = top;
        } else if (row >= top + text_rows()) {
            row = top + text_rows() - 1;
        }
        if (col > lines[row].length) {
            col = lines[row].length;
        }
    }
    if (click && !asking) {
        int r = top + (e->y - MARGIN) / FONT_HEIGHT;
        int c = left + (e->x - MARGIN) / FONT_WIDTH - GUTTER - 1;
        row = r < 0 ? 0 : r >= line_count ? line_count - 1 : r;
        col = c < 0 ? 0 : c > lines[row].length ? lines[row].length : c;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        strncpy(path, argv[1], sizeof(path) - 1);
    }
    window = vx_window_create_flags("Text Editor", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "edit: no desktop to open a window on\n");
        return 1;
    }
    if (path[0]) {
        load(path);
    } else {
        insert_line(0);
        snprintf(message, sizeof(message), "New file");
    }
    int buttons = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            return 0;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY:
            key(&e);
            break;
        case VX_GUI_POINTER:
            pointer(&e, &buttons);
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 200 && e.height >= 120) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
