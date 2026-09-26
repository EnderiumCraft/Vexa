/* files: a file manager window. It lists a directory (folders first); a
 * double click (or Enter) opens a folder, starts an app (a .vxapp bundle,
 * shown with its icon, like a file), runs a program from /bin, and opens
 * anything else with the app for its type (<vexa/app.h>).
 * Up (or Backspace) goes to the parent folder; the path at the top can be
 * typed into.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 620
#define HEIGHT 440
#define TOOLBAR 36
#define STATUS 22
#define ROW 20
#define MAX_ENTRIES 1024
#define DOUBLE_CLICK_MS 500

struct item {
    char name[256];
    uint32_t type;
    uint64_t size;
    bool app;                /* A .vxapp bundle. */
    struct vx_image *icon;   /* An app's icon. */
};

static struct vx_window *window;
static char cwd[512] = "/";
static char typed[512];
static bool typing; /* The path field has the keyboard. */
static struct item items[MAX_ENTRIES];
static int item_count, selected = -1, top;
static int hot_button = -1; /* 0 Up, 1 Home */
static long last_click_ms;
static int last_click;
static char status[128];

static int list_rows(void) {
    return (window->surface.height - TOOLBAR - STATUS - 8) / ROW;
}

static int compare(const void *a, const void *b) {
    const struct item *x = a, *y = b;
    bool dx = x->type == VX_TYPE_DIRECTORY && !x->app, dy = y->type == VX_TYPE_DIRECTORY && !y->app;
    if (dx != dy) {
        return dx ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

static void join(char *out, size_t size, const char *dir, const char *name) {
    snprintf(out, size, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

static void load(void) {
    for (int i = 0; i < item_count; i++) {
        vx_image_free(items[i].icon);
    }
    item_count = 0;
    selected = -1;
    top = 0;
    int handle = vx_open(cwd, VX_OPEN_READ);
    if (handle < 0) {
        snprintf(status, sizeof(status), "Can't open %s: %s", cwd, vx_strerror(handle));
        return;
    }
    struct vx_dir_entry entries[32];
    long n;
    while ((n = vx_read_dir(handle, entries, 32)) > 0) {
        for (long i = 0; i < n && item_count < MAX_ENTRIES; i++) {
            if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) {
                continue;
            }
            struct item *item = &items[item_count++];
            strncpy(item->name, entries[i].name, sizeof(item->name) - 1);
            item->name[sizeof(item->name) - 1] = '\0';
            item->type = entries[i].type;
            item->size = 0;
            item->app = false;
            item->icon = NULL;
            char path[800];
            join(path, sizeof(path), cwd, item->name);
            struct vx_stat stat;
            if (vx_stat(path, &stat) == 0) {
                item->size = stat.size;
                if (item->type == VX_TYPE_SYMLINK && stat.type == VX_TYPE_DIRECTORY) {
                    item->type = VX_TYPE_DIRECTORY; /* A link to a folder opens like one. */
                }
            }
            struct vx_app app;
            if (item->type == VX_TYPE_DIRECTORY && vx_app_is_bundle(item->name) &&
                vx_app_load(path, &app) == 0) {
                item->app = true;
                item->icon = app.icon[0] ? vx_image_load(app.icon, VX_IMAGE_ALPHA) : NULL;
            }
        }
    }
    vx_close(handle);
    qsort(items, item_count, sizeof(items[0]), compare);
    snprintf(status, sizeof(status), "%d item%s", item_count, item_count == 1 ? "" : "s");
    char title[600];
    snprintf(title, sizeof(title), "%s - Files", cwd);
    vx_window_set_title(window, title);
}

static void go(const char *path) {
    struct vx_stat stat;
    if (vx_stat(path, &stat) || stat.type != VX_TYPE_DIRECTORY) {
        snprintf(status, sizeof(status), "Not a folder: %s", path);
        return;
    }
    char resolved[512];
    long saved = vx_getcwd(resolved, sizeof(resolved));
    (void)saved;
    if (vx_chdir(path) == 0 && vx_getcwd(resolved, sizeof(resolved)) >= 0) {
        strncpy(cwd, resolved, sizeof(cwd) - 1);
    } else {
        strncpy(cwd, path, sizeof(cwd) - 1);
    }
    load();
}

static void up(void) {
    if (!strcmp(cwd, "/")) {
        return;
    }
    char parent[512];
    strcpy(parent, cwd);
    char *slash = strrchr(parent, '/');
    if (slash == parent) {
        parent[1] = '\0';
    } else if (slash) {
        *slash = '\0';
    }
    go(parent);
}

static void start(int process, const char *what) {
    if (process < 0) {
        snprintf(status, sizeof(status), "Can't start %s: %s", what, vx_strerror(process));
    } else {
        vx_close(process); /* It runs on its own. */
    }
}

static void run(const char *program, const char *argument) {
    const char *argv[] = {program, argument};
    unsigned long envc = 0;
    while (environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {
        .argv = argv, .argc = argument ? 2 : 1, .envp = (const char *const *)environ,
        .envc = envc, .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
    };
    start(vx_spawn(program, &spawn), program);
}

static void open_item(int index) {
    if (index < 0 || index >= item_count) {
        return;
    }
    struct item *item = &items[index];
    char path[800];
    join(path, sizeof(path), cwd, item->name);
    struct vx_app app;
    if (item->app && vx_app_load(path, &app) == 0) {
        start(vx_app_open(&app, NULL), app.name);
    } else if (item->type == VX_TYPE_DIRECTORY) {
        go(path);
    } else if (!strcmp(cwd, "/bin")) {
        run(path, NULL);
    } else if (vx_app_for_file(path, &app) == 0) {
        start(vx_app_open(&app, path), app.name);
    } else {
        snprintf(status, sizeof(status), "No app opens %s", item->name);
    }
}

/* ---- Drawing ---- */

static void draw_icon(struct vx_surface *s, int x, int y, uint32_t type) {
    if (type == VX_TYPE_DIRECTORY) { /* A folder. */
        vx_fill(s, x, y + 3, 6, 2, 0xf2cc60);
        vx_fill(s, x, y + 5, 14, 9, 0xf2cc60);
        vx_fill(s, x + 1, y + 7, 12, 6, 0xd9ad3c);
    } else if (type == VX_TYPE_SYMLINK) { /* An arrow. */
        vx_fill(s, x + 2, y + 8, 9, 2, 0x56d4dd);
        vx_fill(s, x + 9, y + 5, 2, 8, 0x56d4dd);
        vx_fill(s, x + 11, y + 7, 2, 4, 0x56d4dd);
    } else if (type == VX_TYPE_CHAR_DEVICE || type == VX_TYPE_BLOCK_DEVICE) { /* A chip. */
        vx_fill(s, x + 2, y + 4, 10, 10, 0x79a8ff);
        for (int i = 0; i < 3; i++) {
            vx_fill(s, x, y + 5 + i * 3, 2, 1, 0x79a8ff);
            vx_fill(s, x + 12, y + 5 + i * 3, 2, 1, 0x79a8ff);
        }
    } else { /* A page. */
        vx_fill(s, x + 2, y + 2, 10, 13, 0xe4dcf2);
        vx_fill(s, x + 4, y + 6, 6, 1, 0x8a80a3);
        vx_fill(s, x + 4, y + 9, 6, 1, 0x8a80a3);
        vx_fill(s, x + 4, y + 12, 4, 1, 0x8a80a3);
    }
}

static void format_size(char *out, size_t size, const struct item *item) {
    if (item->app) {
        snprintf(out, size, "app");
    } else if (item->type == VX_TYPE_DIRECTORY) {
        snprintf(out, size, "folder");
    } else if (item->size < 1024) {
        snprintf(out, size, "%lu B", (unsigned long)item->size);
    } else if (item->size < 1024 * 1024) {
        snprintf(out, size, "%lu KiB", (unsigned long)(item->size / 1024));
    } else {
        snprintf(out, size, "%lu MiB", (unsigned long)(item->size / (1024 * 1024)));
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_WINDOW);
    /* The toolbar: Up, Home, the path. */
    vx_draw_button(s, 8, 6, 44, 24, "Up", hot_button == 0);
    vx_draw_button(s, 58, 6, 52, 24, "Home", hot_button == 1);
    vx_draw_field(s, 118, 6, w - 126, typing ? typed : cwd, typing);
    /* The list. */
    int list_y = TOOLBAR + 2, rows = list_rows();
    vx_fill(s, 8, list_y, w - 16, rows * ROW + 4, VX_COLOR_VIEW);
    for (int i = 0; i < rows && top + i < item_count; i++) {
        struct item *item = &items[top + i];
        int y = list_y + 2 + i * ROW;
        if (top + i == selected) {
            vx_fill(s, 10, y, w - 20, ROW, VX_COLOR_SELECTED);
        }
        if (item->icon) {
            vx_blit_alpha(s, 15, y + 1, 18, 18, &item->icon->surface);
        } else {
            draw_icon(s, 16, y + 2, item->type);
        }
        char size[32], name[256];
        format_size(size, sizeof(size), item);
        /* Apps without ".vxapp", as Finder shows them. */
        snprintf(name, sizeof(name), "%s", item->name);
        if (item->app) {
            name[strlen(name) - strlen(VX_APP_EXTENSION)] = '\0';
        }
        int size_x = w - 24 - (int)strlen(size) * FONT_WIDTH;
        vx_draw_text_fit(s, 38, y + 2, size_x - 48, name, VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_text(s, size_x, y + 2, size, VX_COLOR_DIM, VX_TRANSPARENT);
    }
    if (item_count > rows) { /* Where we are in a long list. */
        int bar = (rows * ROW) * rows / item_count;
        int at = (rows * ROW - bar) * top / (item_count - rows);
        vx_fill(s, w - 13, list_y + 2 + at, 3, bar, VX_COLOR_LINE);
    }
    vx_draw_text_fit(s, 10, h - STATUS + 3, w - 20, status, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_window_present(window, 0, 0, w, h);
}

static void select_item(int index) {
    if (item_count == 0) {
        return;
    }
    selected = index < 0 ? 0 : index >= item_count ? item_count - 1 : index;
    if (selected < top) {
        top = selected;
    } else if (selected >= top + list_rows()) {
        top = selected - list_rows() + 1;
    }
}

static void scroll(int by) {
    int max = item_count - list_rows();
    top += by;
    top = top > max ? max : top;
    top = top < 0 ? 0 : top;
}

/* ---- Events ---- */

static void key(const struct vx_gui_event *e) {
    if (e->value == 0) {
        return;
    }
    if (typing) {
        if (e->key == VX_KEY_ENTER) {
            typing = false;
            go(typed);
        } else if (e->key == VX_KEY_ESC) {
            typing = false;
        } else {
            vx_field_key(typed, sizeof(typed), e);
        }
        return;
    }
    switch (e->key) {
    case VX_KEY_UP: select_item(selected - 1); break;
    case VX_KEY_DOWN: select_item(selected + 1); break;
    case VX_KEY_PAGEUP: select_item(selected - list_rows()); break;
    case VX_KEY_PAGEDOWN: select_item(selected + list_rows()); break;
    case VX_KEY_HOME: select_item(0); break;
    case VX_KEY_END: select_item(item_count - 1); break;
    case VX_KEY_ENTER: open_item(selected); break;
    case VX_KEY_BACKSPACE: up(); break;
    }
}

static void pointer(const struct vx_gui_event *e, int *buttons) {
    int w = window->surface.width;
    bool click = (e->buttons & 1) && !(*buttons & 1);
    *buttons = e->buttons;
    hot_button = vx_inside(e->x, e->y, 8, 6, 44, 24) ? 0 : vx_inside(e->x, e->y, 58, 6, 52, 24) ? 1 : -1;
    if (e->wheel) {
        scroll(-e->wheel * 3);
    }
    if (!click) {
        return;
    }
    if (hot_button == 0) {
        up();
        return;
    }
    if (hot_button == 1) {
        go("/");
        return;
    }
    if (vx_inside(e->x, e->y, 118, 6, w - 126, FONT_HEIGHT + 8)) {
        typing = true;
        strcpy(typed, cwd);
        return;
    }
    typing = false;
    int row = (e->y - TOOLBAR - 4) / ROW;
    if (e->y >= TOOLBAR + 4 && row < list_rows() && top + row < item_count) {
        int index = top + row;
        long now = vx_uptime();
        if (index == last_click && now - last_click_ms < DOUBLE_CLICK_MS) {
            open_item(index);
            last_click = -1;
            return;
        }
        selected = index;
        last_click = index;
        last_click_ms = now;
    }
}

int main(int argc, char **argv) {
    window = vx_window_create_flags("Files", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "files: no desktop to open a window on\n");
        return 1;
    }
    go(argc > 1 ? argv[1] : "/");
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
            if (e.width >= 240 && e.height >= 160) {
                vx_window_resize(window, e.width, e.height);
                scroll(0);
            }
            break;
        }
    }
}
