/* software: the Software app. Browses the package index (by category, or
 * what's installed, or what has updates) and installs, updates, opens and
 * removes apps, through pkg (which does the downloading and checking).
 */
#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <vexa/app.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 820
#define HEIGHT 540
#define SIDEBAR 190
#define HEADER 64
#define ROW 76
#define INDEX_CACHE "/tmp/pkg-index.conf"
#define MAX_PACKAGES 128
#define BUTTON_W 92

extern char **environ;

struct package {
    char id[48], name[64], version[24], category[24], summary[160], bundle[96];
    long size;
    char installed[24];
};

static struct package packages[MAX_PACKAGES];
static int package_count;

/* The sidebar's sections: everything, the categories, installed, updates. */
static const char *const sections[] = {"All Apps", "Accessories", "Games", "Graphics", "Internet",
                                       "Multimedia", "System", "Other", "Installed", "Updates"};
#define SECTIONS 10
#define SECTION_INSTALLED 8
#define SECTION_UPDATES 9
static int section;
static int scroll;
static char status[160] = "";

/* pkg, while it runs: its process, and what it's doing. */
static pid_t busy_pid;
static char busy_id[48];
static char busy_what[16];

static struct vx_window *window;
static int pointer_x = -1, pointer_y = -1;

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static void find_installed(void) {
    DIR *d = opendir(VX_APPS_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char path[512], line[128], id[48] = "", version[24] = "";
        snprintf(path, sizeof(path), "%s/%s/Contents/Package.conf", VX_APPS_DIR, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (!strncmp(line, "package=", 8)) {
                snprintf(id, sizeof(id), "%s", line + 8);
            } else if (!strncmp(line, "version=", 8)) {
                snprintf(version, sizeof(version), "%s", line + 8);
            }
        }
        fclose(f);
        for (int i = 0; i < package_count; i++) {
            if (!strcmp(packages[i].id, id)) {
                snprintf(packages[i].installed, sizeof(packages[i].installed), "%s", version);
            }
        }
    }
    if (d) {
        closedir(d);
    }
}

static bool load_index(void) {
    package_count = 0;
    FILE *f = fopen(INDEX_CACHE, "r");
    if (!f) {
        return false;
    }
    char line[600];
    struct package *p = NULL;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            p = end && package_count < MAX_PACKAGES ? &packages[package_count++] : NULL;
            if (p) {
                *end = '\0';
                memset(p, 0, sizeof(*p));
                snprintf(p->id, sizeof(p->id), "%s", line + 1);
            }
        } else if (p && eq) {
            *eq = '\0';
            const char *v = eq + 1;
            if (!strcmp(line, "name")) {
                snprintf(p->name, sizeof(p->name), "%s", v);
            } else if (!strcmp(line, "version")) {
                snprintf(p->version, sizeof(p->version), "%s", v);
            } else if (!strcmp(line, "category")) {
                snprintf(p->category, sizeof(p->category), "%s", v);
            } else if (!strcmp(line, "summary")) {
                snprintf(p->summary, sizeof(p->summary), "%s", v);
            } else if (!strcmp(line, "bundle")) {
                snprintf(p->bundle, sizeof(p->bundle), "%s", v);
            } else if (!strcmp(line, "size")) {
                p->size = atol(v);
            }
        }
    }
    fclose(f);
    find_installed();
    printf("software: %d package%s\n", package_count, package_count == 1 ? "" : "s");
    fflush(stdout);
    return true;
}

static bool has_update(const struct package *p) {
    return p->installed[0] && strcmp(p->installed, p->version);
}

static bool in_section(const struct package *p) {
    switch (section) {
    case 0: return true;
    case SECTION_INSTALLED: return p->installed[0];
    case SECTION_UPDATES: return has_update(p);
    default: {
        const char *category = p->category[0] ? p->category : "Other";
        bool known = false;
        for (int i = 1; i < SECTION_INSTALLED - 1; i++) {
            known |= !strcmp(category, sections[i]);
        }
        return !strcmp(sections[section], known ? category : "Other");
    }
    }
}

/* Starts pkg (update, install, remove) in the background. */
static void start_pkg(const char *what, const char *id) {
    if (busy_pid) {
        return;
    }
    const char *argv[] = {"/bin/pkg", what, id, NULL};
    if (posix_spawn(&busy_pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0) {
        busy_pid = 0;
        snprintf(status, sizeof(status), "Can't start pkg");
        return;
    }
    snprintf(busy_id, sizeof(busy_id), "%s", id ? id : "");
    snprintf(busy_what, sizeof(busy_what), "%s", what);
    if (!strcmp(what, "update")) {
        snprintf(status, sizeof(status), "Reading the list of apps...");
    } else {
        snprintf(status, sizeof(status), "%s %s...",
                 !strcmp(what, "install") ? "Installing" : "Removing", id);
    }
    printf("software: pkg %s %s\n", what, id ? id : "");
    fflush(stdout);
}

/* When pkg is done: read everything again. */
static bool check_pkg(void) {
    if (!busy_pid) {
        return false;
    }
    int code = 0;
    if (waitpid(busy_pid, &code, WNOHANG) != busy_pid) {
        return false;
    }
    busy_pid = 0;
    bool ok = WIFEXITED(code) && WEXITSTATUS(code) == 0;
    load_index();
    if (!strcmp(busy_what, "update")) {
        snprintf(status, sizeof(status), ok ? "" : "Can't read the list of apps (is there a network?)");
    } else {
        snprintf(status, sizeof(status), "%s %s %s", ok ? "Done:" : "Couldn't", busy_what, busy_id);
        printf("software: %s %s %s\n", busy_what, busy_id, ok ? "done" : "failed");
        fflush(stdout);
    }
    return true;
}

static uint32_t tile_color(const char *name) {
    static const uint32_t colors[] = {0x4c8dff, 0x824cff, 0x2fb6a8, 0x3fae5a,
                                      0xf08a2c, 0xe5508f, 0xd9534f, 0x6c7a89};
    unsigned hash = 5381;
    for (const char *c = name; *c; c++) {
        hash = hash * 33 + (unsigned char)*c;
    }
    return colors[hash % 8];
}

/* The visible packages, in order, and where row i is. */
static int visible(int *out) {
    int n = 0;
    for (int i = 0; i < package_count; i++) {
        if (in_section(&packages[i])) {
            out[n++] = i;
        }
    }
    return n;
}

static int row_y(int i) {
    return HEADER + 8 + i * ROW - scroll;
}

/* The buttons on a row: the main one (Install/Update/Open) and Remove. */
static void row_buttons(int i, int *main_x, int *remove_x, int *y) {
    *y = row_y(i) + (ROW - 28) / 2;
    *main_x = WIDTH - 20 - BUTTON_W;
    *remove_x = *main_x - 8 - BUTTON_W;
}

static bool hot(int x, int y, int w, int h) {
    return vx_inside(pointer_x, pointer_y, x, y, w, h);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_VIEW);
    /* The sidebar. */
    vx_fill(s, 0, 0, SIDEBAR, s->height, VX_COLOR_WINDOW);
    vx_fill(s, SIDEBAR - 1, 0, 1, s->height, VX_COLOR_LINE);
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 20);
    vx_text(s, bold, 18, 16, "Software", VX_COLOR_TEXT, VX_TRANSPARENT);
    int updates = 0;
    for (int i = 0; i < package_count; i++) {
        updates += has_update(&packages[i]);
    }
    for (int i = 0; i < SECTIONS; i++) {
        int y = 58 + i * 30 + (i >= SECTION_INSTALLED ? 14 : 0);
        if (i == section) {
            vx_draw_selection(s, 8, y - 4, SIDEBAR - 16, 26);
        }
        char label[48];
        snprintf(label, sizeof(label), i == SECTION_UPDATES && updates ? "%s (%d)" : "%s",
                 sections[i], updates);
        vx_draw_text(s, 20, y, label, i == section ? 0xffffff : VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    /* The list. */
    int shown[MAX_PACKAGES];
    int n = visible(shown);
    for (int r = 0; r < n; r++) {
        struct package *p = &packages[shown[r]];
        int y = row_y(r);
        if (y + ROW < HEADER || y > s->height) {
            continue;
        }
        int x = SIDEBAR + 20;
        vx_draw_gel(s, x, y + 14, 48, 48, 12, tile_color(p->name));
        char letter[2] = {p->name[0] ? p->name[0] : '?', 0};
        const struct vx_font *big = vx_font(VX_FACE_BOLD, 26);
        int lw = vx_text_width_font(big, letter);
        vx_text(s, big, x + 24 - lw / 2, y + 22, letter, 0xffffff, VX_TRANSPARENT);
        int tx = x + 64, room = WIDTH - 20 - 2 * BUTTON_W - 16 - tx;
        vx_text(s, vx_font(VX_FACE_BOLD, VX_UI_FONT_SIZE), tx, y + 14, p->name, VX_COLOR_TEXT,
                VX_TRANSPARENT);
        vx_draw_text_fit(s, tx, y + 34, room, p->summary, VX_COLOR_DIM, VX_TRANSPARENT);
        char detail[96];
        if (has_update(p)) {
            snprintf(detail, sizeof(detail), "%s (installed: %s) - %ld KiB", p->version,
                     p->installed, (p->size + 1023) / 1024);
        } else {
            snprintf(detail, sizeof(detail), "%s - %ld KiB%s", p->version, (p->size + 1023) / 1024,
                     p->installed[0] ? " - installed" : "");
        }
        vx_draw_text_fit(s, tx, y + 52, room, detail, VX_COLOR_DIM, VX_TRANSPARENT);
        int mx, rx, by;
        row_buttons(r, &mx, &rx, &by);
        bool working = busy_pid && !strcmp(busy_id, p->id);
        const char *label = working ? "Working..." : has_update(p) ? "Update"
                                                   : p->installed[0] ? "Open" : "Install";
        unsigned flags = (busy_pid ? VX_BUTTON_DISABLED : 0) |
                         (!p->installed[0] || has_update(p) ? VX_BUTTON_HOT : 0);
        if (!busy_pid && hot(mx, by, BUTTON_W, 28)) {
            flags |= VX_BUTTON_HOT;
        }
        vx_draw_button_flags(s, mx, by, BUTTON_W, 28, label, flags);
        if (p->installed[0]) {
            vx_draw_button_flags(s, rx, by, BUTTON_W, 28, "Remove",
                                 busy_pid ? VX_BUTTON_DISABLED : hot(rx, by, BUTTON_W, 28) ? VX_BUTTON_HOT : 0);
        }
        vx_fill(s, x, y + ROW - 1, WIDTH - x - 20, 1, VX_COLOR_LINE);
    }
    /* The header, over the list. */
    vx_fill(s, SIDEBAR, 0, WIDTH - SIDEBAR, HEADER, VX_COLOR_VIEW);
    vx_text(s, vx_font(VX_FACE_BOLD, 22), SIDEBAR + 20, 12, sections[section], VX_COLOR_TEXT,
            VX_TRANSPARENT);
    const char *note = status[0] ? status
                       : !package_count ? "No apps to show yet: Refresh reads the list."
                       : !n ? "Nothing here." : "";
    vx_draw_text_fit(s, SIDEBAR + 20, 40, WIDTH - SIDEBAR - 150, note, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_draw_button_flags(s, WIDTH - 20 - 100, 16, 100, 28, "Refresh",
                         busy_pid ? VX_BUTTON_DISABLED : hot(WIDTH - 120, 16, 100, 28) ? VX_BUTTON_HOT : 0);
    vx_fill(s, SIDEBAR, HEADER - 1, WIDTH - SIDEBAR, 1, VX_COLOR_LINE);
    vx_window_present(window, 0, 0, s->width, s->height);
}

static void open_app(const struct package *p) {
    char bundle[256];
    struct vx_app app;
    snprintf(bundle, sizeof(bundle), "%s/%s", VX_APPS_DIR, p->bundle);
    if (vx_app_load(bundle, &app) == 0) {
        vx_app_open(&app, NULL);
        printf("software: opened %s\n", p->name);
        fflush(stdout);
    }
}

static void click(int x, int y) {
    if (x < SIDEBAR) {
        for (int i = 0; i < SECTIONS; i++) {
            int top = 58 + i * 30 + (i >= SECTION_INSTALLED ? 14 : 0) - 4;
            if (y >= top && y < top + 26) {
                section = i;
                scroll = 0;
                printf("software: showing %s\n", sections[i]);
                fflush(stdout);
            }
        }
        return;
    }
    if (busy_pid) {
        return;
    }
    if (vx_inside(x, y, WIDTH - 120, 16, 100, 28)) {
        start_pkg("update", NULL);
        return;
    }
    if (y < HEADER) {
        return;
    }
    int shown[MAX_PACKAGES];
    int n = visible(shown);
    for (int r = 0; r < n; r++) {
        struct package *p = &packages[shown[r]];
        int mx, rx, by;
        row_buttons(r, &mx, &rx, &by);
        if (vx_inside(x, y, mx, by, BUTTON_W, 28)) {
            if (!p->installed[0] || has_update(p)) {
                start_pkg("install", p->id);
            } else {
                open_app(p);
            }
        } else if (p->installed[0] && vx_inside(x, y, rx, by, BUTTON_W, 28)) {
            start_pkg("remove", p->id);
        }
    }
}

int main(void) {
    window = vx_window_create("Software", WIDTH, HEIGHT);
    if (!window) {
        fprintf(stderr, "software: no desktop to open a window on\n");
        return 1;
    }
    if (!load_index()) {
        start_pkg("update", NULL);
    }
    for (;;) {
        draw();
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, busy_pid ? 200 : -1);
        if (got < 0) {
            return 0;
        }
        check_pkg();
        if (!got) {
            continue;
        }
        if (e.type == VX_GUI_CLOSE) {
            if (busy_pid) {
                kill(busy_pid, SIGTERM);
            }
            vx_window_destroy(window);
            return 0;
        }
        if (e.type == VX_GUI_POINTER) {
            static int held;
            pointer_x = e.x;
            pointer_y = e.y;
            if ((e.buttons & 1) && !(held & 1)) {
                click(e.x, e.y);
            }
            held = e.buttons;
            if (e.wheel) {
                int shown[MAX_PACKAGES];
                int limit = visible(shown) * ROW - (HEIGHT - HEADER - 16);
                scroll += e.wheel * ROW / 2;
                scroll = scroll > limit ? limit : scroll;
                scroll = scroll < 0 ? 0 : scroll;
            }
        }
    }
}
