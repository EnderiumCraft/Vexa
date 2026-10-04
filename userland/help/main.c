/* help: Help. Vexa's user guide (/share/help/USER-GUIDE.md, a Markdown
 * file), set out to read: the contents on the left (a click goes there),
 * the guide on the right, and search (Ctrl+F, or typing; Enter finds the
 * next). `help [file.md]` shows another Markdown file.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 900
#define HEIGHT 620
#define SIDEBAR 230
#define TOP 46
#define PAD 24
#define MAX_BLOCKS 2048
#define MAX_RUNS 40000
#define MAX_LINES 12000

enum block_kind { HEADING, PARAGRAPH, BULLET, NUMBERED, CODE, TABLE_ROW, RULE, PICTURE };

struct block {
    enum block_kind kind;
    int level;      /* HEADING: 1-3; BULLET: how deep. */
    char *text;     /* Its Markdown (a table row: the cells, split by '|'). */
    bool header;    /* TABLE_ROW: the table's first row. */
    int table;      /* TABLE_ROW: which table (for column widths). */
};

/* A piece of text on a line, in one style. */
struct run {
    int x;
    const char *text;
    int length;
    unsigned char style; /* 0 plain, 1 bold, 2 code, 3 heading 1, 4 heading 2, 5 heading 3, 6 dim */
};

struct line {
    int y, height;
    int first_run, run_count;
    int block;
    uint32_t background; /* Code, table headers: a band behind; 0 none. */
};

static struct vx_window *window;
static char file[512] = "/share/help/USER-GUIDE.md";
static char *source;
static struct block blocks[MAX_BLOCKS];
static int block_count;
static struct run runs[MAX_RUNS];
static int run_count;
static struct line lines[MAX_LINES];
static int line_count, content_height;
static int scroll, toc_scroll;
static char search[80];
static bool searching;
static int found_block = -1;
static bool ctrl;

/* Table columns: the widest cell of each, per table. */
#define MAX_TABLES 64
#define MAX_COLUMNS 8
static int column_widths[MAX_TABLES][MAX_COLUMNS];

static const struct vx_font *font_of(int style) {
    switch (style) {
    case 1: return vx_font(VX_FACE_BOLD, 14);
    case 2: return vx_font(VX_FACE_MONO, 13);
    case 3: return vx_font(VX_FACE_BOLD, 28);
    case 4: return vx_font(VX_FACE_BOLD, 21);
    case 5: return vx_font(VX_FACE_BOLD, 16);
    default: return vx_font(VX_FACE_SANS, 14);
    }
}

/* ---- Reading the Markdown ---- */

static char *read_whole(const char *path) {
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return NULL;
    }
    size_t size = 0, cap = 65536;
    char *data = malloc(cap);
    long n;
    while (data && (n = vx_read(handle, data + size, cap - size - 1)) > 0) {
        size += (size_t)n;
        if (cap - size < 4096) {
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
    return data;
}

static void add_block(enum block_kind kind, int level, char *text) {
    if (block_count < MAX_BLOCKS) {
        blocks[block_count++] = (struct block){kind, level, text, false, 0};
    }
}

static void parse(void) {
    block_count = 0;
    free(source);
    source = read_whole(file);
    if (!source) {
        static char missing[] = "# Help\n\nThe guide isn't here (/share/help/USER-GUIDE.md).";
        source = strdup(missing);
    }
    char *paragraph = NULL; /* A paragraph being gathered (lines joined). */
    bool in_code = false, in_table = false;
    int tables = 0;
    for (char *line = source, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (!strncmp(line, "```", 3)) {
            in_code = !in_code;
            paragraph = NULL;
            continue;
        }
        if (in_code) {
            add_block(CODE, 0, line);
            continue;
        }
        char *t = line;
        while (*t == ' ') {
            t++;
        }
        int indent = (int)(t - line);
        if (!*t) {
            paragraph = NULL;
            in_table = false;
            continue;
        }
        if (*t == '|') {
            /* A table row; the |---| line under the header isn't one. */
            if (strspn(t, "|-: ") == strlen(t)) {
                continue;
            }
            if (!in_table) {
                tables++;
            }
            add_block(TABLE_ROW, 0, t);
            blocks[block_count - 1].header = !in_table;
            blocks[block_count - 1].table = tables < MAX_TABLES ? tables - 1 : MAX_TABLES - 1;
            in_table = true;
            paragraph = NULL;
            continue;
        }
        in_table = false;
        if (*t == '#') {
            int level = 0;
            while (t[level] == '#') {
                level++;
            }
            add_block(HEADING, level > 3 ? 3 : level, t + level + (t[level] == ' '));
            paragraph = NULL;
            continue;
        }
        if (!strcmp(t, "---") || !strcmp(t, "***")) {
            add_block(RULE, 0, t);
            paragraph = NULL;
            continue;
        }
        if (!strncmp(t, "![", 2)) {
            add_block(PICTURE, 0, t);
            paragraph = NULL;
            continue;
        }
        if ((*t == '-' || *t == '*') && t[1] == ' ') {
            add_block(BULLET, indent / 2, t + 2);
            paragraph = blocks[block_count - 1].text;
            continue;
        }
        if (*t >= '0' && *t <= '9') {
            char *dot = t;
            while (*dot >= '0' && *dot <= '9') {
                dot++;
            }
            if (*dot == '.' && dot[1] == ' ') {
                add_block(NUMBERED, indent / 2, t);
                paragraph = blocks[block_count - 1].text;
                continue;
            }
        }
        if (paragraph) {
            /* A line that goes on the one before: join them. */
            size_t n = strlen(paragraph);
            if (paragraph + n + 1 == line) { /* (Right after it in the source.) */
                paragraph[n] = ' ';
                memmove(paragraph + n + 1, t, strlen(t) + 1);
                continue;
            }
            char *joined = malloc(n + strlen(t) + 2);
            if (joined) {
                sprintf(joined, "%s %s", paragraph, t);
                blocks[block_count - 1].text = joined;
                paragraph = joined;
            }
            continue;
        }
        add_block(PARAGRAPH, 0, t);
        paragraph = blocks[block_count - 1].text;
    }
}

/* ---- Laying it out ---- */

static struct line *new_line(int y, int height, int block, uint32_t background) {
    if (line_count == MAX_LINES) {
        return NULL;
    }
    struct line *l = &lines[line_count++];
    *l = (struct line){y, height, run_count, 0, block, background};
    return l;
}

static void add_run(struct line *l, int x, const char *text, int length, int style) {
    if (!l || run_count == MAX_RUNS || length <= 0) {
        return;
    }
    runs[run_count++] = (struct run){x, text, length, (unsigned char)style};
    l->run_count++;
}

/* Inline Markdown into words with styles, wrapped to `width` from `x0`. */
static int lay_text(const char *text, int base_style, int x0, int width, int y, int block) {
    int line_height = vx_font_height(font_of(base_style)) + 6;
    struct line *l = new_line(y, line_height, block, 0);
    int x = x0;
    int style = base_style;
    bool bold = false, code = false;
    const char *p = text;
    while (*p) {
        /* Markup: **bold**, `code`, [text](link) (just the text), \x. */
        if (!code && p[0] == '*' && p[1] == '*') {
            bold = !bold;
            style = bold ? (base_style >= 3 ? base_style : 1) : base_style;
            p += 2;
            continue;
        }
        if (*p == '`') {
            code = !code;
            style = code ? 2 : bold ? 1 : base_style;
            p++;
            continue;
        }
        if (!code && *p == '[') {
            p++;
            continue;
        }
        if (!code && *p == ']' && p[1] == '(') {
            const char *close = strchr(p, ')');
            p = close ? close + 1 : p + 1;
            continue;
        }
        if (!code && *p == '\\' && p[1]) {
            p++;
        }
        /* A word (and the space after it). */
        const char *end = p;
        while (*end && *end != ' ' && !(*end == '*' && end[1] == '*') && *end != '`' &&
               !(!code && (*end == '[' || (*end == ']' && end[1] == '(')))) {
            end++;
        }
        while (*end == ' ' && end == p) {
            end++;
        }
        if (end == p) {
            end++;
        }
        int n = (int)(end - p);
        int w = vx_text_width_bytes(font_of(style), p, (size_t)n);
        if (x + w > x0 + width && x > x0 && *p != ' ') {
            y += line_height;
            l = new_line(y, line_height, block, 0);
            x = x0;
        }
        if (!(x == x0 && *p == ' ')) {
            add_run(l, x, p, n, style);
            x += w;
        }
        p = end;
    }
    return y + line_height;
}

static int plain_width(const char *text, int length) {
    /* A cell's width without its markup. */
    char clean[256];
    int n = 0;
    for (int i = 0; i < length && n < 255; i++) {
        if (text[i] == '`' || (text[i] == '*' && i + 1 < length && text[i + 1] == '*')) {
            if (text[i] == '*') {
                i++;
            }
            continue;
        }
        clean[n++] = text[i];
    }
    clean[n] = '\0';
    return vx_text_width_font(font_of(0), clean);
}

static void measure_tables(void) {
    memset(column_widths, 0, sizeof(column_widths));
    for (int b = 0; b < block_count; b++) {
        if (blocks[b].kind != TABLE_ROW) {
            continue;
        }
        const char *p = blocks[b].text + 1;
        for (int c = 0; c < MAX_COLUMNS && *p; c++) {
            const char *bar = strchr(p, '|');
            int n = bar ? (int)(bar - p) : (int)strlen(p);
            int w = plain_width(p, n) + 24;
            int *cw = &column_widths[blocks[b].table][c];
            *cw = w > *cw ? w : *cw;
            if (!bar) {
                break;
            }
            p = bar + 1;
        }
    }
}

static void layout(void) {
    run_count = line_count = 0;
    measure_tables();
    int width = window->surface.width - SIDEBAR - 2 * PAD;
    int x0 = SIDEBAR + PAD, y = PAD;
    for (int b = 0; b < block_count; b++) {
        struct block *k = &blocks[b];
        switch (k->kind) {
        case HEADING:
            y += k->level == 1 ? 8 : 18;
            y = lay_text(k->text, 2 + k->level, x0, width, y, b);
            if (k->level <= 2) {
                struct line *l = new_line(y, 8, b, 0);
                (void)l;
                y += 8;
            }
            break;
        case PARAGRAPH: y = lay_text(k->text, 0, x0, width, y, b) + 8; break;
        case BULLET:
        case NUMBERED: {
            int indent = 18 + k->level * 18;
            struct line *mark = new_line(y, 0, b, 0);
            add_run(mark, x0 + indent - 14, k->kind == BULLET ? "\xe2\x80\xa2" : "", 3, 0);
            y = lay_text(k->text, 0, x0 + indent, width - indent, y, b) + 2;
            break;
        }
        case CODE: {
            struct line *l = new_line(y, 18, b, vx_theme.dark ? 0x241c38 : 0xeceaf2);
            add_run(l, x0 + 10, k->text, (int)strlen(k->text), 2);
            y += 18;
            if (b + 1 >= block_count || blocks[b + 1].kind != CODE) {
                y += 10;
            }
            break;
        }
        case TABLE_ROW: {
            /* Cells side by side, each cut to its column. */
            int columns_total = 0;
            for (int c = 0; c < MAX_COLUMNS; c++) {
                columns_total += column_widths[k->table][c];
            }
            float shrink = columns_total > width ? (float)width / columns_total : 1;
            struct line *l = new_line(y, 24, b, k->header ? (vx_theme.dark ? 0x2a2140 : 0xe4e0ee) : 0);
            const char *p = k->text + 1;
            int x = x0;
            for (int c = 0; c < MAX_COLUMNS && *p; c++) {
                const char *bar = strchr(p, '|');
                int n = bar ? (int)(bar - p) : (int)strlen(p);
                while (n && *p == ' ') {
                    p++, n--;
                }
                while (n && p[n - 1] == ' ') {
                    n--;
                }
                int cw = (int)(column_widths[k->table][c] * shrink);
                /* Markup is dropped in cells (the style of the whole cell is enough). */
                int style = k->header ? 1 : (n && *p == '`') ? 2 : 0;
                const char *text = p;
                int length = n;
                if (length >= 2 && text[0] == '`' && text[length - 1] == '`') {
                    text++, length -= 2;
                } else if (length >= 4 && !strncmp(text, "**", 2) && !strncmp(text + length - 2, "**", 2)) {
                    text += 2, length -= 4, style = 1;
                }
                /* Cut to the column's width. */
                while (length > 0 && vx_text_width_bytes(font_of(style), text, (size_t)length) > cw - 12) {
                    length = (int)vx_utf8_previous(text, (size_t)length);
                }
                add_run(l, x + 6, text, length, style);
                x += cw;
                if (!bar) {
                    break;
                }
                p = bar + 1;
            }
            y += 24;
            if (b + 1 >= block_count || blocks[b + 1].kind != TABLE_ROW) {
                y += 10;
            }
            break;
        }
        case RULE: new_line(y + 8, 1, b, 0); y += 18; break;
        case PICTURE: {
            struct line *l = new_line(y, 20, b, 0);
            static const char text[] = "(a picture: see the guide on GitHub)";
            add_run(l, x0, text, (int)strlen(text), 6);
            y += 28;
            break;
        }
        }
    }
    content_height = y + PAD;
}

/* ---- Drawing ---- */

static int view_height(void) {
    return window->surface.height - TOP;
}

static int heading_count(void) {
    int n = 0;
    for (int b = 0; b < block_count; b++) {
        n += blocks[b].kind == HEADING && blocks[b].level >= 2;
    }
    return n;
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_VIEW);
    int max = content_height - view_height();
    scroll = scroll > max ? max : scroll < 0 ? 0 : scroll;
    /* The guide. */
    struct vx_surface view = {s->pixels + (long)TOP * s->stride, w, h - TOP, s->stride};
    for (int i = 0; i < line_count; i++) {
        struct line *l = &lines[i];
        int y = l->y - scroll;
        if (y + l->height < 0 || y > view.height) {
            continue;
        }
        if (l->background) {
            vx_fill(&view, SIDEBAR + PAD, y, w - SIDEBAR - 2 * PAD, l->height, l->background);
        }
        if (l->block == found_block) {
            vx_fill(&view, SIDEBAR + PAD - 8, y, 3, l->height, VX_COLOR_ACCENT);
        }
        struct block *k = &blocks[l->block];
        if (k->kind == RULE || (k->kind == HEADING && k->level <= 2 && !l->run_count && l->height == 8)) {
            vx_fill(&view, SIDEBAR + PAD, y + l->height / 2, w - SIDEBAR - 2 * PAD, 1, VX_COLOR_LINE);
        }
        if (k->kind == TABLE_ROW) {
            vx_fill(&view, SIDEBAR + PAD, y + l->height - 1, w - SIDEBAR - 2 * PAD, 1, VX_COLOR_LINE);
        }
        for (int r = 0; r < l->run_count; r++) {
            struct run *run = &runs[l->first_run + r];
            char piece[1024];
            int n = run->length < (int)sizeof(piece) - 1 ? run->length : (int)sizeof(piece) - 1;
            memcpy(piece, run->text, (size_t)n);
            piece[n] = '\0';
            const struct vx_font *f = font_of(run->style);
            uint32_t color = run->style >= 3 && run->style <= 5 ? VX_COLOR_TEXT
                             : run->style == 2 ? (vx_theme.dark ? 0xc3e88d : 0x15803d)
                             : run->style == 6 ? VX_COLOR_DIM : VX_COLOR_TEXT;
            int ty = y + (l->height - vx_font_height(f)) / 2;
            if (l->height == 0) { /* A bullet: with the line after. */
                ty = y + 3;
            }
            vx_text(&view, f, run->x, ty, piece, color, VX_TRANSPARENT);
        }
    }
    /* The scroll bar. */
    if (content_height > view_height()) {
        int bar = view_height() * view_height() / content_height;
        bar = bar < 24 ? 24 : bar;
        int y = TOP + (view_height() - bar) * scroll / (max > 0 ? max : 1);
        vx_fill(s, w - 8, y, 4, bar, VX_COLOR_LINE);
    }
    /* The contents. */
    vx_fill(s, 0, 0, SIDEBAR, h, vx_theme.sidebar);
    vx_fill(s, SIDEBAR - 1, 0, 1, h, VX_COLOR_LINE);
    int y = TOP + 6 - toc_scroll;
    int current = -1;
    for (int i = 0; i < line_count; i++) {
        if (lines[i].y <= scroll + 30 && blocks[lines[i].block].kind == HEADING) {
            current = lines[i].block;
        }
    }
    for (int b = 0; b < block_count; b++) {
        if (blocks[b].kind != HEADING || blocks[b].level < 2) {
            continue;
        }
        if (y >= TOP && y < h - 20) {
            int indent = blocks[b].level == 3 ? 26 : 12;
            if (b == current) {
                vx_draw_selection(s, 4, y - 2, SIDEBAR - 8, 22);
            }
            char title[128];
            const char *t = blocks[b].text;
            int n = 0;
            for (; *t && n < 127; t++) {
                if (*t != '`' && *t != '*') {
                    title[n++] = *t;
                }
            }
            title[n] = '\0';
            vx_draw_text_fit(s, indent, y, SIDEBAR - indent - 10, title,
                             blocks[b].level == 2 ? VX_COLOR_TEXT : VX_COLOR_DIM, VX_TRANSPARENT);
        }
        y += 22;
    }
    /* The search field, over the top. */
    vx_draw_toolbar(s, 0, 0, w, TOP);
    vx_text(s, vx_font(VX_FACE_BOLD, 15), 14, 13, "Vexa Help", VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_field(s, w - 280, 10, 266, search, searching);
    if (!search[0] && !searching) {
        vx_draw_text(s, w - 274, 14, "Search the guide", VX_COLOR_DIM, VX_TRANSPARENT);
    }
    vx_window_present(window, 0, 0, w, h);
}

/* ---- Search and moving ---- */

static bool contains(const char *text, const char *what) {
    size_t n = strlen(what);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 0x20) == (what[i] | 0x20)) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static void show_block(int b) {
    for (int i = 0; i < line_count; i++) {
        if (lines[i].block == b) {
            scroll = lines[i].y - 40;
            return;
        }
    }
}

static void find_next(void) {
    if (!search[0]) {
        return;
    }
    for (int step = 1; step <= block_count; step++) {
        int b = (found_block + step + block_count) % block_count;
        if (contains(blocks[b].text, search)) {
            found_block = b;
            show_block(b);
            printf("help: found \"%s\"\n", search);
            fflush(stdout);
            return;
        }
    }
}

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
        return;
    }
    if (!e->value) {
        return;
    }
    if (ctrl && e->key == 33) { /* F */
        searching = true;
        return;
    }
    switch (e->key) {
    case VX_KEY_DOWN: scroll += 40; return;
    case VX_KEY_UP: scroll -= 40; return;
    case VX_KEY_PAGEDOWN: scroll += view_height() - 40; return;
    case VX_KEY_PAGEUP: scroll -= view_height() - 40; return;
    case VX_KEY_HOME: scroll = 0; return;
    case VX_KEY_END: scroll = content_height; return;
    case VX_KEY_ENTER: find_next(); return;
    case VX_KEY_ESC:
        search[0] = '\0';
        searching = false;
        found_block = -1;
        return;
    }
    if (e->character == ' ' && !searching) {
        scroll += view_height() - 40;
        return;
    }
    if (e->character >= ' ' || e->key == VX_KEY_BACKSPACE) {
        searching = true;
        if (vx_field_key(search, sizeof(search), e)) {
            found_block = -1;
            find_next();
        }
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    if (e->wheel) {
        if (e->x < SIDEBAR) {
            toc_scroll -= e->wheel * 44;
            int max = heading_count() * 22 - (window->surface.height - TOP - 20);
            toc_scroll = toc_scroll > max ? max : toc_scroll;
            toc_scroll = toc_scroll < 0 ? 0 : toc_scroll;
        } else {
            scroll -= e->wheel * 60;
        }
    }
    if (!click) {
        return;
    }
    searching = vx_inside(e->x, e->y, window->surface.width - 280, 10, 266, 24);
    if (e->x < SIDEBAR && e->y > TOP) {
        int y = TOP + 6 - toc_scroll;
        for (int b = 0; b < block_count; b++) {
            if (blocks[b].kind != HEADING || blocks[b].level < 2) {
                continue;
            }
            if (e->y >= y - 2 && e->y < y + 20) {
                show_block(b);
                printf("help: showing \"%s\"\n", blocks[b].text);
                fflush(stdout);
                return;
            }
            y += 22;
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        snprintf(file, sizeof(file), "%s", argv[1]);
    }
    parse();
    window = vx_window_create_flags("Help", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "help: no desktop to open a window on\n");
        return 1;
    }
    layout();
    int held = 0;
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
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_THEME: layout(); break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = false;
            }
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 600 && e.height >= 300) {
                vx_window_resize(window, e.width, e.height);
                layout();
            }
            break;
        }
    }
}
