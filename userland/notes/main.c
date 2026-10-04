/* notes: Notes. The notes on the left (the latest first, each named by its
 * first line), the one chosen on the right, wrapped to the width. They're
 * saved as you type, as text files in /home/Notes.
 *
 *     Ctrl+N a new note, Ctrl+Delete delete it, Ctrl+F search (the field
 *     above the list), Ctrl+C/X/V copy, cut, paste (Shift and the arrows select)
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/time.h>

#define WIDTH 780
#define HEIGHT 520
#define LIST 240
#define TOP 44
#define ITEM 52
#define PAD 16
#define FOLDER "/home/Notes"
#define MAX_NOTES 256
#define LINE_HEIGHT 19

struct note {
    char file[96];
    char title[80];
    char preview[100];
    long long modified;
};

static struct vx_window *window;
static struct note notes[MAX_NOTES];
static int note_count, chosen = -1, list_top;
static char search[64];
static bool searching;
static int shown[MAX_NOTES], shown_count;

/* The note being edited. */
static char *text;
static size_t length, capacity;
static size_t cursor, anchor = (size_t)-1;
static bool dirty;
static long changed_ms;
static int scroll; /* Lines scrolled. */
static bool ctrl, shift;

/* Wrapped lines: where each starts, in bytes. */
#define MAX_LINES 8192
static size_t line_start[MAX_LINES];
static int line_count;

static const struct vx_font *body_font(void) {
    return vx_font(VX_FACE_SANS, 14);
}

/* ---- The notes' files ---- */

static void title_of(const char *content, char *title, size_t tsize, char *preview, size_t psize) {
    const char *p = content;
    while (*p == '\n' || *p == ' ') {
        p++;
    }
    size_t n = strcspn(p, "\n");
    snprintf(title, tsize, "%.*s", (int)n, n ? p : "New Note");
    if (!n) {
        snprintf(title, tsize, "New Note");
    }
    const char *rest = p + n;
    while (*rest == '\n' || *rest == ' ') {
        rest++;
    }
    size_t m = strcspn(rest, "\n");
    snprintf(preview, psize, "%.*s", (int)m, rest);
}

static char *read_whole(const char *path, size_t *out_length) {
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    size_t size = 0, cap = 4096;
    char *data = malloc(cap);
    long n;
    while (data && (n = vx_read(handle, data + size, cap - size - 1)) > 0) {
        size += (size_t)n;
        if (cap - size < 1024) {
            char *more = realloc(data, cap * 2);
            if (!more) {
                break;
            }
            data = more;
            cap *= 2;
        }
    }
    vx_close(handle);
    if (data) {
        data[size] = '\0';
    }
    if (out_length) {
        *out_length = size;
    }
    return data;
}

static int newest_first(const void *a, const void *b) {
    const struct note *x = a, *y = b;
    return x->modified < y->modified ? 1 : x->modified > y->modified ? -1 : strcmp(x->file, y->file);
}

static bool matches(const struct note *n) {
    if (!search[0]) {
        return true;
    }
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, n->file);
    char *content = read_whole(path, NULL);
    bool found = false;
    if (content) {
        size_t k = strlen(search);
        for (char *p = content; *p && !found; p++) {
            size_t i = 0;
            while (i < k && p[i] && ((p[i] | 0x20) == (search[i] | 0x20))) {
                i++;
            }
            found = i == k;
        }
        free(content);
    }
    return found;
}

static void filter(void) {
    shown_count = 0;
    for (int i = 0; i < note_count; i++) {
        if (matches(&notes[i])) {
            shown[shown_count++] = i;
        }
    }
}

static void read_notes(void) {
    vx_mkdir("/home");
    vx_mkdir(FOLDER);
    note_count = 0;
    int handle = vx_open(FOLDER, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    struct vx_dir_entry entries[32];
    long n;
    while ((n = vx_read_dir(handle, entries, 32)) > 0) {
        for (long i = 0; i < n && note_count < MAX_NOTES; i++) {
            size_t len = strlen(entries[i].name);
            if (entries[i].name[0] == '.' || len < 5 || strcmp(entries[i].name + len - 4, ".txt")) {
                continue;
            }
            struct note *note = &notes[note_count++];
            snprintf(note->file, sizeof(note->file), "%s", entries[i].name);
            char path[200];
            snprintf(path, sizeof(path), "%s/%s", FOLDER, note->file);
            struct vx_stat st;
            note->modified = vx_stat(path, &st) == 0 ? st.modified : 0;
            char *content = read_whole(path, NULL);
            title_of(content ? content : "", note->title, sizeof(note->title), note->preview,
                     sizeof(note->preview));
            free(content);
        }
    }
    vx_close(handle);
    qsort(notes, (size_t)note_count, sizeof(notes[0]), newest_first);
    filter();
}

static void save_note(void) {
    if (!dirty || chosen < 0) {
        return;
    }
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, notes[chosen].file);
    int handle = vx_open(path, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle >= 0) {
        vx_write(handle, text, length);
        vx_close(handle);
    }
    dirty = false;
    title_of(text, notes[chosen].title, sizeof(notes[chosen].title), notes[chosen].preview,
             sizeof(notes[chosen].preview));
    notes[chosen].modified = vx_time();
    printf("notes: saved %s\n", notes[chosen].file);
    fflush(stdout);
}

static bool reserve(size_t more) {
    if (length + more + 1 <= capacity) {
        return true;
    }
    size_t cap = capacity ? capacity : 1024;
    while (cap < length + more + 1) {
        cap *= 2;
    }
    char *grown = realloc(text, cap);
    if (!grown) {
        return false;
    }
    text = grown;
    capacity = cap;
    return true;
}

static void choose(int index) {
    save_note();
    chosen = index;
    free(text);
    text = NULL;
    length = capacity = 0;
    cursor = 0;
    anchor = (size_t)-1;
    scroll = 0;
    if (index < 0) {
        return;
    }
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, notes[index].file);
    text = read_whole(path, &length);
    capacity = length + 1;
    if (!text) {
        reserve(16);
        text[0] = '\0';
        length = 0;
    }
    cursor = length;
}

static void new_note(void) {
    save_note();
    if (note_count == MAX_NOTES) {
        return;
    }
    char name[96];
    vx_unique_name(FOLDER, "Note.txt", name, sizeof(name));
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, name);
    int handle = vx_open(path, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle >= 0) {
        vx_close(handle);
    }
    read_notes();
    for (int i = 0; i < note_count; i++) {
        if (!strcmp(notes[i].file, name)) {
            choose(i);
        }
    }
    search[0] = '\0';
    filter();
    printf("notes: new note %s\n", name);
    fflush(stdout);
}

static void delete_note(void) {
    if (chosen < 0) {
        return;
    }
    char path[200];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, notes[chosen].file);
    dirty = false;
    vx_remove(path);
    chosen = -1;
    read_notes();
    choose(shown_count ? shown[0] : -1);
}

/* ---- Editing ---- */

static bool has_selection(void) {
    return anchor != (size_t)-1 && anchor != cursor;
}

static void delete_selection(void) {
    size_t a = anchor < cursor ? anchor : cursor, b = anchor < cursor ? cursor : anchor;
    memmove(text + a, text + b, length - b + 1);
    length -= b - a;
    cursor = a;
    anchor = (size_t)-1;
}

static void insert(const char *s, size_t n) {
    if (chosen < 0) {
        new_note();
    }
    if (has_selection()) {
        delete_selection();
    }
    anchor = (size_t)-1;
    if (!reserve(n)) {
        return;
    }
    memmove(text + cursor + n, text + cursor, length - cursor + 1);
    memcpy(text + cursor, s, n);
    length += n;
    cursor += n;
    dirty = true;
    changed_ms = vx_uptime();
}

/* Lays the text out in lines that fit `width`, breaking at spaces. */
static void wrap(int width) {
    const struct vx_font *f = body_font();
    line_count = 0;
    size_t at = 0;
    while (line_count < MAX_LINES) {
        line_start[line_count++] = at;
        if (at >= length) {
            break;
        }
        size_t end = at;
        size_t last_space = (size_t)-1;
        int x = 0;
        while (end < length && text[end] != '\n') {
            const char *p = text + end;
            uint32_t c = vx_utf8_next(&p);
            char glyph[5] = "";
            glyph[vx_utf8_encode(c, glyph)] = '\0';
            int gw = vx_text_width_font(f, glyph);
            if (x + gw > width && end > at) {
                break;
            }
            if (c == ' ') {
                last_space = end;
            }
            x += gw;
            end = (size_t)(p - text);
        }
        if (end < length && text[end] == '\n') {
            at = end + 1;
        } else if (end < length && last_space != (size_t)-1 && last_space > at) {
            at = last_space + 1;
        } else {
            at = end;
        }
    }
}

static int line_of(size_t at) {
    int line = 0;
    while (line + 1 < line_count && line_start[line + 1] <= at) {
        line++;
    }
    return line;
}

static int x_of(size_t at) {
    int line = line_of(at);
    return vx_text_width_bytes(body_font(), text + line_start[line], at - line_start[line]);
}

/* The byte in a line closest to an x. */
static size_t byte_at(int line, int x) {
    size_t start = line_start[line];
    size_t end = line + 1 < line_count ? line_start[line + 1] : length;
    if (end > start && end <= length && end > 0 && (text[end - 1] == '\n' || text[end - 1] == ' ') &&
        line + 1 < line_count) {
        end--;
    }
    size_t best = start;
    for (size_t at = start; at <= end;) {
        int w = vx_text_width_bytes(body_font(), text + start, at - start);
        if (w > x) {
            int before = vx_text_width_bytes(body_font(), text + start, best - start);
            return x - before < w - x ? best : at;
        }
        best = at;
        if (at == end) {
            break;
        }
        const char *p = text + at;
        vx_utf8_next(&p);
        at = (size_t)(p - text);
    }
    return best;
}

/* ---- Drawing ---- */

static int editor_x(void) {
    return LIST + PAD;
}

static int editor_width(void) {
    return window->surface.width - LIST - 2 * PAD;
}

static int editor_rows(void) {
    return (window->surface.height - TOP - PAD) / LINE_HEIGHT;
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_VIEW);
    /* The list. */
    vx_fill(s, 0, 0, LIST, h, vx_theme.sidebar);
    vx_fill(s, LIST - 1, 0, 1, h, VX_COLOR_LINE);
    vx_draw_field(s, 10, 10, LIST - 52, search, searching);
    if (!search[0] && !searching) {
        vx_draw_text(s, 16, 14, "Search", VX_COLOR_DIM, VX_TRANSPARENT);
    }
    vx_draw_button(s, LIST - 36, 10, 26, 24, "+", false);
    for (int r = 0; r * ITEM < h - TOP && list_top + r < shown_count; r++) {
        int i = shown[list_top + r];
        int y = TOP + r * ITEM;
        if (i == chosen) {
            vx_draw_selection(s, 6, y + 2, LIST - 12, ITEM - 4);
        }
        const struct vx_font *bold = vx_font(VX_FACE_BOLD, 13);
        char title[80];
        size_t n = vx_text_fit_bytes(bold, notes[i].title, LIST - 36);
        snprintf(title, sizeof(title), "%.*s", (int)n, notes[i].title);
        vx_text(s, bold, 16, y + 7, title, VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_text_fit(s, 16, y + 26, LIST - 36, notes[i].preview[0] ? notes[i].preview : "No more text",
                         VX_COLOR_DIM, VX_TRANSPARENT);
        vx_fill(s, 16, y + ITEM - 1, LIST - 32, 1, VX_COLOR_LINE);
    }
    if (!shown_count) {
        vx_draw_text(s, 16, TOP + 10, search[0] ? "No notes match" : "No notes yet", VX_COLOR_DIM,
                     VX_TRANSPARENT);
    }
    /* The note. */
    if (chosen < 0) {
        const char *hint = "Ctrl+N or + makes a note";
        vx_draw_text(s, LIST + (w - LIST - vx_text_width(hint)) / 2, h / 2, hint, VX_COLOR_DIM,
                     VX_TRANSPARENT);
        vx_window_present(window, 0, 0, w, h);
        return;
    }
    wrap(editor_width());
    int cursor_line = line_of(cursor);
    if (cursor_line < scroll) {
        scroll = cursor_line;
    } else if (cursor_line >= scroll + editor_rows()) {
        scroll = cursor_line - editor_rows() + 1;
    }
    char when[64] = "";
    if (notes[chosen].modified) {
        struct vx_date d;
        vx_date_of(notes[chosen].modified, &d);
        snprintf(when, sizeof(when), "%d %s %d at %02d:%02d", d.day, vx_month_names[d.month - 1],
                 d.year, d.hour, d.minute);
    }
    vx_draw_text(s, LIST + (w - LIST - vx_text_width(when)) / 2, 14, when, VX_COLOR_DIM,
                 VX_TRANSPARENT);
    const struct vx_font *f = body_font();
    size_t a = has_selection() ? (anchor < cursor ? anchor : cursor) : 0;
    size_t b = has_selection() ? (anchor < cursor ? cursor : anchor) : 0;
    for (int r = 0; r < editor_rows() && scroll + r < line_count; r++) {
        int line = scroll + r;
        size_t start = line_start[line];
        size_t end = line + 1 < line_count ? line_start[line + 1] : length;
        size_t shown_end = end;
        while (shown_end > start && (text[shown_end - 1] == '\n')) {
            shown_end--;
        }
        int y = TOP + r * LINE_HEIGHT;
        if (has_selection() && b > start && a < end) {
            size_t sa = a > start ? a : start, sb = b < shown_end ? b : shown_end;
            int x0 = vx_text_width_bytes(f, text + start, sa - start);
            int x1 = vx_text_width_bytes(f, text + start, sb - start) + (b > shown_end ? 6 : 0);
            vx_fill(s, editor_x() + x0, y, x1 - x0, LINE_HEIGHT, VX_COLOR_SELECTED);
        }
        char piece[2048];
        size_t n = shown_end - start < sizeof(piece) - 1 ? shown_end - start : sizeof(piece) - 1;
        memcpy(piece, text + start, n);
        piece[n] = '\0';
        /* The first line is the title: bigger. */
        vx_text(s, line == 0 ? vx_font(VX_FACE_BOLD, 14) : f, editor_x(),
                y + (LINE_HEIGHT - vx_font_height(f)) / 2, piece, VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    vx_fill(s, editor_x() + x_of(cursor), TOP + (cursor_line - scroll) * LINE_HEIGHT + 1, 2,
            LINE_HEIGHT - 2, VX_COLOR_ACCENT);
    vx_window_present(window, 0, 0, w, h);
}

/* ---- Events ---- */

static void move_cursor(size_t to, bool extend) {
    if (extend) {
        if (anchor == (size_t)-1) {
            anchor = cursor;
        }
    } else {
        anchor = (size_t)-1;
    }
    cursor = to > length ? length : to;
}

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
        return;
    }
    if (e->key == VX_KEY_LEFTSHIFT || e->key == VX_KEY_RIGHTSHIFT) {
        shift = e->value != 0;
        return;
    }
    if (!e->value) {
        return;
    }
    if (ctrl) {
        switch (e->key) {
        case 49: new_note(); return;    /* N */
        case 33: searching = true; return; /* F */
        case VX_KEY_DELETE: delete_note(); return;
        case 30: anchor = 0, cursor = length; return; /* A */
        case 46:                                     /* C */
        case 45:                                     /* X */
            if (has_selection()) {
                size_t a = anchor < cursor ? anchor : cursor, b = anchor < cursor ? cursor : anchor;
                vx_clipboard_set(text + a, b - a);
                if (e->key == 45) {
                    delete_selection();
                    dirty = true;
                    changed_ms = vx_uptime();
                }
            }
            return;
        case 47: { /* V */
            char *clip = vx_clipboard_get();
            if (clip) {
                insert(clip, strlen(clip));
                free(clip);
            }
            return;
        }
        }
        return;
    }
    if (searching) {
        if (e->key == VX_KEY_ESC || e->key == VX_KEY_ENTER) {
            searching = false;
            if (e->key == VX_KEY_ESC) {
                search[0] = '\0';
                filter();
            }
            return;
        }
        if (vx_field_key(search, sizeof(search), e)) {
            list_top = 0;
            filter();
        }
        return;
    }
    if (chosen < 0 && e->character < ' ') {
        return;
    }
    switch (e->key) {
    case VX_KEY_LEFT:
        move_cursor(has_selection() && !shift ? (anchor < cursor ? anchor : cursor)
                                              : vx_utf8_previous(text, cursor), shift);
        return;
    case VX_KEY_RIGHT: {
        size_t to = cursor;
        if (has_selection() && !shift) {
            to = anchor > cursor ? anchor : cursor;
        } else if (cursor < length) {
            const char *p = text + cursor;
            vx_utf8_next(&p);
            to = (size_t)(p - text);
        }
        move_cursor(to, shift);
        return;
    }
    case VX_KEY_UP:
    case VX_KEY_DOWN: {
        int line = line_of(cursor) + (e->key == VX_KEY_UP ? -1 : 1);
        if (line < 0 || line >= line_count) {
            move_cursor(line < 0 ? 0 : length, shift);
        } else {
            move_cursor(byte_at(line, x_of(cursor)), shift);
        }
        return;
    }
    case VX_KEY_HOME: move_cursor(line_start[line_of(cursor)], shift); return;
    case VX_KEY_END: {
        int line = line_of(cursor);
        size_t end = line + 1 < line_count ? line_start[line + 1] - 1 : length;
        move_cursor(end, shift);
        return;
    }
    case VX_KEY_BACKSPACE:
        if (has_selection()) {
            delete_selection();
        } else if (cursor > 0) {
            size_t from = vx_utf8_previous(text, cursor);
            memmove(text + from, text + cursor, length - cursor + 1);
            length -= cursor - from;
            cursor = from;
        }
        dirty = true;
        changed_ms = vx_uptime();
        return;
    case VX_KEY_DELETE:
        if (has_selection()) {
            delete_selection();
        } else if (cursor < length) {
            const char *p = text + cursor;
            vx_utf8_next(&p);
            size_t to = (size_t)(p - text);
            memmove(text + cursor, text + to, length - to + 1);
            length -= to - cursor;
        }
        dirty = true;
        changed_ms = vx_uptime();
        return;
    case VX_KEY_ENTER: insert("\n", 1); return;
    case VX_KEY_TAB: insert("    ", 4); return;
    }
    if (e->character >= ' ' && e->character != 127) {
        char bytes[4];
        insert(bytes, (size_t)vx_utf8_encode((uint32_t)e->character, bytes));
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    vx_window_set_cursor(window, e->x > LIST || vx_inside(e->x, e->y, 10, 10, LIST - 52, 24)
                                     ? VX_CURSOR_TEXT : VX_CURSOR_ARROW);
    if (e->wheel) {
        if (e->x < LIST) {
            list_top -= e->wheel;
            int max = shown_count - (window->surface.height - TOP) / ITEM;
            list_top = list_top > max ? max : list_top;
            list_top = list_top < 0 ? 0 : list_top;
        } else {
            scroll -= e->wheel * 3;
            scroll = scroll < 0 ? 0 : scroll >= line_count ? line_count - 1 : scroll;
        }
    }
    static bool selecting;
    if (click) {
        searching = vx_inside(e->x, e->y, 10, 10, LIST - 52, 24);
        if (vx_inside(e->x, e->y, LIST - 36, 10, 26, 24)) {
            new_note();
            return;
        }
        if (e->x < LIST && e->y >= TOP) {
            int r = list_top + (e->y - TOP) / ITEM;
            if (r < shown_count) {
                choose(shown[r]);
            }
            return;
        }
    }
    if (chosen >= 0 && e->x >= LIST && (click || (selecting && (e->buttons & 1)))) {
        int line = scroll + (e->y - TOP) / LINE_HEIGHT;
        line = line < 0 ? 0 : line >= line_count ? line_count - 1 : line;
        move_cursor(byte_at(line, e->x - editor_x()), !click || shift);
        selecting = true;
    }
    if (!(e->buttons & 1)) {
        selecting = false;
    }
}

int main(void) {
    read_notes();
    window = vx_window_create_flags("Notes", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "notes: no desktop to open a window on\n");
        return 1;
    }
    choose(shown_count ? shown[0] : -1);
    int held = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, dirty ? 800 : -1);
        if (got < 0) {
            save_note();
            return 0;
        }
        if (got == 0) {
            if (dirty && vx_uptime() - changed_ms >= 700) {
                save_note(); /* A moment after typing stops. */
            }
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            save_note();
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = shift = false;
                save_note();
            }
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 520 && e.height >= 300) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
