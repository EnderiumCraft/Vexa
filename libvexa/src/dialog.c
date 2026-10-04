#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

/*
 * Open and Save dialogs (see <vexa/gui.h>): a window with the places on the
 * left, the folder's contents (folders first), and for Save a name. A
 * double click opens a folder or chooses a file; Enter chooses; Backspace
 * goes up; Escape cancels. Other windows' events wait until it's done
 * (they're dropped: the app draws again afterwards anyway).
 */

#define WIDTH 620
#define HEIGHT 430
#define SIDEBAR 150
#define TOP 44
#define ROW 24
#define BOTTOM 76
#define MAX_NAMES 512

struct entry {
    char name[256];
    bool is_dir;
};

static struct dialog {
    struct vx_window *window;
    bool save;
    char folder[512];
    char name[256]; /* Save: the name field. */
    struct entry *entries;
    int count, selected, top, hot_button;
    long last_click_ms;
    int last_click;
    bool done, chosen;
    char result[768];
} d;

static const struct {
    const char *label, *path; /* (A path without a '/' is in the home folder; "" is it.) */
} places[] = {
    {"Home", ""}, {"Desktop", "Desktop"}, {"Documents", "Documents"},
    {"Pictures", "Pictures"}, {"Wallpapers", "/share/pictures"}, {"Vexa", "/"},
    {"Temporary", "/tmp"},
};

static const char *place_path(int i) {
    return places[i].path[0] == '/' ? places[i].path
           : places[i].path[0]      ? vx_home_folder(places[i].path)
                                    : vx_home();
}
#define PLACE_COUNT (int)(sizeof(places) / sizeof(places[0]))

static struct vx_image *folder_icon, *file_icon;

static int compare(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

static void read_folder(void) {
    d.count = 0;
    d.selected = -1;
    d.top = 0;
    int handle = vx_open(d.folder, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    struct vx_dir_entry entries[32];
    long n;
    while ((n = vx_read_dir(handle, entries, 32)) > 0) {
        for (long i = 0; i < n && d.count < MAX_NAMES; i++) {
            if (entries[i].name[0] == '.') {
                continue;
            }
            struct entry *e = &d.entries[d.count++];
            snprintf(e->name, sizeof(e->name), "%s", entries[i].name);
            e->is_dir = entries[i].type == VX_TYPE_DIRECTORY;
            if (entries[i].type == VX_TYPE_SYMLINK) {
                char path[800];
                struct vx_stat st;
                vx_join_path(path, sizeof(path), d.folder, e->name);
                e->is_dir = vx_stat(path, &st) == 0 && st.type == VX_TYPE_DIRECTORY;
            }
        }
    }
    vx_close(handle);
    qsort(d.entries, (size_t)d.count, sizeof(d.entries[0]), compare);
}

static void go(const char *path) {
    struct vx_stat st;
    if (vx_stat(path, &st) || st.type != VX_TYPE_DIRECTORY) {
        return;
    }
    snprintf(d.folder, sizeof(d.folder), "%s", path);
    read_folder();
}

static void up(void) {
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", d.folder);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return;
    }
    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    go(parent);
}

static int visible_rows(void) {
    return (HEIGHT - TOP - BOTTOM) / ROW;
}

static void choose_path(const char *name) {
    vx_join_path(d.result, sizeof(d.result), d.folder, name);
    d.done = d.chosen = true;
}

/* Enter, the Open/Save button, or a double click on a file. */
static void accept(void) {
    if (d.selected >= 0 && d.entries[d.selected].is_dir &&
        (!d.save || !d.name[0] || !strcmp(d.name, d.entries[d.selected].name))) {
        char path[800];
        vx_join_path(path, sizeof(path), d.folder, d.entries[d.selected].name);
        go(path);
        return;
    }
    if (d.save) {
        if (d.name[0]) {
            if (d.name[0] == '/') {
                snprintf(d.result, sizeof(d.result), "%s", d.name);
                d.done = d.chosen = true;
            } else {
                choose_path(d.name);
            }
        }
        return;
    }
    if (d.selected >= 0) {
        choose_path(d.entries[d.selected].name);
    }
}

static void select_row(int i) {
    if (i < 0 || i >= d.count) {
        return;
    }
    d.selected = i;
    if (d.save && !d.entries[i].is_dir) {
        snprintf(d.name, sizeof(d.name), "%s", d.entries[i].name);
    }
    if (i < d.top) {
        d.top = i;
    } else if (i >= d.top + visible_rows()) {
        d.top = i - visible_rows() + 1;
    }
}

/* ---- Drawing ---- */

static void button_rect(int which, int *x, int *y) {
    /* 0: Cancel, 1: Open/Save, at the bottom right. */
    *x = WIDTH - 16 - (which == 1 ? 96 : 200);
    *y = HEIGHT - 38;
}

static void draw(void) {
    struct vx_surface *s = &d.window->surface;
    vx_fill(s, 0, 0, WIDTH, HEIGHT, VX_COLOR_WINDOW);
    /* The places. */
    vx_fill(s, 0, 0, SIDEBAR, HEIGHT - BOTTOM + 30, vx_theme.sidebar);
    vx_draw_text(s, 12, 12, "Places", VX_COLOR_DIM, VX_TRANSPARENT);
    for (int i = 0; i < PLACE_COUNT; i++) {
        int y = TOP + i * ROW;
        if (!strcmp(d.folder, place_path(i))) {
            vx_draw_selection(s, 4, y, SIDEBAR - 8, ROW - 2);
        }
        if (folder_icon) {
            vx_blit_alpha(s, 12, y + 3, 16, 16, &folder_icon->surface);
        }
        vx_draw_text(s, 34, y + 3, places[i].label, VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    /* Where it is, and the Up button. */
    vx_draw_button(s, SIDEBAR + 10, 8, 28, 24, "^", d.hot_button == 2);
    vx_draw_field(s, SIDEBAR + 44, 8, WIDTH - SIDEBAR - 56, d.folder, false);
    /* The folder. */
    int list_x = SIDEBAR + 10, list_w = WIDTH - SIDEBAR - 20;
    int list_h = HEIGHT - TOP - BOTTOM;
    vx_fill_rounded(s, list_x, TOP, list_w, list_h, 6, VX_COLOR_LINE, 255);
    vx_fill_rounded(s, list_x + 1, TOP + 1, list_w - 2, list_h - 2, 5, VX_COLOR_VIEW, 255);
    for (int row = 0; row < visible_rows() && d.top + row < d.count; row++) {
        int i = d.top + row;
        int y = TOP + 1 + row * ROW;
        if (i == d.selected) {
            vx_draw_selection(s, list_x + 3, y, list_w - 6, ROW);
        }
        struct vx_image *icon = d.entries[i].is_dir ? folder_icon : file_icon;
        if (icon) {
            vx_blit_alpha(s, list_x + 8, y + 4, 16, 16, &icon->surface);
        }
        vx_draw_text_fit(s, list_x + 32, y + 4, list_w - 44, d.entries[i].name, VX_COLOR_TEXT,
                         VX_TRANSPARENT);
    }
    if (!d.count) {
        vx_draw_text(s, list_x + 12, TOP + 10, "Nothing here", VX_COLOR_DIM, VX_TRANSPARENT);
    }
    /* The name (Save), and the buttons. */
    if (d.save) {
        vx_draw_text(s, SIDEBAR + 10, HEIGHT - BOTTOM + 12, "Name:", VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_field(s, SIDEBAR + 60, HEIGHT - BOTTOM + 8, WIDTH - SIDEBAR - 72, d.name, true);
    }
    int bx, by;
    button_rect(0, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, "Cancel", d.hot_button == 0);
    button_rect(1, &bx, &by);
    vx_fill(s, bx, by, 88, 26, VX_COLOR_ACCENT);
    int w = vx_text_width(d.save ? "Save" : "Open");
    vx_draw_text(s, bx + (88 - w) / 2, by + 5, d.save ? "Save" : "Open", 0xffffff, VX_TRANSPARENT);
    vx_window_present(d.window, 0, 0, WIDTH, HEIGHT);
}

/* ---- Events ---- */

static void key(const struct vx_gui_event *e) {
    if (!e->value) {
        return;
    }
    switch (e->key) {
    case VX_KEY_ESC: d.done = true; return;
    case VX_KEY_ENTER: accept(); return;
    case VX_KEY_UP: select_row(d.selected > 0 ? d.selected - 1 : 0); return;
    case VX_KEY_DOWN: select_row(d.selected + 1 < d.count ? d.selected + 1 : d.selected); return;
    case VX_KEY_BACKSPACE:
        if (!d.save || !d.name[0]) {
            up();
            return;
        }
        break;
    }
    if (d.save) {
        vx_field_key(d.name, sizeof(d.name), e);
        return;
    }
    /* Open: typing goes to the first name that starts with it. */
    if (e->character > ' ' && e->character < 127) {
        for (int i = 0; i < d.count; i++) {
            char c = d.entries[i].name[0];
            if ((c | 0x20) == (e->character | 0x20)) {
                select_row(i);
                return;
            }
        }
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    int bx, by;
    d.hot_button = -1;
    for (int b = 0; b < 2; b++) {
        button_rect(b, &bx, &by);
        if (vx_inside(e->x, e->y, bx, by, 88, 26)) {
            d.hot_button = b;
        }
    }
    if (vx_inside(e->x, e->y, SIDEBAR + 10, 8, 28, 24)) {
        d.hot_button = 2;
    }
    if (e->wheel) {
        d.top -= e->wheel * 3;
        int max = d.count - visible_rows();
        d.top = d.top > max ? max : d.top;
        d.top = d.top < 0 ? 0 : d.top;
    }
    if (!click) {
        return;
    }
    if (d.hot_button == 0) {
        d.done = true;
        return;
    }
    if (d.hot_button == 1) {
        accept();
        return;
    }
    if (d.hot_button == 2) {
        up();
        return;
    }
    if (e->x < SIDEBAR) {
        int i = (e->y - TOP) / ROW;
        if (e->y >= TOP && i >= 0 && i < PLACE_COUNT) {
            go(place_path(i));
        }
        return;
    }
    int list_x = SIDEBAR + 10, list_w = WIDTH - SIDEBAR - 20;
    if (vx_inside(e->x, e->y, list_x, TOP, list_w, HEIGHT - TOP - BOTTOM)) {
        int i = d.top + (e->y - TOP - 1) / ROW;
        if (i >= d.count) {
            d.selected = -1;
            return;
        }
        long now = vx_uptime();
        if (i == d.last_click && now - d.last_click_ms < 500) {
            d.last_click = -1;
            select_row(i);
            if (d.entries[i].is_dir) {
                char path[800];
                vx_join_path(path, sizeof(path), d.folder, d.entries[i].name);
                go(path);
            } else {
                choose_path(d.entries[i].name);
            }
            return;
        }
        d.last_click = i;
        d.last_click_ms = now;
        select_row(i);
    }
}

static bool run(bool save, const char *title, const char *folder, const char *name, char *out,
                size_t size) {
    memset(&d, 0, sizeof(d));
    d.save = save;
    d.last_click = -1;
    d.hot_button = -1;
    d.entries = malloc(MAX_NAMES * sizeof(struct entry));
    if (!d.entries) {
        return false;
    }
    if (!folder_icon) {
        folder_icon = vx_image_load("/apps/Files.vxapp/Contents/Resources/folder.png", VX_IMAGE_ALPHA);
        file_icon = vx_image_load("/apps/Files.vxapp/Contents/Resources/document.png", VX_IMAGE_ALPHA);
    }
    if (name) {
        snprintf(d.name, sizeof(d.name), "%s", name);
    }
    snprintf(d.folder, sizeof(d.folder), "%s", folder && folder[0] ? folder : vx_home());
    struct vx_stat st;
    if (vx_stat(d.folder, &st) || st.type != VX_TYPE_DIRECTORY) {
        snprintf(d.folder, sizeof(d.folder), "%s", vx_home());
    }
    read_folder();
    d.window = vx_window_create(title ? title : save ? "Save" : "Open", WIDTH, HEIGHT);
    if (!d.window) {
        free(d.entries);
        return false;
    }
    int held = 0;
    while (!d.done) {
        draw();
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            break;
        }
        if (e.window != d.window->id) {
            continue; /* Another window's: it waits (and is lost). */
        }
        switch (e.type) {
        case VX_GUI_CLOSE: d.done = true; break;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        }
    }
    vx_window_destroy(d.window);
    free(d.entries);
    if (d.chosen) {
        snprintf(out, size, "%s", d.result);
        printf("dialog: chose %s\n", d.result);
        fflush(stdout);
    }
    return d.chosen;
}

bool vx_open_dialog(const char *title, const char *folder, char *out, size_t size) {
    return run(false, title, folder, NULL, out, size);
}

bool vx_save_dialog(const char *title, const char *folder, const char *name, char *out,
                    size_t size) {
    return run(true, title, folder, name, out, size);
}
