/* Search (Ctrl+Space, or the magnifier on the panel), as macOS's Spotlight:
 * type, and it finds apps, Settings' sections and files (in the home
 * folder, the shared pictures, /tmp and the disks), and does sums
 * ("12*(3+4)"). Up and Down choose, Enter opens, Escape closes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

#define BOX_WIDTH 620
#define FIELD_HEIGHT 48
#define ROW_HEIGHT 34
#define MAX_RESULTS 9
#define MAX_FILES 4000

bool search_open;
static char query[128];
static int selected, hot = -1;

enum result_kind { R_CALC, R_APP, R_SETTING, R_FILE };
static struct result {
    enum result_kind kind;
    int index;          /* R_APP: the app; R_SETTING: the section; R_FILE: the file. */
    char text[96];      /* What's shown. */
    char detail[160];   /* On the right, dimmer. */
} results[MAX_RESULTS];
static int result_count;

/* Settings' sections (the same as the Settings app's, which opens at one
 * given by name), and words that find them. */
static const struct {
    const char *label, *keywords;
} setting_sections[] = {
    {"Appearance", "theme dark light mode accent color colour look"},
    {"Wallpaper", "background picture image gradient desktop"},
    {"Desktop & Panel", "icons clock seconds date weekday notifications snapping title bar"},
    {"Date & Time", "clock time zone city utc 12 24 hours"},
    {"Mouse & Keyboard", "pointer speed double click scroll natural left handed layout key repeat"},
    {"Display", "resolution screen size scale monitor"},
    {"Lock Screen", "screensaver password lock idle sleep security"},
    {"Default Apps", "open with file types associations"},
    {"Startup", "login start apps terminal"},
    {"Network", "computer name hostname ip address internet"},
    {"Storage", "disks space free usage"},
    {"About", "version system memory cpu restart shut down power"},
};
#define SETTING_COUNT (int)(sizeof(setting_sections) / sizeof(setting_sections[0]))

/* ---- The files' names, read when search opens ---- */

static char **files;
static bool *file_is_dir;
static int file_count;

static void forget_files(void) {
    for (int i = 0; i < file_count; i++) {
        free(files[i]);
    }
    free(files);
    free(file_is_dir);
    files = NULL;
    file_is_dir = NULL;
    file_count = 0;
}

static void walk(const char *dir, int depth) {
    if (depth > 6 || file_count >= MAX_FILES) {
        return;
    }
    int handle = vx_open(dir, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    struct vx_dir_entry entries[16];
    long n;
    while ((n = vx_read_dir(handle, entries, 16)) > 0 && file_count < MAX_FILES) {
        for (long i = 0; i < n && file_count < MAX_FILES; i++) {
            const char *name = entries[i].name;
            if (name[0] == '.' || !strcmp(name, "lost+found")) {
                continue;
            }
            /* Not the boot CD (Linux's files: thousands nobody looks for). */
            if (!strcmp(dir, "/mnt") && !strncmp(name, "cd", 2)) {
                continue;
            }
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", name);
            bool is_dir = entries[i].type == VX_TYPE_DIRECTORY;
            files[file_count] = strdup(path);
            file_is_dir[file_count] = is_dir;
            if (files[file_count]) {
                file_count++;
            }
            /* Bundles are apps (found as such), not folders to look in. */
            size_t length = strlen(name);
            bool bundle = length > 6 && !strcmp(name + length - 6, VX_APP_EXTENSION);
            if (is_dir && !bundle) {
                walk(path, depth + 1);
            }
        }
    }
    vx_close(handle);
}

static void read_files(void) {
    forget_files();
    files = malloc(MAX_FILES * sizeof(*files));
    file_is_dir = malloc(MAX_FILES * sizeof(*file_is_dir));
    if (!files || !file_is_dir) {
        free(files);
        free(file_is_dir);
        files = NULL;
        file_is_dir = NULL;
        return;
    }
    static const char *const roots[] = {HOME, "/share", "/tmp", "/mnt"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        walk(roots[i], 0);
    }
}

/* ---- Matching ---- */

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* How well `text` matches the query: 0 not at all, 3 at its start, 2 at a
 * word's start, 1 anywhere. */
static int match(const char *text, const char *q) {
    size_t n = strlen(q);
    if (!n) {
        return 0;
    }
    int best = 0;
    for (const char *p = text; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && lower((unsigned char)p[k]) == lower((unsigned char)q[k])) {
            k++;
        }
        if (k == n) {
            int score = p == text ? 3 : (p[-1] == ' ' || p[-1] == '-' || p[-1] == '_') ? 2 : 1;
            best = score > best ? score : best;
        }
    }
    return best;
}

/* ---- Sums: + - * / and brackets, in fixed point (six decimals) ---- */

#define ONE 1000000LL
static const char *expr_at;
static bool expr_bad;

static long long expr_sum(void);

static void skip_spaces(void) {
    while (*expr_at == ' ') {
        expr_at++;
    }
}

static long long expr_value(void) {
    skip_spaces();
    if (*expr_at == '(') {
        expr_at++;
        long long v = expr_sum();
        skip_spaces();
        if (*expr_at != ')') {
            expr_bad = true;
        } else {
            expr_at++;
        }
        return v;
    }
    if (*expr_at == '-') {
        expr_at++;
        return -expr_value();
    }
    if (!((*expr_at >= '0' && *expr_at <= '9') || *expr_at == '.')) {
        expr_bad = true;
        return 0;
    }
    long long whole = 0, fraction = 0, scale = ONE;
    while (*expr_at >= '0' && *expr_at <= '9') {
        whole = whole * 10 + (*expr_at++ - '0');
        if (whole > 1000000000000LL) {
            expr_bad = true;
        }
    }
    if (*expr_at == '.') {
        expr_at++;
        while (*expr_at >= '0' && *expr_at <= '9') {
            if (scale > 1) {
                scale /= 10;
                fraction += (*expr_at - '0') * scale;
            }
            expr_at++;
        }
    }
    return whole * ONE + fraction;
}

static long long expr_product(void) {
    long long v = expr_value();
    for (;;) {
        skip_spaces();
        char op = *expr_at;
        if (op != '*' && op != '/' && op != 'x') {
            return v;
        }
        expr_at++;
        long long w = expr_value();
        if (op == '/') {
            if (!w) {
                expr_bad = true;
                return 0;
            }
            v = (long long)((long double)v * ONE / w);
        } else {
            v = (long long)((long double)v * w / ONE);
        }
    }
}

static long long expr_sum(void) {
    long long v = expr_product();
    for (;;) {
        skip_spaces();
        char op = *expr_at;
        if (op != '+' && op != '-') {
            return v;
        }
        expr_at++;
        long long w = expr_product();
        v = op == '+' ? v + w : v - w;
    }
}

/* The query as a sum: true (and its value as text) if it is one. */
static bool calculate(const char *q, char *out, size_t size) {
    if (!strpbrk(q, "+-*/x") || !strpbrk(q, "0123456789")) {
        return false;
    }
    for (const char *p = q; *p; p++) {
        if (!strchr("0123456789.+-*/x() ", *p)) {
            return false;
        }
    }
    expr_at = q;
    expr_bad = false;
    long long v = expr_sum();
    skip_spaces();
    if (expr_bad || *expr_at) {
        return false;
    }
    bool negative = v < 0;
    unsigned long long a = negative ? (unsigned long long)-v : (unsigned long long)v;
    a += 5; /* Rounded to five decimals. */
    unsigned long long whole = a / ONE, fraction = a % ONE / 10;
    char decimals[8] = "";
    if (fraction) {
        snprintf(decimals, sizeof(decimals), ".%05llu", fraction);
        for (int i = (int)strlen(decimals) - 1; decimals[i] == '0'; i--) {
            decimals[i] = '\0';
        }
    }
    snprintf(out, size, "%s%llu%s", negative && (whole || fraction) ? "-" : "", whole, decimals);
    return true;
}

/* ---- The results ---- */

static void add_result(enum result_kind kind, int index, const char *text, const char *detail) {
    if (result_count == MAX_RESULTS) {
        return;
    }
    struct result *r = &results[result_count++];
    r->kind = kind;
    r->index = index;
    snprintf(r->text, sizeof(r->text), "%s", text);
    snprintf(r->detail, sizeof(r->detail), "%s", detail);
}

static void find(void) {
    result_count = 0;
    selected = 0;
    hot = -1;
    if (!query[0]) {
        return;
    }
    char sum[48];
    if (calculate(query, sum, sizeof(sum))) {
        char text[96];
        snprintf(text, sizeof(text), "= %s", sum);
        add_result(R_CALC, 0, text, "Calculator");
    }
    for (int score = 3; score >= 1; score--) {
        for (int i = 0; i < app_count; i++) {
            if (match(apps[i].name, query) == score) {
                add_result(R_APP, i, apps[i].name, "App");
            }
        }
    }
    for (int score = 3; score >= 1; score--) {
        for (int i = 0; i < SETTING_COUNT; i++) {
            int m = match(setting_sections[i].label, query);
            if (!m && match(setting_sections[i].keywords, query) >= 2) {
                m = 1;
            }
            if (m == score) {
                add_result(R_SETTING, i, setting_sections[i].label, "Settings");
            }
        }
    }
    for (int score = 3; score >= 1 && result_count < MAX_RESULTS; score--) {
        for (int i = 0; i < file_count && result_count < MAX_RESULTS; i++) {
            const char *name = strrchr(files[i], '/') + 1;
            if (match(name, query) == score) {
                add_result(R_FILE, i, name, files[i]);
            }
        }
    }
}

/* ---- Showing it ---- */

struct rect search_rect(void) {
    int height = FIELD_HEIGHT + (result_count ? result_count * ROW_HEIGHT + 12 : 0);
    return (struct rect){(screen.width - BOX_WIDTH) / 2, screen.height / 5, BOX_WIDTH, height};
}

static struct rect damage_rect(void) {
    struct rect r = search_rect();
    return (struct rect){r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW,
                         FIELD_HEIGHT + MAX_RESULTS * ROW_HEIGHT + 12 + 2 * SHADOW + 4};
}

void search_show(void) {
    if (search_open) {
        return;
    }
    search_open = true;
    query[0] = '\0';
    result_count = 0;
    read_files();
    add_damage(damage_rect());
    printf("desktop: search (%d files)\n", file_count);
}

void search_hide(void) {
    if (!search_open) {
        return;
    }
    add_damage(damage_rect());
    search_open = false;
    forget_files();
}

static void open_result(int i) {
    if (i < 0 || i >= result_count) {
        return;
    }
    struct result r = results[i];
    char query_shown[128];
    snprintf(query_shown, sizeof(query_shown), "%s", query);
    search_hide();
    printf("desktop: search opens \"%s\"\n", r.text);
    switch (r.kind) {
    case R_CALC: {
        char note[200];
        snprintf(note, sizeof(note), "Calculator: %s %s", query_shown, r.text);
        add_note(note);
        break;
    }
    case R_APP: run_app(r.index); break;
    case R_SETTING: run_named("Settings", setting_sections[r.index].label); break;
    case R_FILE: open_path(r.detail); break;
    }
}

void search_key(int key, int value, int character) {
    if (!value) {
        return;
    }
    add_damage(damage_rect());
    if (key == VX_KEY_ESC) {
        search_hide();
        return;
    }
    if (key == VX_KEY_ENTER) {
        open_result(selected);
        return;
    }
    if (key == VX_KEY_DOWN) {
        selected = result_count ? (selected + 1) % result_count : 0;
        return;
    }
    if (key == VX_KEY_UP) {
        selected = result_count ? (selected + result_count - 1) % result_count : 0;
        return;
    }
    struct vx_gui_event e = {.type = VX_GUI_KEY, .key = key, .value = value, .character = character};
    if (vx_field_key(query, sizeof(query), &e)) {
        find();
    }
}

static int row_at(int x, int y) {
    struct rect r = search_rect();
    int top = r.y + FIELD_HEIGHT + 6;
    if (x < r.x || x >= r.x + r.width || y < top) {
        return -1;
    }
    int i = (y - top) / ROW_HEIGHT;
    return i < result_count ? i : -1;
}

void search_button(bool down) {
    if (!down) {
        return;
    }
    if (!inside(search_rect(), pointer_x, pointer_y)) {
        search_hide();
        return;
    }
    int i = row_at(pointer_x, pointer_y);
    if (i >= 0) {
        open_result(i);
    }
}

void search_pointer(void) {
    int i = row_at(pointer_x, pointer_y);
    if (i != hot) {
        hot = i;
        if (i >= 0) {
            selected = i;
        }
        add_damage(damage_rect());
    }
}

void search_draw(struct vx_surface *view, int ox, int oy) {
    struct rect r = search_rect();
    r.x += ox, r.y += oy;
    draw_glass_popup(view, r, 12, 170);
    draw_magnifier(view, r.x + 16, r.y + 14, 20, vx_theme.dim);
    const struct vx_font *big = vx_font(VX_FACE_SANS, 20);
    int text_y = r.y + (FIELD_HEIGHT - vx_font_height(big)) / 2;
    int end;
    if (query[0]) {
        end = vx_text(view, big, r.x + 48, text_y, query, vx_theme.text, VX_TRANSPARENT);
    } else {
        vx_text(view, big, r.x + 48, text_y, "Search apps, settings and files", vx_theme.dim,
                VX_TRANSPARENT);
        end = r.x + 48;
    }
    vx_fill(view, end + 1, text_y + 2, 2, vx_font_height(big) - 4, vx_theme.accent);
    if (!result_count) {
        return;
    }
    vx_fill(view, r.x, r.y + FIELD_HEIGHT, r.width, 1, vx_theme.line);
    int top = r.y + FIELD_HEIGHT + 6;
    for (int i = 0; i < result_count; i++) {
        struct result *res = &results[i];
        int y = top + i * ROW_HEIGHT;
        if (i == selected) {
            vx_draw_gel(view, r.x + 6, y, r.width - 12, ROW_HEIGHT, 7, vx_theme.accent);
        }
        struct vx_image *icon = res->kind == R_APP ? app_icons[res->index]
                                : res->kind == R_FILE ? file_icon(files[res->index],
                                                                  file_is_dir[res->index])
                                : res->kind == R_SETTING && app_named_icon("Settings")
                                    ? app_named_icon("Settings")
                                    : NULL;
        if (icon) {
            vx_blit_alpha(view, r.x + 16, y + 5, 24, 24, &icon->surface);
        } else if (res->kind == R_CALC) {
            vx_text(view, vx_font(VX_FACE_BOLD, 18), r.x + 20, y + 6, "=",
                    i == selected ? 0xffffff : vx_theme.accent, VX_TRANSPARENT);
        }
        uint32_t text_color = i == selected ? 0xffffff : vx_theme.text;
        uint32_t dim = i == selected ? vx_mix(0xffffff, vx_theme.accent, 80) : vx_theme.dim;
        int detail_width = vx_text_width(res->detail);
        int room = r.width - 60 - 24;
        int name_room = room - (detail_width < room / 2 ? detail_width : room / 2) - 16;
        const struct vx_font *f = res->kind == R_CALC ? vx_font(VX_FACE_BOLD, 15) : vx_font_ui();
        if (res->kind == R_CALC) {
            vx_text(view, f, r.x + 50, y + 8, res->text + 2, text_color, VX_TRANSPARENT);
        } else {
            vx_draw_text_fit(view, r.x + 50, y + 9, name_room, res->text, text_color, VX_TRANSPARENT);
        }
        int detail_room = room - name_room - 16;
        int dx = r.x + r.width - 20 - (detail_width < detail_room ? detail_width : detail_room);
        vx_draw_text_fit(view, dx, y + 9, detail_room, res->detail, dim, VX_TRANSPARENT);
    }
}
