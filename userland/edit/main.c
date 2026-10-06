/* edit: the Text Editor. `edit [file...]` opens files (or a new one), each in
 * a tab.
 *
 *     typing, Tab, Enter, Backspace, Delete    edit
 *     arrows, Home/End, Page Up/Down, a click  move (with Shift, or dragging: select)
 *     Ctrl+Z, Ctrl+Y (Ctrl+Shift+Z)            undo, redo
 *     Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+A           cut, copy, paste, select all
 *     Ctrl+F, Ctrl+H, F3                       find, replace, find the next
 *     Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S     new, open, save, save as
 *     Ctrl+W, Ctrl+Tab, Ctrl+Q                 close the tab, the next tab, quit
 *
 * C, Python, shell, Markdown and settings files are coloured.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

#define WIDTH 720
#define HEIGHT 480
#define GUTTER 5 /* Cells for line numbers. */
#define MARGIN 6
#define TABS 26  /* The tab bar. */
#define STATUS 24
#define FIND_BAR 34
#define TAB 4
#define MAX_DOCS 12
#define MAX_UNDO 200

struct line {
    char *text;
    int length, capacity;
};

enum language { PLAIN, C_LIKE, PYTHON, SHELL, MARKDOWN, CONFIG };

struct snapshot {
    char *text;
    int row, col;
};

struct doc {
    struct line *lines;
    int count, capacity;
    int row, col, top, left;   /* The cursor (col: a byte); the first row and cell shown. */
    int anchor_row, anchor_col; /* The selection's other end. */
    bool selecting;
    char path[512];
    bool modified;
    enum language language;
    struct snapshot undo[MAX_UNDO], redo[MAX_UNDO];
    int undo_count, redo_count;
    long last_edit_ms;
    int last_edit_kind; /* Typing in a row merges into one undo. */
};

static struct vx_window *window;
static struct doc *docs[MAX_DOCS];
static int doc_count, current;
static char message[200];
static bool ctrl_held, shift_held;

/* The find bar. */
static bool find_open, replacing;
static int find_field; /* 0: find, 1: replace */
static char find_text[128], replace_text[128];

#define D (docs[current])

/* ---- Lines ---- */

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

static bool insert_line(struct doc *d, int at) {
    if (d->count == d->capacity) {
        int capacity = d->capacity ? d->capacity * 2 : 64;
        struct line *more = realloc(d->lines, (size_t)capacity * sizeof(*more));
        if (!more) {
            return false;
        }
        d->lines = more;
        d->capacity = capacity;
    }
    memmove(d->lines + at + 1, d->lines + at, (size_t)(d->count - at) * sizeof(*d->lines));
    d->lines[at] = (struct line){NULL, 0, 0};
    d->count++;
    return reserve(&d->lines[at], 0) && ((d->lines[at].text[0] = '\0'), true);
}

static void remove_line(struct doc *d, int at) {
    free(d->lines[at].text);
    memmove(d->lines + at, d->lines + at + 1, (size_t)(d->count - at - 1) * sizeof(*d->lines));
    d->count--;
}

static void append(struct line *l, const char *text, int length) {
    if (reserve(l, l->length + length)) {
        memcpy(l->text + l->length, text, (size_t)length);
        l->length += length;
        l->text[l->length] = '\0';
    }
}

static void clear_doc(struct doc *d) {
    while (d->count) {
        remove_line(d, d->count - 1);
    }
}

/* The whole text, lines joined with newlines, in a new string. */
static char *whole_text(struct doc *d, size_t *size_out) {
    size_t size = 1;
    for (int i = 0; i < d->count; i++) {
        size += (size_t)d->lines[i].length + 1;
    }
    char *text = malloc(size);
    if (!text) {
        return NULL;
    }
    size_t n = 0;
    for (int i = 0; i < d->count; i++) {
        memcpy(text + n, d->lines[i].text, (size_t)d->lines[i].length);
        n += (size_t)d->lines[i].length;
        if (i + 1 < d->count) {
            text[n++] = '\n';
        }
    }
    text[n] = '\0';
    if (size_out) {
        *size_out = n;
    }
    return text;
}

static void set_text(struct doc *d, const char *text) {
    clear_doc(d);
    insert_line(d, 0);
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            insert_line(d, d->count);
        } else if (*p == '\t') {
            append(&d->lines[d->count - 1], "    ", TAB);
        } else if (*p != '\r') {
            append(&d->lines[d->count - 1], p, 1);
        }
    }
}

/* ---- UTF-8 positions ---- */

static int previous_char(const struct line *l, int at) {
    return (int)vx_utf8_previous(l->text, (size_t)at);
}

static int next_char(const struct line *l, int at) {
    const char *p = l->text + at;
    if (at < l->length) {
        vx_utf8_next(&p);
    }
    return (int)(p - l->text) > l->length ? l->length : (int)(p - l->text);
}

static int cell_of(const struct line *l, int at) {
    int cells = 0;
    for (int i = 0; i < at && i < l->length; i = next_char(l, i)) {
        cells++;
    }
    return cells;
}

static int byte_of(const struct line *l, int cell) {
    int i = 0;
    while (cell-- > 0 && i < l->length) {
        i = next_char(l, i);
    }
    return i;
}

/* ---- Files ---- */

static enum language language_of(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (!dot) {
        return !strcmp(name, "Makefile") ? SHELL : PLAIN;
    }
    dot++;
    static const char *const c_like[] = {"c", "h", "cpp", "cc", "hpp", "js", "java", "rs", "go",
                                         "ts", "css", "S", NULL};
    for (int i = 0; c_like[i]; i++) {
        if (!strcmp(dot, c_like[i])) {
            return C_LIKE;
        }
    }
    if (!strcmp(dot, "py")) {
        return PYTHON;
    }
    if (!strcmp(dot, "sh") || !strcmp(dot, "mk")) {
        return SHELL;
    }
    if (!strcmp(dot, "md")) {
        return MARKDOWN;
    }
    if (!strcmp(dot, "conf") || !strcmp(dot, "ini") || !strcmp(dot, "cfg") || !strcmp(dot, "toml")) {
        return CONFIG;
    }
    return PLAIN;
}

static void say(const char *text) {
    snprintf(message, sizeof(message), "%s", text);
}

static bool load(struct doc *d, const char *file) {
    int handle = vx_open(file, VX_OPEN_READ);
    if (handle < 0) {
        insert_line(d, 0);
        say("New file");
        return false;
    }
    size_t size = 0, capacity = 8192;
    char *text = malloc(capacity);
    long n;
    while (text && (n = vx_read(handle, text + size, capacity - size - 1)) > 0) {
        size += (size_t)n;
        if (capacity - size < 4096) {
            char *more = realloc(text, capacity * 2);
            if (!more) {
                break;
            }
            text = more;
            capacity *= 2;
        }
    }
    vx_close(handle);
    if (!text) {
        insert_line(d, 0);
        return false;
    }
    text[size] = '\0';
    set_text(d, text);
    free(text);
    if (d->count > 1 && d->lines[d->count - 1].length == 0) {
        remove_line(d, d->count - 1); /* The newline at the end. */
    }
    char note[160];
    snprintf(note, sizeof(note), "%d lines", d->count);
    say(note);
    return true;
}

static bool save(struct doc *d, const char *file) {
    int handle = vx_open(file, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    char note[600];
    if (handle < 0) {
        snprintf(note, sizeof(note), "Can't save %s: %s", file, vx_strerror(handle));
        say(note);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < d->count && ok; i++) {
        ok = vx_write(handle, d->lines[i].text, (size_t)d->lines[i].length) == d->lines[i].length &&
             vx_write(handle, "\n", 1) == 1;
    }
    vx_close(handle);
    if (!ok) {
        snprintf(note, sizeof(note), "Couldn't write all of %s", file);
        say(note);
        return false;
    }
    d->modified = false;
    if (file != d->path) {
        snprintf(d->path, sizeof(d->path), "%s", file);
    }
    d->language = language_of(d->path);
    snprintf(note, sizeof(note), "Saved %s", file);
    say(note);
    printf("edit: saved %s\n", file);
    fflush(stdout);
    return true;
}

static const char *doc_name(const struct doc *d) {
    return d->path[0] ? strrchr(d->path, '/') ? strrchr(d->path, '/') + 1 : d->path : "Untitled";
}

static void update_title(void) {
    static char shown[600];
    char title[600];
    snprintf(title, sizeof(title), "%s%s - Text Editor", D->modified ? "*" : "", doc_name(D));
    if (strcmp(title, shown)) {
        strcpy(shown, title);
        vx_window_set_title(window, title);
    }
}

static bool new_doc(const char *path) {
    if (doc_count == MAX_DOCS) {
        say("Too many tabs");
        return false;
    }
    struct doc *d = calloc(1, sizeof(*d));
    if (!d) {
        return false;
    }
    if (path && path[0]) {
        snprintf(d->path, sizeof(d->path), "%s", path);
        load(d, path);
        d->language = language_of(path);
    } else {
        insert_line(d, 0);
        say("New file");
    }
    d->anchor_row = -1;
    docs[doc_count++] = d;
    current = doc_count - 1;
    return true;
}

static void free_snapshots(struct snapshot *s, int *count) {
    for (int i = 0; i < *count; i++) {
        free(s[i].text);
    }
    *count = 0;
}

static void close_doc(int i) {
    struct doc *d = docs[i];
    clear_doc(d);
    free(d->lines);
    free_snapshots(d->undo, &d->undo_count);
    free_snapshots(d->redo, &d->redo_count);
    free(d);
    memmove(docs + i, docs + i + 1, sizeof(docs[0]) * (size_t)(doc_count - i - 1));
    doc_count--;
    if (current >= doc_count) {
        current = doc_count - 1;
    }
}

/* ---- Undo ---- */

static void push(struct snapshot *stack, int *count, struct doc *d) {
    if (*count == MAX_UNDO) {
        free(stack[0].text);
        memmove(stack, stack + 1, sizeof(stack[0]) * (MAX_UNDO - 1));
        (*count)--;
    }
    char *text = whole_text(d, NULL);
    if (!text) {
        return;
    }
    stack[(*count)++] = (struct snapshot){text, d->row, d->col};
}

/* Before a change: what to go back to. Typing (kind 1) in a row is one
 * change; anything else starts a new one. */
static void before_change(int kind) {
    struct doc *d = D;
    long now = vx_uptime();
    if (!(kind == 1 && d->last_edit_kind == 1 && now - d->last_edit_ms < 1500)) {
        push(d->undo, &d->undo_count, d);
        free_snapshots(d->redo, &d->redo_count);
    }
    d->last_edit_kind = kind;
    d->last_edit_ms = now;
    d->modified = true;
}

static void restore(struct snapshot *from, int *from_count, struct snapshot *to, int *to_count) {
    struct doc *d = D;
    if (!*from_count) {
        say(from == d->undo ? "Nothing to undo" : "Nothing to redo");
        return;
    }
    push(to, to_count, d);
    struct snapshot s = from[--*from_count];
    set_text(d, s.text);
    free(s.text);
    d->row = s.row < d->count ? s.row : d->count - 1;
    d->col = s.col <= d->lines[d->row].length ? s.col : d->lines[d->row].length;
    d->anchor_row = -1;
    d->modified = true;
    d->last_edit_kind = 0;
}

/* ---- Selection ---- */

static bool has_selection(void) {
    return D->anchor_row >= 0 && (D->anchor_row != D->row || D->anchor_col != D->col);
}

/* The selection's start and end, in order. */
static void selection(int *r0, int *c0, int *r1, int *c1) {
    struct doc *d = D;
    bool anchor_first = d->anchor_row < d->row || (d->anchor_row == d->row && d->anchor_col < d->col);
    *r0 = anchor_first ? d->anchor_row : d->row;
    *c0 = anchor_first ? d->anchor_col : d->col;
    *r1 = anchor_first ? d->row : d->anchor_row;
    *c1 = anchor_first ? d->col : d->anchor_col;
}

static char *selected_text(size_t *length) {
    int r0, c0, r1, c1;
    selection(&r0, &c0, &r1, &c1);
    size_t size = 1;
    for (int r = r0; r <= r1; r++) {
        size += (size_t)D->lines[r].length + 1;
    }
    char *text = malloc(size);
    if (!text) {
        return NULL;
    }
    size_t n = 0;
    for (int r = r0; r <= r1; r++) {
        struct line *l = &D->lines[r];
        int from = r == r0 ? c0 : 0, to = r == r1 ? c1 : l->length;
        memcpy(text + n, l->text + from, (size_t)(to - from));
        n += (size_t)(to - from);
        if (r != r1) {
            text[n++] = '\n';
        }
    }
    text[n] = '\0';
    *length = n;
    return text;
}

static void delete_selection(void) {
    int r0, c0, r1, c1;
    selection(&r0, &c0, &r1, &c1);
    struct doc *d = D;
    struct line *first = &d->lines[r0];
    struct line *last = &d->lines[r1];
    char *tail = strdup(last->text + c1);
    first->length = c0;
    first->text[c0] = '\0';
    if (tail) {
        append(first, tail, (int)strlen(tail));
        free(tail);
    }
    for (int r = r1; r > r0; r--) {
        remove_line(d, r);
    }
    d->row = r0;
    d->col = c0;
    d->anchor_row = -1;
}

/* ---- Editing ---- */

static void insert_text(const char *text) {
    struct doc *d = D;
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n');
        int n = end ? (int)(end - p) : (int)strlen(p);
        struct line *l = &d->lines[d->row];
        if (reserve(l, l->length + n)) {
            memmove(l->text + d->col + n, l->text + d->col, (size_t)(l->length - d->col + 1));
            memcpy(l->text + d->col, p, (size_t)n);
            l->length += n;
            d->col += n;
        }
        if (!end) {
            break;
        }
        /* A new line: the rest of this one goes onto it. */
        if (insert_line(d, d->row + 1)) {
            struct line *cur = &d->lines[d->row];
            append(&d->lines[d->row + 1], cur->text + d->col, cur->length - d->col);
            cur->length = d->col;
            cur->text[d->col] = '\0';
            d->row++;
            d->col = 0;
        }
        p = end + 1;
    }
}

static void type_text(const char *text, int kind) {
    before_change(kind);
    if (has_selection()) {
        delete_selection();
    }
    D->anchor_row = -1;
    insert_text(text);
}

static void new_line(void) {
    /* Keeps the line's indentation. */
    struct line *l = &D->lines[D->row];
    char text[128] = "\n";
    int n = 0;
    while (n < l->length && n < 120 && l->text[n] == ' ') {
        n++;
    }
    memset(text + 1, ' ', (size_t)n);
    text[n + 1] = '\0';
    type_text(text, 2);
}

static void backspace(void) {
    struct doc *d = D;
    before_change(3);
    if (has_selection()) {
        delete_selection();
        return;
    }
    d->anchor_row = -1;
    if (d->col > 0) {
        struct line *l = &d->lines[d->row];
        int start = previous_char(l, d->col);
        memmove(l->text + start, l->text + d->col, (size_t)(l->length - d->col + 1));
        l->length -= d->col - start;
        d->col = start;
    } else if (d->row > 0) {
        d->col = d->lines[d->row - 1].length;
        append(&d->lines[d->row - 1], d->lines[d->row].text, d->lines[d->row].length);
        remove_line(d, d->row);
        d->row--;
    }
}

static void delete_forward(void) {
    struct doc *d = D;
    if (has_selection()) {
        before_change(3);
        delete_selection();
        return;
    }
    if (d->col < d->lines[d->row].length) {
        d->col = next_char(&d->lines[d->row], d->col);
        backspace();
    } else if (d->row + 1 < d->count) {
        before_change(3);
        append(&d->lines[d->row], d->lines[d->row + 1].text, d->lines[d->row + 1].length);
        remove_line(d, d->row + 1);
    }
}

static void copy(bool cut) {
    if (!has_selection()) {
        return;
    }
    size_t n;
    char *text = selected_text(&n);
    if (text) {
        vx_clipboard_set(text, n);
        free(text);
    }
    if (cut) {
        before_change(4);
        delete_selection();
    }
}

static void paste(void) {
    char *text = vx_clipboard_get();
    if (text && text[0]) {
        /* Tabs become spaces, as when a file is read. */
        size_t n = strlen(text), tabs = 0;
        for (size_t i = 0; i < n; i++) {
            tabs += text[i] == '\t';
        }
        char *clean = malloc(n + tabs * TAB + 1);
        if (clean) {
            size_t k = 0;
            for (size_t i = 0; i < n; i++) {
                if (text[i] == '\t') {
                    memset(clean + k, ' ', TAB);
                    k += TAB;
                } else if (text[i] != '\r') {
                    clean[k++] = text[i];
                }
            }
            clean[k] = '\0';
            type_text(clean, 4);
            free(clean);
        }
    }
    free(text);
}

/* ---- Find and replace ---- */

static bool match_at(const char *text, const char *what) {
    for (; *what; what++, text++) {
        char a = *text, b = *what;
        if (a >= 'A' && a <= 'Z') {
            a += 32;
        }
        if (b >= 'A' && b <= 'Z') {
            b += 32;
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* The next match after the cursor (or before, going back), wrapping round;
 * it's selected. */
static bool find_next(bool backwards) {
    struct doc *d = D;
    int n = (int)strlen(find_text);
    if (!n) {
        return false;
    }
    int start_row = d->row, start_col = has_selection() && !backwards ? d->col : d->col;
    if (backwards && has_selection()) {
        int r0, c0, r1, c1;
        selection(&r0, &c0, &r1, &c1);
        start_row = r0, start_col = c0;
    }
    for (int step = 0; step <= d->count; step++) {
        int r = backwards ? (start_row - step + d->count * 2) % d->count
                          : (start_row + step) % d->count;
        struct line *l = &d->lines[r];
        if (backwards) {
            int from = step == 0 ? start_col - 1 : l->length - n;
            for (int c = from; c >= 0; c--) {
                if (c + n <= l->length && match_at(l->text + c, find_text)) {
                    d->anchor_row = r, d->anchor_col = c;
                    d->row = r, d->col = c + n;
                    return true;
                }
            }
        } else {
            for (int c = step == 0 ? start_col : 0; c + n <= l->length; c++) {
                if (match_at(l->text + c, find_text)) {
                    d->anchor_row = r, d->anchor_col = c;
                    d->row = r, d->col = c + n;
                    return true;
                }
            }
        }
    }
    say("Not found");
    return false;
}

static bool selection_matches(void) {
    if (!has_selection()) {
        return false;
    }
    size_t n;
    char *text = selected_text(&n);
    bool same = text && n == strlen(find_text) && match_at(text, find_text);
    free(text);
    return same;
}

static void replace_one(void) {
    if (!selection_matches() && !find_next(false)) {
        return;
    }
    type_text(replace_text, 5);
    find_next(false);
}

static void replace_all(void) {
    struct doc *d = D;
    int n = (int)strlen(find_text), count = 0;
    if (!n) {
        return;
    }
    before_change(5);
    for (int r = 0; r < d->count; r++) {
        for (int c = 0; c + n <= d->lines[r].length;) {
            if (match_at(d->lines[r].text + c, find_text)) {
                d->row = r, d->col = c;
                d->anchor_row = r, d->anchor_col = c + n;
                delete_selection();
                insert_text(replace_text);
                c = d->col;
                count++;
            } else {
                c++;
            }
        }
    }
    char note[64];
    snprintf(note, sizeof(note), "Replaced %d", count);
    say(note);
    printf("edit: replaced %d\n", count);
    fflush(stdout);
}

/* ---- Colouring ---- */

enum style { S_TEXT, S_KEYWORD, S_TYPE, S_STRING, S_COMMENT, S_NUMBER, S_DIRECTIVE, S_HEADING };

static uint32_t style_color(enum style s) {
    static const uint32_t dark[] = {0, 0xc792ea, 0x82aaff, 0xc3e88d, 0x7f848e, 0xf78c6c, 0xffcb6b, 0x89ddff};
    static const uint32_t light[] = {0, 0x7c3aed, 0x2563eb, 0x15803d, 0x8a8a8a, 0xc2410c, 0xb45309, 0x0369a1};
    return s == S_TEXT ? VX_COLOR_TEXT : vx_theme.dark ? dark[s] : light[s];
}

static bool word_in(const char *word, int n, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if ((int)strlen(list[i]) == n && !strncmp(list[i], word, (size_t)n)) {
            return true;
        }
    }
    return false;
}

static const char *const c_keywords[] = {
    "if", "else", "for", "while", "do", "return", "switch", "case", "default", "break",
    "continue", "goto", "struct", "union", "enum", "typedef", "static", "const", "extern",
    "sizeof", "volatile", "inline", "register", "function", "var", "let", "new", "class",
    "public", "private", "fn", "impl", "use", "pub", "mut", "true", "false", "NULL", "null",
    "import", "package", "func", "this", NULL,
};
static const char *const c_types[] = {
    "int", "char", "void", "long", "short", "unsigned", "signed", "float", "double", "bool",
    "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t",
    "int64_t", "string", "String", "auto", NULL,
};
static const char *const py_keywords[] = {
    "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from", "as",
    "in", "not", "and", "or", "is", "None", "True", "False", "with", "try", "except",
    "finally", "raise", "pass", "break", "continue", "lambda", "yield", "global", "async",
    "await", "self", NULL,
};
static const char *const sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac", "in",
    "function", "return", "export", "local", "echo", "exit", "set", "unset", NULL,
};

/* Styles for a line's bytes; `in_comment` carries a C block comment over. */
static void style_line(const struct doc *d, const struct line *l, uint8_t *styles,
                       bool *in_comment) {
    memset(styles, S_TEXT, (size_t)l->length);
    const char *t = l->text;
    int n = l->length;
    switch (d->language) {
    case PLAIN: return;
    case MARKDOWN:
        if (n && t[0] == '#') {
            memset(styles, S_HEADING, (size_t)n);
        } else {
            for (int i = 0; i < n; i++) {
                if (t[i] == '`') {
                    int j = i + 1;
                    while (j < n && t[j] != '`') {
                        j++;
                    }
                    memset(styles + i, S_STRING, (size_t)((j < n ? j + 1 : n) - i));
                    i = j;
                }
            }
        }
        return;
    case CONFIG: {
        int i = 0;
        while (i < n && t[i] == ' ') {
            i++;
        }
        if (i < n && (t[i] == '#' || t[i] == ';')) {
            memset(styles + i, S_COMMENT, (size_t)(n - i));
        } else if (i < n && t[i] == '[') {
            memset(styles + i, S_HEADING, (size_t)(n - i));
        } else {
            const char *eq = memchr(t, '=', (size_t)n);
            if (eq) {
                memset(styles, S_KEYWORD, (size_t)(eq - t));
                memset(styles + (eq - t) + 1, S_STRING, (size_t)(n - (eq - t) - 1));
            }
        }
        return;
    }
    default: break;
    }
    bool c = d->language == C_LIKE;
    char line_comment = c ? '/' : '#';
    for (int i = 0; i < n;) {
        if (c && *in_comment) {
            int start = i;
            while (i < n && !(t[i] == '*' && i + 1 < n && t[i + 1] == '/')) {
                i++;
            }
            if (i < n) {
                i += 2;
                *in_comment = false;
            }
            memset(styles + start, S_COMMENT, (size_t)(i - start));
            continue;
        }
        char ch = t[i];
        if (c && ch == '/' && i + 1 < n && t[i + 1] == '*') {
            *in_comment = true;
            styles[i] = styles[i + 1] = S_COMMENT;
            i += 2;
            continue;
        }
        if ((c && ch == '/' && i + 1 < n && t[i + 1] == '/') || (!c && ch == line_comment)) {
            memset(styles + i, S_COMMENT, (size_t)(n - i));
            return;
        }
        if (c && ch == '#' && (i == 0 || t[i - 1] == ' ')) {
            int start = i;
            while (i < n && t[i] != ' ' && t[i] != '<' && t[i] != '"') {
                i++;
            }
            memset(styles + start, S_DIRECTIVE, (size_t)(i - start));
            continue;
        }
        if (ch == '"' || ch == '\'') {
            int start = i++;
            while (i < n && t[i] != ch) {
                i += t[i] == '\\' ? 2 : 1;
            }
            i = i < n ? i + 1 : n;
            memset(styles + start, S_STRING, (size_t)(i - start));
            continue;
        }
        if (ch >= '0' && ch <= '9' && (i == 0 || !(t[i - 1] == '_' || (t[i - 1] | 0x20) >= 'a'))) {
            int start = i;
            while (i < n && ((t[i] | 0x20) >= 'a' ? (t[i] | 0x20) <= 'z' : (t[i] >= '0' && t[i] <= '9') || t[i] == '.')) {
                i++;
            }
            memset(styles + start, S_NUMBER, (size_t)(i - start));
            continue;
        }
        if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') {
            int start = i;
            while (i < n && (((t[i] | 0x20) >= 'a' && (t[i] | 0x20) <= 'z') || t[i] == '_' ||
                             (t[i] >= '0' && t[i] <= '9'))) {
                i++;
            }
            const char *const *keywords = c ? c_keywords : d->language == PYTHON ? py_keywords
                                                                                   : sh_keywords;
            if (word_in(t + start, i - start, keywords)) {
                memset(styles + start, S_KEYWORD, (size_t)(i - start));
            } else if (c && word_in(t + start, i - start, c_types)) {
                memset(styles + start, S_TYPE, (size_t)(i - start));
            }
            continue;
        }
        if (!c && ch == '$') {
            int start = i++;
            while (i < n && (((t[i] | 0x20) >= 'a' && (t[i] | 0x20) <= 'z') || t[i] == '_' ||
                             t[i] == '{' || t[i] == '}' || (t[i] >= '0' && t[i] <= '9'))) {
                i++;
            }
            memset(styles + start, S_TYPE, (size_t)(i - start));
            continue;
        }
        i++;
    }
}

/* ---- Drawing ---- */

static int text_top(void) {
    return TABS;
}

static int text_bottom(void) {
    return window->surface.height - STATUS - (find_open ? FIND_BAR : 0);
}

static int text_rows(void) {
    return (text_bottom() - text_top() - MARGIN) / VX_LINE_HEIGHT;
}

static int text_columns(void) {
    return (window->surface.width - 2 * MARGIN) / VX_CELL_WIDTH - GUTTER - 1;
}

static void keep_cursor_visible(void) {
    struct doc *d = D;
    int rows = text_rows(), columns = text_columns();
    if (d->row < d->top) {
        d->top = d->row;
    } else if (d->row >= d->top + rows) {
        d->top = d->row - rows + 1;
    }
    int cell = cell_of(&d->lines[d->row], d->col);
    if (cell < d->left) {
        d->left = cell;
    } else if (cell >= d->left + columns) {
        d->left = cell - columns + 1;
    }
}

static int tab_width(void) {
    int w = (window->surface.width - 40) / (doc_count ? doc_count : 1);
    return w > 180 ? 180 : w;
}

static void draw_tabs(struct vx_surface *s) {
    vx_draw_toolbar(s, 0, 0, s->width, TABS);
    int w = tab_width();
    for (int i = 0; i < doc_count; i++) {
        int x = 4 + i * w;
        char label[300];
        snprintf(label, sizeof(label), "%s%s", docs[i]->modified ? "*" : "", doc_name(docs[i]));
        vx_draw_tab(s, x, 3, w - 4, TABS - 6, label, i == current);
        vx_draw_text(s, x + w - 18, 5, "x", i == current ? 0xffffff : VX_COLOR_DIM, VX_TRANSPARENT);
    }
    vx_draw_text(s, 4 + doc_count * w + 6, 5, "+", VX_COLOR_TEXT, VX_TRANSPARENT);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    struct doc *d = D;
    int w = s->width, h = s->height, rows = text_rows(), columns = text_columns();
    keep_cursor_visible();
    vx_fill(s, 0, 0, w, h, VX_COLOR_VIEW);
    draw_tabs(s);
    int text_x = MARGIN + (GUTTER + 1) * VX_CELL_WIDTH;
    /* Block comments carry over from lines above what's shown. */
    bool in_comment = false;
    static uint8_t *styles;
    static int styles_size;
    for (int r = 0; r < d->top && d->language == C_LIKE; r++) {
        if (d->lines[r].length + 1 > styles_size) {
            styles_size = d->lines[r].length + 256;
            styles = realloc(styles, (size_t)styles_size);
        }
        if (styles) {
            style_line(d, &d->lines[r], styles, &in_comment);
        }
    }
    int r0 = -1, c0 = 0, r1 = -1, c1 = 0;
    if (has_selection()) {
        selection(&r0, &c0, &r1, &c1);
    }
    for (int i = 0; i < rows && d->top + i < d->count; i++) {
        int r = d->top + i;
        int y = text_top() + MARGIN / 2 + i * VX_LINE_HEIGHT;
        char number[16];
        snprintf(number, sizeof(number), "%*d", GUTTER - 1, r + 1);
        for (int k = 0; number[k]; k++) {
            vx_draw_char(s, MARGIN + k * VX_CELL_WIDTH, y, (unsigned char)number[k],
                         r == d->row ? VX_COLOR_ACCENT : VX_COLOR_DIM, VX_TRANSPARENT);
        }
        struct line *l = &d->lines[r];
        if (l->length + 1 > styles_size) {
            styles_size = l->length + 256;
            styles = realloc(styles, (size_t)styles_size);
        }
        if (styles) {
            style_line(d, l, styles, &in_comment);
        }
        int byte = byte_of(l, d->left);
        const char *p = l->text + byte;
        for (int c = 0; c < columns + 1; c++) {
            int at_byte = (int)(p - l->text);
            bool sel = r0 >= 0 && r >= r0 && r <= r1 && !(r == r0 && at_byte < c0) &&
                       !(r == r1 && at_byte >= c1) && (at_byte < l->length || r < r1);
            int x = text_x + c * VX_CELL_WIDTH;
            if (sel) {
                vx_fill(s, x, y, VX_CELL_WIDTH, VX_LINE_HEIGHT, VX_COLOR_SELECTED);
            }
            if (!*p) {
                break;
            }
            uint32_t ch = vx_utf8_next(&p);
            uint32_t color = styles ? style_color((enum style)styles[at_byte]) : VX_COLOR_TEXT;
            vx_draw_char(s, x, y, ch, color, VX_TRANSPARENT);
        }
    }
    vx_fill(s, text_x + (cell_of(&d->lines[d->row], d->col) - d->left) * VX_CELL_WIDTH,
            text_top() + MARGIN / 2 + (d->row - d->top) * VX_LINE_HEIGHT, 2, VX_LINE_HEIGHT,
            VX_COLOR_ACCENT);
    /* The find bar. */
    if (find_open) {
        int y = h - STATUS - FIND_BAR;
        vx_draw_toolbar(s, 0, y, w, FIND_BAR);
        vx_fill(s, 0, y, w, 1, VX_COLOR_LINE);
        vx_draw_text(s, 8, y + 9, "Find", VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_field(s, 50, y + 5, 170, find_text, find_field == 0);
        vx_draw_button(s, 226, y + 5, 54, 24, "Next", false);
        vx_draw_text(s, 292, y + 9, "Replace", VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_field(s, 352, y + 5, 150, replace_text, find_field == 1);
        vx_draw_button(s, 508, y + 5, 70, 24, "Replace", false);
        vx_draw_button(s, 584, y + 5, 40, 24, "All", false);
    }
    /* The status line. */
    vx_draw_toolbar(s, 0, h - STATUS, w, STATUS);
    vx_fill(s, 0, h - STATUS, w, 1, VX_COLOR_LINE);
    static const char *const languages[] = {"Plain text", "C", "Python", "Shell", "Markdown",
                                            "Settings"};
    char where[96];
    snprintf(where, sizeof(where), "Ln %d, Col %d   %s", d->row + 1,
             cell_of(&d->lines[d->row], d->col) + 1, languages[d->language]);
    int x = w - MARGIN - vx_text_width(where);
    vx_draw_text_fit(s, MARGIN, h - STATUS + 4, x - 2 * MARGIN, message, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_draw_text(s, x, h - STATUS + 4, where, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_window_present(window, 0, 0, w, h);
    update_title();
}

/* ---- Commands ---- */

/* Asks where to save the document, and saves it there. True if it was saved. */
static bool save_doc_as(struct doc *d) {
    char path[512], folder[512];
    snprintf(folder, sizeof(folder), "%s", vx_home_folder("Documents"));
    if (d->path[0]) {
        snprintf(folder, sizeof(folder), "%s", d->path);
        char *slash = strrchr(folder, '/');
        if (slash) {
            *slash = slash == folder ? (folder[1] = '\0', '/') : '\0';
        }
    }
    if (vx_save_dialog("Save", folder, d->path[0] ? doc_name(d) : "Untitled.txt", path,
                       sizeof(path))) {
        return save(d, path);
    }
    return false;
}

static void save_as(void) {
    save_doc_as(D);
}

/* Saves the document (asking where, if it's never been saved). True if it was. */
static bool save_doc(struct doc *d) {
    return d->path[0] ? save(d, d->path) : save_doc_as(d);
}

static void save_current(void) {
    save_doc(D);
}

/* Before a document with changes is closed: shows it, and asks whether to save
 * them (Save, Don't Save, Cancel). False if it's to stay open: Cancel, or a
 * Save that didn't happen. */
static bool may_close(int i, const char *verb) {
    struct doc *d = docs[i];
    if (!d->modified) {
        return true;
    }
    current = i;
    draw();
    switch (vx_ask_save_changes(doc_name(d), verb)) {
    case VX_SAVE_YES: return save_doc(d);
    case VX_SAVE_NO: return true;
    default: return false;
    }
}

/* Before the editor quits: every document with changes, one after the other. */
static bool may_quit(void) {
    for (int i = 0; i < doc_count; i++) {
        if (!may_close(i, "close")) {
            return false;
        }
    }
    return true;
}

static void open_file(void) {
    char path[512];
    if (vx_open_dialog("Open", vx_home_folder("Documents"), path, sizeof(path))) {
        /* Into this tab if it's an empty new one. */
        if (!D->path[0] && !D->modified && D->count == 1 && !D->lines[0].length) {
            close_doc(current);
        }
        new_doc(path);
    }
}

/* ---- Events ---- */

static void move_to(int row, int col, bool extend) {
    struct doc *d = D;
    if (extend) {
        if (d->anchor_row < 0) {
            d->anchor_row = d->row;
            d->anchor_col = d->col;
        }
    } else {
        d->anchor_row = -1;
    }
    d->row = row < 0 ? 0 : row >= d->count ? d->count - 1 : row;
    struct line *l = &d->lines[d->row];
    d->col = col < 0 ? 0 : col > l->length ? l->length : col;
}

static void find_key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_ESC) {
        find_open = false;
        return;
    }
    if (e->key == VX_KEY_TAB) {
        find_field = !find_field;
        return;
    }
    if (e->key == VX_KEY_ENTER) {
        if (find_field == 0) {
            find_next(shift_held);
        } else {
            replace_one();
        }
        return;
    }
    vx_field_key(find_field ? replace_text : find_text, find_field ? sizeof(replace_text)
                                                                   : sizeof(find_text), e);
    if (find_field == 0 && e->character >= ' ') {
        /* Finding as it's typed: from where the match started. */
        struct doc *d = D;
        if (has_selection()) {
            int r0, c0, r1, c1;
            selection(&r0, &c0, &r1, &c1);
            d->row = r0, d->col = c0;
        }
        d->anchor_row = -1;
        find_next(false);
    }
}

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl_held = e->value != 0;
        return;
    }
    if (e->key == VX_KEY_LEFTSHIFT || e->key == VX_KEY_RIGHTSHIFT) {
        shift_held = e->value != 0;
        return;
    }
    if (e->value == 0) {
        return;
    }
    struct doc *d = D;
    if (ctrl_held) {
        switch (e->key) {
        case 31: shift_held ? save_as() : save_current(); return; /* S */
        case 16: /* Q */
            vx_window_destroy(window);
            exit(0);
        case 44: /* Z */
            if (shift_held) {
                restore(d->redo, &d->redo_count, d->undo, &d->undo_count);
            } else {
                restore(d->undo, &d->undo_count, d->redo, &d->redo_count);
            }
            return;
        case 21: restore(d->redo, &d->redo_count, d->undo, &d->undo_count); return; /* Y */
        case 45: copy(true); return;  /* X */
        case 46: copy(false); return; /* C */
        case 47: paste(); return;     /* V */
        case 30: /* A */
            d->anchor_row = 0, d->anchor_col = 0;
            d->row = d->count - 1, d->col = d->lines[d->row].length;
            return;
        case 33: /* F */
            find_open = true;
            find_field = 0;
            replacing = false;
            return;
        case 35: /* H */
            find_open = true;
            find_field = 1;
            replacing = true;
            return;
        case 49: /* N */
        case 20: new_doc(NULL); return; /* T */
        case 24: open_file(); return; /* O */
        case 17: /* W */
            if (!may_close(current, "close")) {
                return;
            }
            close_doc(current);
            if (!doc_count) {
                vx_window_destroy(window);
                exit(0);
            }
            return;
        case VX_KEY_TAB:
        case VX_KEY_PAGEDOWN: current = (current + 1) % doc_count; return;
        case VX_KEY_PAGEUP: current = (current + doc_count - 1) % doc_count; return;
        case VX_KEY_HOME: move_to(0, 0, shift_held); return;
        case VX_KEY_END: move_to(d->count - 1, d->lines[d->count - 1].length, shift_held); return;
        }
    }
    if (e->key == 61) { /* F3 */
        find_next(shift_held);
        return;
    }
    if (find_open && (find_field == 0 || find_field == 1) && e->key != VX_KEY_UP &&
        e->key != VX_KEY_DOWN && e->key != VX_KEY_PAGEUP && e->key != VX_KEY_PAGEDOWN) {
        find_key(e);
        return;
    }
    int rows = text_rows();
    int cell = cell_of(&d->lines[d->row], d->col); /* Up and down keep to the same cell. */
    switch (e->key) {
    case VX_KEY_UP: move_to(d->row - 1, byte_of(&d->lines[d->row > 0 ? d->row - 1 : 0], cell), shift_held); break;
    case VX_KEY_DOWN:
        move_to(d->row + 1, byte_of(&d->lines[d->row + 1 < d->count ? d->row + 1 : d->row], cell),
                shift_held);
        break;
    case VX_KEY_PAGEUP: {
        int r = d->row > rows ? d->row - rows : 0;
        move_to(r, byte_of(&d->lines[r], cell), shift_held);
        break;
    }
    case VX_KEY_PAGEDOWN: {
        int r = d->row + rows < d->count ? d->row + rows : d->count - 1;
        move_to(r, byte_of(&d->lines[r], cell), shift_held);
        break;
    }
    case VX_KEY_LEFT:
        if (has_selection() && !shift_held) {
            int r0, c0, r1, c1;
            selection(&r0, &c0, &r1, &c1);
            move_to(r0, c0, false);
        } else if (d->col > 0) {
            move_to(d->row, previous_char(&d->lines[d->row], d->col), shift_held);
        } else if (d->row > 0) {
            move_to(d->row - 1, d->lines[d->row - 1].length, shift_held);
        }
        break;
    case VX_KEY_RIGHT:
        if (has_selection() && !shift_held) {
            int r0, c0, r1, c1;
            selection(&r0, &c0, &r1, &c1);
            move_to(r1, c1, false);
        } else if (d->col < d->lines[d->row].length) {
            move_to(d->row, next_char(&d->lines[d->row], d->col), shift_held);
        } else if (d->row + 1 < d->count) {
            move_to(d->row + 1, 0, shift_held);
        }
        break;
    case VX_KEY_HOME: {
        /* The first non-space, then the start of the line. */
        int first = 0;
        while (first < d->lines[d->row].length && d->lines[d->row].text[first] == ' ') {
            first++;
        }
        move_to(d->row, d->col == first ? 0 : first, shift_held);
        break;
    }
    case VX_KEY_END: move_to(d->row, d->lines[d->row].length, shift_held); break;
    case VX_KEY_ENTER: new_line(); break;
    case VX_KEY_BACKSPACE: backspace(); break;
    case VX_KEY_DELETE: delete_forward(); break;
    case VX_KEY_TAB: {
        char spaces[TAB + 1] = "    ";
        spaces[TAB - cell % TAB] = '\0';
        type_text(spaces, 1);
        break;
    }
    case VX_KEY_ESC: d->anchor_row = -1; break;
    default:
        if (e->character >= ' ' && e->character != 127 && !ctrl_held) {
            char bytes[5] = "";
            bytes[vx_utf8_encode((uint32_t)e->character, bytes)] = '\0';
            type_text(bytes, 1);
        }
        return;
    }
}

static void click_text(int px, int py, bool extend) {
    struct doc *d = D;
    int r = d->top + (py - text_top() - MARGIN / 2) / VX_LINE_HEIGHT;
    int c = d->left + (px - MARGIN) / VX_CELL_WIDTH - GUTTER - 1;
    r = r < 0 ? 0 : r >= d->count ? d->count - 1 : r;
    move_to(r, byte_of(&d->lines[r], c < 0 ? 0 : c), extend);
}

static void pointer(const struct vx_gui_event *e, int *buttons) {
    bool click = (e->buttons & 1) && !(*buttons & 1);
    *buttons = e->buttons;
    bool in_text = e->y >= text_top() && e->y < text_bottom();
    vx_window_set_cursor(window, in_text ? VX_CURSOR_TEXT : VX_CURSOR_ARROW);
    struct doc *d = D;
    if (e->wheel) {
        d->top -= e->wheel * 3;
        int max = d->count - text_rows();
        d->top = d->top > max ? max : d->top;
        d->top = d->top < 0 ? 0 : d->top;
        if (d->row < d->top) {
            move_to(d->top, d->col, false);
        } else if (d->row >= d->top + text_rows()) {
            move_to(d->top + text_rows() - 1, d->col, false);
        }
    }
    if (click && e->y < TABS) {
        int w = tab_width(), i = (e->x - 4) / w;
        if (i >= 0 && i < doc_count) {
            if (e->x >= 4 + i * w + w - 22) {
                if (!may_close(i, "close")) {
                    return;
                }
                close_doc(i);
                if (!doc_count) {
                    vx_window_destroy(window);
                    exit(0);
                }
            } else {
                current = i;
            }
        } else if (e->x >= 4 + doc_count * w) {
            new_doc(NULL);
        }
        return;
    }
    if (click && find_open && e->y >= text_bottom() && e->y < window->surface.height - STATUS) {
        int y = text_bottom();
        if (vx_inside(e->x, e->y, 50, y + 5, 170, 24)) {
            find_field = 0;
        } else if (vx_inside(e->x, e->y, 352, y + 5, 150, 24)) {
            find_field = 1;
        } else if (vx_inside(e->x, e->y, 226, y + 5, 54, 24)) {
            find_next(false);
        } else if (vx_inside(e->x, e->y, 508, y + 5, 70, 24)) {
            replace_one();
        } else if (vx_inside(e->x, e->y, 584, y + 5, 40, 24)) {
            replace_all();
        }
        return;
    }
    if (click && in_text) {
        click_text(e->x, e->y, shift_held);
        d->selecting = true;
        if (find_open) {
            find_field = -1; /* Typing goes to the text. */
        }
        return;
    }
    if (d->selecting && (e->buttons & 1)) {
        click_text(e->x, e->y, true);
        if (e->y < text_top() && d->top > 0) {
            d->top--;
        } else if (e->y >= text_bottom() && d->top + text_rows() < d->count) {
            d->top++;
        }
    }
    if (!(e->buttons & 1)) {
        d->selecting = false;
    }
}

int main(int argc, char **argv) {
    window = vx_window_create_flags("Text Editor", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "edit: no desktop to open a window on\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        new_doc(argv[i]);
    }
    if (!doc_count) {
        new_doc(NULL);
    }
    current = 0;
    int buttons = 0;
    for (;;) {
        draw();
        /* The desktop warns before logging out or shutting down with changes unsaved. */
        bool any = false;
        for (int i = 0; i < doc_count; i++) {
            any = any || docs[i]->modified;
        }
        vx_window_set_modified(window, any);
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            return 0;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            if (may_quit()) {
                vx_window_destroy(window);
                return 0;
            }
            break;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &buttons); break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl_held = shift_held = false;
            }
            break;
        case VX_GUI_DROP: {
            char *paths = vx_drop_paths(&e);
            vx_remove(e.text);
            for (char *line = paths, *next; line && *line; line = next) {
                next = strchr(line, '\n');
                if (next) {
                    *next++ = '\0';
                }
                if (line[0] == '/') {
                    new_doc(line);
                }
            }
            free(paths);
            break;
        }
        case VX_GUI_RESIZE:
            if (e.width >= 300 && e.height >= 160) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
