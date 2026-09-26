/* files: a file manager window, in the manner of macOS's Finder.
 *
 * It lists a folder (folders first). A double click (or Enter) opens a
 * folder, starts an app (a .vxapp bundle, shown with its icon, like a
 * file), runs a program from /bin, and opens anything else with the app for
 * its type (<vexa/app.h>). A right click opens a menu: Open, Show Package
 * Contents, Get Info, Rename, Copy, Cut, Paste, Move to Trash; on the empty
 * part of the list, New Folder, Paste, Open in Terminal and, in the Trash,
 * Empty Trash. The keyboard does the same: Ctrl+C, Ctrl+X, Ctrl+V,
 * Ctrl+I, F2, Delete, Ctrl+Shift+N. Up (or Backspace) goes to the parent
 * folder; the path at the top can be typed into (Ctrl+L). Names starting
 * with a dot are hidden, as in Finder, until Ctrl+H.
 *
 * The clipboard is a file (CLIPBOARD) that every Files window shares.
 * Deleted things go to /Trash first. Copying a .vxapp into /apps installs
 * an app: the desktop notices it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/files.h>
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
#define TRASH "/Trash"
#define CLIPBOARD "/tmp/.files-clipboard"
#define PATH_X 214 /* The path field, after the buttons. */

struct item {
    char name[256];
    uint32_t type;
    uint64_t size;
    bool app;              /* A .vxapp bundle. */
    struct vx_image *icon; /* An app's icon. */
};

static struct vx_window *window;
static char cwd[512] = "/";
static char typed[512];
static bool typing; /* The path field has the keyboard. */
static struct item items[MAX_ENTRIES];
static int item_count, selected = -1, top;
static int hot_button = -1;
static long last_click_ms;
static int last_click;
static char status[160];
static bool ctrl, shift;
static bool show_hidden; /* Names starting with a dot. */

/* The toolbar's buttons. */
static const struct {
    const char *label;
    int x, width;
} buttons[] = {{"Up", 8, 36}, {"Home", 48, 48}, {"Apps", 100, 48}, {"Trash", 152, 56}};
#define BUTTONS (int)(sizeof(buttons) / sizeof(buttons[0]))

/* What has the keyboard and pointer besides the list: a menu, renaming,
 * a question, or an item's information. */
static enum { NORMAL, MENU, RENAMING, CONFIRM, INFO } mode;

/* ---- The folder ---- */

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

static void item_path(char *out, size_t size, int index) {
    vx_join_path(out, size, cwd, items[index].name);
}

static void set_status(const char *text) {
    snprintf(status, sizeof(status), "%s", text);
    printf("files: %s\n", text); /* (The desktop's output: a log.) */
    fflush(stdout);
}

static void select_item(int index);

/* Reads the folder again, selecting `name` if it's given. */
static void load(const char *name) {
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
            if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..") ||
                (entries[i].name[0] == '.' && !show_hidden)) {
                continue;
            }
            struct item *item = &items[item_count++];
            snprintf(item->name, sizeof(item->name), "%s", entries[i].name);
            item->type = entries[i].type;
            item->size = 0;
            item->app = false;
            item->icon = NULL;
            char path[800];
            vx_join_path(path, sizeof(path), cwd, item->name);
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
    for (int i = 0; name && i < item_count; i++) {
        if (!strcmp(items[i].name, name)) {
            select_item(i);
        }
    }
}

static void go(const char *path) {
    struct vx_stat stat;
    if (vx_stat(path, &stat) || stat.type != VX_TYPE_DIRECTORY) {
        snprintf(status, sizeof(status), "Not a folder: %s", path);
        return;
    }
    char resolved[512];
    if (vx_chdir(path) == 0 && vx_getcwd(resolved, sizeof(resolved)) >= 0) {
        snprintf(cwd, sizeof(cwd), "%s", resolved);
    } else {
        snprintf(cwd, sizeof(cwd), "%s", path);
    }
    load(NULL);
}

static void up(void) {
    if (!strcmp(cwd, "/")) {
        return;
    }
    char parent[512], *slash;
    snprintf(parent, sizeof(parent), "%s", cwd);
    slash = strrchr(parent, '/');
    if (slash == parent) {
        parent[1] = '\0';
    } else if (slash) {
        *slash = '\0';
    }
    go(parent);
}

/* ---- Opening ---- */

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
    item_path(path, sizeof(path), index);
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

static void open_with(int index, const char *app_name) {
    struct vx_app app;
    char path[800];
    item_path(path, sizeof(path), index);
    if (vx_app_find(app_name, &app) == 0) {
        start(vx_app_open(&app, path), app.name);
    }
}

/* ---- The clipboard: "copy" or "cut", then a path. ---- */

static bool read_clipboard(char *path, size_t size, bool *cut) {
    int handle = vx_open(CLIPBOARD, VX_OPEN_READ);
    if (handle < 0) {
        return false;
    }
    char text[600];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    char *newline = strchr(text, '\n');
    if (!newline || !newline[1]) {
        return false;
    }
    *newline = '\0';
    *cut = !strcmp(text, "cut");
    snprintf(path, size, "%s", newline + 1);
    return vx_lstat(path, &(struct vx_stat){0}) == 0;
}

static void write_clipboard(const char *verb, const char *path) {
    int handle = vx_open(CLIPBOARD, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle >= 0) {
        char text[600];
        int n = snprintf(text, sizeof(text), "%s\n%s", verb, path);
        vx_write(handle, text, (size_t)n);
        vx_close(handle);
    }
}

static void copy_item(int index, bool cut) {
    if (index < 0) {
        return;
    }
    char path[800], message[900];
    item_path(path, sizeof(path), index);
    write_clipboard(cut ? "cut" : "copy", path);
    snprintf(message, sizeof(message), "%s %s", cut ? "cut" : "copied", path);
    set_status(message);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static void paste(void) {
    char from[512], name[256], to[800], message[1400];
    bool cut;
    if (!read_clipboard(from, sizeof(from), &cut)) {
        set_status("Nothing to paste");
        return;
    }
    vx_unique_name(cwd, base_name(from), name, sizeof(name));
    vx_join_path(to, sizeof(to), cwd, name);
    long error = cut ? vx_move(from, to) : vx_copy_tree(from, to);
    if (error) {
        snprintf(message, sizeof(message), "Can't paste %s: %s", from, vx_strerror(error));
        set_status(message);
        return;
    }
    if (cut) {
        vx_remove(CLIPBOARD); /* It's moved; there's nothing to paste again. */
    }
    load(name);
    snprintf(message, sizeof(message), "%s %s to %s", cut ? "moved" : "pasted", from, to);
    set_status(message);
}

static bool in_trash(void) {
    return !strcmp(cwd, TRASH);
}

static void trash_item(int index) {
    if (index < 0) {
        return;
    }
    char path[800], name[256], to[800], message[1700];
    item_path(path, sizeof(path), index);
    vx_mkdir(TRASH);
    vx_unique_name(TRASH, items[index].name, name, sizeof(name));
    vx_join_path(to, sizeof(to), TRASH, name);
    long error = vx_move(path, to);
    if (error) {
        snprintf(message, sizeof(message), "Can't move %s to the Trash: %s", path, vx_strerror(error));
        set_status(message);
        return;
    }
    load(NULL);
    select_item(index);
    snprintf(message, sizeof(message), "moved %s to the Trash", path);
    set_status(message);
}

static void delete_item(int index) {
    char path[800], message[900];
    item_path(path, sizeof(path), index);
    long error = vx_remove_tree(path);
    snprintf(message, sizeof(message), error ? "Can't delete %s: %s" : "deleted %s", path,
             error ? vx_strerror(error) : "");
    load(NULL);
    set_status(message);
}

static void empty_trash(void) {
    long error = vx_remove_tree(TRASH);
    vx_mkdir(TRASH);
    if (in_trash()) {
        load(NULL);
    }
    set_status(error ? "Can't empty the Trash" : "emptied the Trash");
}

/* ---- Renaming (and naming a new folder) ---- */

static char new_name[256];
static int renaming = -1;

static void start_rename(int index) {
    if (index < 0) {
        return;
    }
    select_item(index);
    renaming = index;
    snprintf(new_name, sizeof(new_name), "%s", items[index].name);
    mode = RENAMING;
}

static void finish_rename(void) {
    mode = NORMAL;
    if (renaming < 0 || !new_name[0] || !strcmp(new_name, items[renaming].name)) {
        return;
    }
    if (strchr(new_name, '/')) {
        set_status("A name can't have a / in it");
        return;
    }
    char from[800], to[800], message[1700];
    item_path(from, sizeof(from), renaming);
    vx_join_path(to, sizeof(to), cwd, new_name);
    long error = vx_move(from, to);
    if (error) {
        snprintf(message, sizeof(message), "Can't rename to %s: %s", new_name, vx_strerror(error));
        set_status(message);
        return;
    }
    char keep[256];
    snprintf(keep, sizeof(keep), "%s", new_name);
    load(keep);
    snprintf(message, sizeof(message), "renamed %s to %s", from, to);
    set_status(message);
}

static void new_folder(void) {
    char name[256], path[800], message[900];
    vx_unique_name(cwd, "untitled folder", name, sizeof(name));
    vx_join_path(path, sizeof(path), cwd, name);
    long error = vx_mkdir(path);
    if (error) {
        snprintf(message, sizeof(message), "Can't make a folder: %s", vx_strerror(error));
        set_status(message);
        return;
    }
    load(name);
    snprintf(message, sizeof(message), "new folder %s", path);
    set_status(message);
    start_rename(selected); /* Name it right away, as Finder does. */
}

/* ---- A question: emptying the Trash, or deleting from it ---- */

static enum { ASK_EMPTY_TRASH, ASK_DELETE } question;

static void ask(int what) {
    question = what;
    mode = CONFIRM;
}

/* ---- Get Info ---- */

static int info_item = -1; /* -1: the folder itself. */
#define INFO_LINES 9
static char info_labels[INFO_LINES][16], info_values[INFO_LINES][200];
static int info_count;
static struct vx_image *info_icon;
static int info_lines(char labels[INFO_LINES][16], char values[INFO_LINES][200],
                      struct vx_image **icon);

static void show_info(int index) {
    info_item = index;
    /* Worked out once: a folder's size means reading all of it. */
    info_count = info_lines(info_labels, info_values, &info_icon);
    mode = INFO;
    printf("files: info for %s: %s, %s\n", info_values[0], info_values[1], info_values[2]);
    fflush(stdout);
}

/* "2026-09-26 10:54" (UTC) from seconds since 1970. */
static void format_date(char *out, size_t size, long long seconds) {
    long long days = seconds / 86400, rest = seconds % 86400;
    /* Days to a civil date (Howard Hinnant's algorithm). */
    long long z = days + 719468, era = z / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, size, "%04lld-%02lld-%02lld %02lld:%02lld UTC", y + (m <= 2), m, d, rest / 3600,
             rest / 60 % 60);
}

static void format_bytes(char *out, size_t size, unsigned long long bytes) {
    if (bytes < 1024) {
        snprintf(out, size, "%llu bytes", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, size, "%llu KiB (%llu bytes)", bytes / 1024, bytes);
    } else {
        snprintf(out, size, "%llu.%llu MiB (%llu bytes)", bytes / (1024 * 1024),
                 bytes * 10 / (1024 * 1024) % 10, bytes);
    }
}

static const char *kind_of(const char *path, const struct vx_stat *stat, bool app) {
    if (app) {
        return "App";
    }
    if (stat->type == VX_TYPE_DIRECTORY) {
        return "Folder";
    }
    if (stat->type == VX_TYPE_SYMLINK) {
        return "Link";
    }
    if (stat->type == VX_TYPE_CHAR_DEVICE || stat->type == VX_TYPE_BLOCK_DEVICE) {
        return "Device";
    }
    if (stat->type == VX_TYPE_SOCKET) {
        return "Socket";
    }
    const char *dot = strrchr(base_name(path), '.');
    if (dot && (!strcmp(dot, ".png") || !strcmp(dot, ".bmp") || !strcmp(dot, ".ppm"))) {
        return "Image";
    }
    if (dot && (!strcmp(dot, ".txt") || !strcmp(dot, ".conf") || !strcmp(dot, ".md"))) {
        return "Text";
    }
    return "File";
}

/* The lines of the Get Info panel: labels and values. */
static int info_lines(char labels[INFO_LINES][16], char values[INFO_LINES][200], struct vx_image **icon) {
    char path[800];
    if (info_item >= 0) {
        item_path(path, sizeof(path), info_item);
    } else {
        snprintf(path, sizeof(path), "%s", cwd);
    }
    struct vx_stat stat = {0}, target = {0};
    vx_lstat(path, &stat);
    vx_stat(path, &target);
    bool app = info_item >= 0 ? items[info_item].app : vx_app_is_bundle(path);
    *icon = info_item >= 0 ? items[info_item].icon : NULL;
    int n = 0;
#define LINE(label, ...) \
    (snprintf(labels[n], 16, "%s", label), snprintf(values[n], 200, __VA_ARGS__), n++)
    LINE("Name:", "%s", base_name(path));
    LINE("Kind:", "%s", kind_of(path, &stat, app));
    if (stat.type == VX_TYPE_DIRECTORY || (stat.type == VX_TYPE_SYMLINK && target.type == VX_TYPE_DIRECTORY)) {
        long files = 0;
        char bytes[64];
        format_bytes(bytes, sizeof(bytes), vx_tree_size(path, &files));
        LINE("Size:", "%s in %ld file%s", bytes, files, files == 1 ? "" : "s");
    } else {
        char bytes[64];
        format_bytes(bytes, sizeof(bytes), target.size);
        LINE("Size:", "%s", bytes);
    }
    char where[512];
    snprintf(where, sizeof(where), "%s", path);
    char *slash = strrchr(where, '/');
    if (slash) {
        slash == where ? (void)(where[1] = '\0') : (void)(*slash = '\0');
    }
    LINE("Where:", "%s", where);
    if (stat.modified) {
        char date[64];
        format_date(date, sizeof(date), stat.modified);
        LINE("Modified:", "%s", date);
    }
    if (stat.type == VX_TYPE_SYMLINK) {
        char to[200];
        long got = vx_readlink(path, to, sizeof(to) - 1);
        to[got > 0 ? got : 0] = '\0';
        LINE("Points to:", "%s", to);
    }
    struct vx_app a;
    if (app && vx_app_load(path, &a) == 0) {
        LINE("App name:", "%s", a.name);
        LINE("Program:", "%s", a.executable);
        LINE("Opens:", "%s", a.opens[0] ? a.opens : "(no files)");
    } else if (stat.type == VX_TYPE_FILE && vx_app_for_file(path, &a) == 0) {
        LINE("Opens with:", "%s", a.name);
    }
#undef LINE
    return n;
}

/* ---- The context menu ---- */

enum action {
    ACT_OPEN, ACT_SHOW_CONTENTS, ACT_OPEN_IN_EDITOR, ACT_INFO, ACT_RENAME, ACT_COPY, ACT_CUT,
    ACT_PASTE, ACT_TRASH, ACT_DELETE, ACT_NEW_FOLDER, ACT_TERMINAL, ACT_EMPTY_TRASH, ACT_NONE
};

#define MAX_MENU 16
static struct vx_menu_item menu[MAX_MENU];
static enum action menu_actions[MAX_MENU];
static int menu_count, menu_x, menu_y, menu_hot = -1;

static void add(const char *label, const char *keys, enum action action, bool disabled) {
    menu[menu_count] = (struct vx_menu_item){label, keys, disabled};
    menu_actions[menu_count++] = action;
}

static void open_menu(int x, int y) {
    char clip[512];
    bool cut, can_paste = read_clipboard(clip, sizeof(clip), &cut);
    menu_count = 0;
    if (selected >= 0) {
        struct item *item = &items[selected];
        add("Open", "Enter", ACT_OPEN, false);
        if (item->app) {
            add("Show Package Contents", NULL, ACT_SHOW_CONTENTS, false);
        } else if (item->type != VX_TYPE_DIRECTORY) {
            add("Open in Text Editor", NULL, ACT_OPEN_IN_EDITOR, false);
        }
        add(NULL, NULL, ACT_NONE, false);
        add("Get Info", "Ctrl+I", ACT_INFO, false);
        add("Rename", "F2", ACT_RENAME, false);
        add(NULL, NULL, ACT_NONE, false);
        add("Copy", "Ctrl+C", ACT_COPY, false);
        add("Cut", "Ctrl+X", ACT_CUT, false);
        add("Paste", "Ctrl+V", ACT_PASTE, !can_paste);
        add(NULL, NULL, ACT_NONE, false);
        if (in_trash()) {
            add("Delete Immediately", "Delete", ACT_DELETE, false);
        } else {
            add("Move to Trash", "Delete", ACT_TRASH, false);
        }
    } else {
        add("New Folder", "Ctrl+Shift+N", ACT_NEW_FOLDER, false);
        add("Paste", "Ctrl+V", ACT_PASTE, !can_paste);
        add(NULL, NULL, ACT_NONE, false);
        add("Get Info", "Ctrl+I", ACT_INFO, false);
        add("Open in Terminal", NULL, ACT_TERMINAL, false);
        if (in_trash()) {
            add(NULL, NULL, ACT_NONE, false);
            add("Empty Trash", NULL, ACT_EMPTY_TRASH, item_count == 0);
        }
    }
    /* Inside the window. */
    int w, h;
    vx_menu_size(menu, menu_count, &w, &h);
    menu_x = x + w > window->surface.width - 4 ? window->surface.width - w - 4 : x;
    menu_y = y + h > window->surface.height - 4 ? window->surface.height - h - 4 : y;
    menu_x = menu_x < 0 ? 0 : menu_x;
    menu_y = menu_y < 0 ? 0 : menu_y;
    menu_hot = -1;
    mode = MENU;
    printf("files: menu for %s\n", selected >= 0 ? items[selected].name : cwd);
    fflush(stdout);
}

static void act(enum action action) {
    mode = NORMAL;
    switch (action) {
    case ACT_OPEN: open_item(selected); break;
    case ACT_SHOW_CONTENTS: {
        char path[800];
        item_path(path, sizeof(path), selected);
        go(path);
        break;
    }
    case ACT_OPEN_IN_EDITOR: open_with(selected, "Editor"); break;
    case ACT_INFO: show_info(selected); break;
    case ACT_RENAME: start_rename(selected); break;
    case ACT_COPY: copy_item(selected, false); break;
    case ACT_CUT: copy_item(selected, true); break;
    case ACT_PASTE: paste(); break;
    case ACT_TRASH: trash_item(selected); break;
    case ACT_DELETE: ask(ASK_DELETE); break;
    case ACT_NEW_FOLDER: new_folder(); break;
    case ACT_TERMINAL: run("/bin/term", NULL); break; /* It starts here: our folder. */
    case ACT_EMPTY_TRASH: ask(ASK_EMPTY_TRASH); break;
    case ACT_NONE: break;
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

/* A panel in the middle of the window (dialogs). */
static void panel(int width, int height, int *x, int *y) {
    struct vx_surface *s = &window->surface;
    *x = (s->width - width) / 2;
    *y = (s->height - height) / 2;
    *y = *y < TOOLBAR ? TOOLBAR : *y;
    vx_fill(s, *x + 4, *y + 4, width, height, 0x06030c);
    vx_fill(s, *x, *y, width, height, VX_COLOR_WINDOW);
    vx_draw_outline(s, *x, *y, width, height, VX_COLOR_ACCENT);
}

/* Where a dialog's buttons are: right-aligned at the bottom. */
static void dialog_button(int px, int py, int pw, int ph, int i, int *x, int *y) {
    *x = px + pw - 16 - (i + 1) * 96 + 8;
    *y = py + ph - 38;
}

static void draw_confirm(void) {
    struct vx_surface *s = &window->surface;
    int x, y, bx, by, w = 380, h = 120;
    panel(w, h, &x, &y);
    char text[300];
    if (question == ASK_EMPTY_TRASH) {
        snprintf(text, sizeof(text), "Empty the Trash? Its %d item%s go for good.", item_count,
                 item_count == 1 ? "" : "s");
    } else {
        snprintf(text, sizeof(text), "Delete \"%s\" for good?", items[selected].name);
    }
    vx_draw_text_fit(s, x + 16, y + 20, w - 32, text, VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_text(s, x + 16, y + 44, "This can't be undone.", VX_COLOR_DIM, VX_TRANSPARENT);
    dialog_button(x, y, w, h, 0, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, question == ASK_EMPTY_TRASH ? "Empty" : "Delete", false);
    dialog_button(x, y, w, h, 1, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, "Cancel", false);
}

static void draw_info(void) {
    struct vx_surface *s = &window->surface;
    char (*labels)[16] = info_labels, (*values)[200] = info_values;
    struct vx_image *icon = info_icon;
    int n = info_count;
    int x, y, bx, by, w = 440, h = 96 + n * (FONT_HEIGHT + 4);
    panel(w, h, &x, &y);
    if (icon) {
        vx_blit_alpha(s, x + 16, y + 16, 48, 48, &icon->surface);
    } else {
        vx_fill(s, x + 16, y + 16, 48, 48, VX_COLOR_BUTTON);
        draw_icon(s, x + 33, y + 31, info_item >= 0 ? items[info_item].type : VX_TYPE_DIRECTORY);
    }
    for (int i = 0; i < n; i++) {
        int ly = y + 16 + i * (FONT_HEIGHT + 4);
        vx_draw_text(s, x + 80, ly, labels[i], VX_COLOR_DIM, VX_TRANSPARENT);
        vx_draw_text_fit(s, x + 80 + 12 * FONT_WIDTH, ly, w - 96 - 12 * FONT_WIDTH, values[i],
                         i == 0 ? VX_COLOR_ACCENT : VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    dialog_button(x, y, w, h, 0, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, "OK", false);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_WINDOW);
    for (int i = 0; i < BUTTONS; i++) {
        vx_draw_button(s, buttons[i].x, 6, buttons[i].width, 24, buttons[i].label, hot_button == i);
    }
    vx_draw_field(s, PATH_X, 6, w - PATH_X - 8, typing ? typed : cwd, typing);
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
        if (mode == RENAMING && top + i == renaming) {
            vx_draw_field(s, 36, y - 2, size_x - 44, new_name, true);
        } else {
            vx_draw_text_fit(s, 38, y + 2, size_x - 48, name, VX_COLOR_TEXT, VX_TRANSPARENT);
        }
        vx_draw_text(s, size_x, y + 2, size, VX_COLOR_DIM, VX_TRANSPARENT);
    }
    if (item_count > rows) { /* Where we are in a long list. */
        int bar = (rows * ROW) * rows / item_count;
        int at = (rows * ROW - bar) * top / (item_count - rows);
        vx_fill(s, w - 13, list_y + 2 + at, 3, bar, VX_COLOR_LINE);
    }
    vx_draw_text_fit(s, 10, h - STATUS + 3, w - 20, status, VX_COLOR_DIM, VX_TRANSPARENT);
    if (mode == MENU) {
        vx_draw_menu(s, menu_x, menu_y, menu, menu_count, menu_hot);
    } else if (mode == CONFIRM) {
        draw_confirm();
    } else if (mode == INFO) {
        draw_info();
    }
    vx_window_present(window, 0, 0, w, h);
}

static void select_item(int index) {
    if (item_count == 0) {
        selected = -1;
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

static void answer(bool yes) {
    mode = NORMAL;
    if (!yes) {
        return;
    }
    if (question == ASK_EMPTY_TRASH) {
        empty_trash();
    } else if (selected >= 0) {
        delete_item(selected);
    }
}

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
    } else if (e->key == VX_KEY_LEFTSHIFT || e->key == VX_KEY_RIGHTSHIFT) {
        shift = e->value != 0;
    }
    if (e->value == 0) {
        return;
    }
    switch (mode) {
    case MENU:
        if (e->key == VX_KEY_ESC) {
            mode = NORMAL;
        }
        return;
    case CONFIRM:
        if (e->key == VX_KEY_ENTER || e->key == VX_KEY_ESC) {
            answer(e->key == VX_KEY_ENTER);
        }
        return;
    case INFO:
        if (e->key == VX_KEY_ENTER || e->key == VX_KEY_ESC) {
            mode = NORMAL;
        }
        return;
    case RENAMING:
        if (e->key == VX_KEY_ENTER) {
            finish_rename();
        } else if (e->key == VX_KEY_ESC) {
            mode = NORMAL;
        } else {
            vx_field_key(new_name, sizeof(new_name), e);
        }
        return;
    case NORMAL:
        break;
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
    if (ctrl) {
        switch (e->key) {
        case 46: copy_item(selected, false); break; /* C */
        case 45: copy_item(selected, true); break;  /* X */
        case 47: paste(); break;                    /* V */
        case 23: show_info(selected); break;        /* I */
        case 38:                                    /* L: type a path */
            typing = true;
            typed[0] = '\0';
            break;
        case 35:                                    /* H: hidden files */
            show_hidden = !show_hidden;
            load(NULL);
            break;
        case 49:                                    /* N */
            if (shift) {
                new_folder();
            }
            break;
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
    case VX_KEY_F1 + 1: start_rename(selected); break; /* F2 */
    case VX_KEY_DELETE:
        if (selected >= 0) {
            in_trash() ? ask(ASK_DELETE) : trash_item(selected);
        }
        break;
    }
}

/* The list row at a point, or -1. */
static int row_at(int y) {
    int row = (y - TOOLBAR - 4) / ROW;
    if (y < TOOLBAR + 4 || row >= list_rows() || top + row >= item_count) {
        return -1;
    }
    return top + row;
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    bool right_click = (e->buttons & 2) && !(*held & 2);
    *held = e->buttons;
    if (mode == MENU) {
        menu_hot = vx_menu_item_at(menu, menu_count, menu_x, menu_y, e->x, e->y);
        if (click || right_click) {
            int hit = menu_hot;
            mode = NORMAL;
            if (hit >= 0) {
                act(menu_actions[hit]);
            }
        }
        return;
    }
    if (mode == CONFIRM || mode == INFO) {
        if (!click) {
            return;
        }
        int x, y, bx, by, w = mode == INFO ? 440 : 380;
        int h = 120;
        if (mode == INFO) {
            h = 96 + info_count * (FONT_HEIGHT + 4);
        }
        x = (window->surface.width - w) / 2;
        y = (window->surface.height - h) / 2;
        y = y < TOOLBAR ? TOOLBAR : y;
        for (int i = 0; i < (mode == INFO ? 1 : 2); i++) {
            dialog_button(x, y, w, h, i, &bx, &by);
            if (vx_inside(e->x, e->y, bx, by, 88, 26)) {
                if (mode == INFO) {
                    mode = NORMAL;
                } else {
                    answer(i == 0);
                }
            }
        }
        return;
    }
    if (mode == RENAMING && (click || right_click)) {
        finish_rename(); /* Clicking elsewhere keeps the new name, as in Finder. */
    }
    hot_button = -1;
    for (int i = 0; i < BUTTONS; i++) {
        if (vx_inside(e->x, e->y, buttons[i].x, 6, buttons[i].width, 24)) {
            hot_button = i;
        }
    }
    if (e->wheel) {
        scroll(-e->wheel * 3);
    }
    if (right_click) {
        int index = row_at(e->y);
        selected = index;
        open_menu(e->x, e->y);
        return;
    }
    if (!click) {
        return;
    }
    switch (hot_button) {
    case 0: up(); return;
    case 1: go("/"); return;
    case 2: go(VX_APPS_DIR); return;
    case 3:
        vx_mkdir(TRASH);
        go(TRASH);
        return;
    }
    int w = window->surface.width;
    if (vx_inside(e->x, e->y, PATH_X, 6, w - PATH_X - 8, FONT_HEIGHT + 8)) {
        typing = true;
        snprintf(typed, sizeof(typed), "%s", cwd);
        return;
    }
    typing = false;
    int index = row_at(e->y);
    if (index >= 0) {
        long now = vx_uptime();
        if (index == last_click && now - last_click_ms < DOUBLE_CLICK_MS) {
            open_item(index);
            last_click = -1;
            return;
        }
        selected = index;
        last_click = index;
        last_click_ms = now;
    } else if (e->y > TOOLBAR) {
        selected = -1; /* A click on nothing. */
    }
}

int main(int argc, char **argv) {
    window = vx_window_create_flags("Files", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "files: no desktop to open a window on\n");
        return 1;
    }
    go(argc > 1 ? argv[1] : "/");
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
        case VX_GUI_KEY:
            key(&e);
            break;
        case VX_GUI_POINTER:
            pointer(&e, &held);
            break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = shift = false; /* Their key ups go to another window. */
            }
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 300 && e.height >= 200) {
                vx_window_resize(window, e.width, e.height);
                scroll(0);
            }
            break;
        }
    }
}
