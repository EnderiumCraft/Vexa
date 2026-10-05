/* files: Vexa's file manager, in the manner of macOS's Finder.
 *
 * A window with a sidebar (places, and disks), a toolbar (Back, Forward,
 * Up, the location, list or icon view, search) and the folder: as a list
 * with columns (name, kind, size, modified; a click on a heading sorts by
 * it) or as icons (pictures show as thumbnails). Apps (.vxapp bundles) show
 * as apps, with their icons.
 *
 * A double click (or Enter) opens: a folder, an app, a program in /bin,
 * or a file with the app for its type (<vexa/app.h>). Ctrl-click and
 * Shift-click select several things; dragging them onto a folder or a place
 * moves them (Ctrl: copies). Space shows a Quick Look of an image or a text
 * file. A right click opens a menu: Open, Show Package Contents, Get Info,
 * Rename, Duplicate, Make Alias, Copy, Cut, Paste, Move to Trash; on the
 * empty part, New Folder, New Text Document, Paste, Open in Terminal and, in
 * the Trash, Empty Trash. The keyboard does the same: Ctrl+C, X, V, D, I,
 * A (all), F (search), L (location), H (hidden files), 1 and 2 (list,
 * icons), F2, Delete, Ctrl+Shift+N; Alt+Left and Right go back and forward,
 * Backspace up; typing a name's first letters goes to it.
 *
 * The clipboard is a file (CLIPBOARD) that every Files window shares.
 * Deleted things go to /Trash first. Copying a .vxapp into /apps installs
 * an app: the desktop notices it. What Files does is also printed (its
 * output is the desktop's log when the desktop started it).
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <vexa/app.h>
#include <vexa/files.h>
#include <vexa/gui.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

#define WIDTH 780
#define HEIGHT 500
#define TOOLBAR 40
#define STATUS 22
#define SIDEBAR 160
#define ROW 20
#define HEADER 20
#define CELL_W 104
#define CELL_H 90
#define MAX_ENTRIES 1024
#define DOUBLE_CLICK_MS double_click_ms /* Settings, Mouse & Keyboard. */
#define TRASH trash_folder /* "$HOME/.Trash" (main sets it). */
#define CLIPBOARD clipboard_file /* "/tmp/.files-clipboard-UID": each account's own. */
#define RESOURCES "/apps/Files.vxapp/Contents/Resources"
#define MAX_HISTORY 32
#define MAX_THUMB_BYTES (8 * 1024 * 1024)

/* ---- What's shown ---- */

enum kind { K_FOLDER, K_APP, K_IMAGE, K_TEXT, K_PROGRAM, K_DEVICE, K_LINK, K_DOCUMENT, K_AUDIO,
            K_VIDEO, KIND_COUNT };
static const char *const kind_names[] = {"Folder", "App", "Image", "Text", "Program",
                                         "Device", "Link", "Document", "Audio", "Video"};

struct item {
    char name[256];
    uint32_t type;
    uint64_t size;
    long long modified;
    enum kind kind;
    bool link;               /* A symbolic link (to what `kind` says). */
    bool app;                /* A .vxapp bundle. */
    bool selected;
    struct vx_image *icon;   /* An app's icon. */
    struct vx_image *thumb;  /* A picture's thumbnail (icon view). */
    bool thumb_tried;
};

static struct vx_window *window;
static char cwd[512] = "/";
static struct item items[MAX_ENTRIES];
static int item_count;
static int shown[MAX_ENTRIES]; /* The items that pass the search, in order. */
static int shown_count;
static int cursor = -1, anchor = -1; /* Positions in shown[]. */
static int top;                      /* The first row in view. */
static bool icon_view;
static bool show_hidden;
static enum { SORT_NAME, SORT_KIND, SORT_SIZE, SORT_MODIFIED } sort_by;
static bool sort_down; /* Reversed. */
static bool ctrl, shift, alt;
static int double_click_ms = 500;

static void read_settings(void) {
    struct vx_settings s;
    vx_settings_load(&s, "desktop.conf");
    double_click_ms = vx_settings_int(&s, "double_click_ms", 500);
}

static char message[200];
static long message_ms;

/* Typing into the location or the search field. */
static enum { FIELD_NONE, FIELD_PATH, FIELD_SEARCH } field;
static char typed[512];
static char search[64];

static enum { NORMAL, MENU, RENAMING, CONFIRM, INFO, PREVIEW } mode;

static char back_stack[MAX_HISTORY][512], forward_stack[MAX_HISTORY][512];
static int back_count, forward_count;

/* Files' own pictures, from its bundle. */
static struct vx_image *kind_icons[KIND_COUNT], *place_icons[16], *disk_icon;
/* Folders with icons of their own: home, Pictures, Music. */
static struct vx_image *home_icon, *pictures_icon, *music_icon;

/* ---- Small helpers ---- */

static void say(const char *text) {
    snprintf(message, sizeof(message), "%s", text);
    message_ms = vx_uptime();
    printf("files: %s\n", text);
    fflush(stdout);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static bool ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), m = strlen(suffix);
    return n >= m && !strcmp(text + n - m, suffix);
}

static bool same_letters(char a, char b) {
    return (a | 0x20) == (b | 0x20);
}

/* True if `text` has `part` in it, ignoring case. */
static bool contains(const char *text, const char *part) {
    size_t n = strlen(part);
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && same_letters(text[i], part[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0;
}

static struct item *at(int position) {
    return position >= 0 && position < shown_count ? &items[shown[position]] : NULL;
}

static void item_path(char *out, size_t size, const struct item *item) {
    vx_join_path(out, size, cwd, item->name);
}

static struct vx_image *resource(const char *name) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.png", RESOURCES, name);
    return vx_image_load(path, VX_IMAGE_ALPHA);
}

static void load_resources(void) {
    static const char *const kinds[] = {"folder", "folder", "image", "text", "program",
                                        "device", "document", "document", "audio", "video"};
    for (int i = 0; i < KIND_COUNT; i++) {
        kind_icons[i] = resource(kinds[i]);
    }
    disk_icon = resource("disk");
    home_icon = resource("home");
    pictures_icon = resource("pictures");
    music_icon = resource("music");
}

static enum kind kind_of_name(const char *name, uint32_t type) {
    if (type == VX_TYPE_DIRECTORY) {
        return K_FOLDER;
    }
    if (type == VX_TYPE_CHAR_DEVICE || type == VX_TYPE_BLOCK_DEVICE || type == VX_TYPE_SOCKET) {
        return K_DEVICE;
    }
    const char *dot = strrchr(name, '.');
    if (dot) {
        static const char *const images[] = {".png", ".bmp", ".ppm", ".PNG"};
        static const char *const texts[] = {".txt", ".conf", ".md", ".c", ".h", ".py", ".sh",
                                            ".log", ".desktop", ".ini", ".json", ".xml"};
        for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
            if (!strcmp(dot, images[i])) {
                return K_IMAGE;
            }
        }
        for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
            if (!strcmp(dot, texts[i])) {
                return K_TEXT;
            }
        }
        static const char *const sounds[] = {".mp3", ".ogg", ".oga", ".flac", ".wav"};
        for (size_t i = 0; i < sizeof(sounds) / sizeof(sounds[0]); i++) {
            if (!strcasecmp(dot, sounds[i])) {
                return K_AUDIO;
            }
        }
        if (!strcasecmp(dot, ".mpg") || !strcasecmp(dot, ".mpeg")) {
            return K_VIDEO;
        }
    }
    return K_DOCUMENT;
}

/* ---- The sidebar: places, then disks ---- */

struct place {
    char label[40];
    char path[256];
    struct vx_image **icon;
};
/* The account's own folders (main sets them). */
static char home_folder[256], desktop_folder[300], documents_folder[300], pictures_folder[300];
static char trash_folder[300], clipboard_file[64];

#define MAX_PLACES 16
static struct place places[MAX_PLACES];
static int place_count, disks_start;
static int drop_place = -1; /* Where a drag would drop, or -1. */

static void find_places(void) {
    static const struct {
        const char *label, *path, *icon;
    } fixed[] = {
        {"Vexa", "/", "computer"}, {"Apps", VX_APPS_DIR, "apps"},
        {"Home", home_folder, "home"}, {"Desktop", desktop_folder, "folder"},
        {"Documents", documents_folder, "folder"}, {"Pictures", pictures_folder, "pictures"},
        {"Wallpapers", "/share/pictures", "pictures"}, {"Temporary", "/tmp", "folder"},
        {"Trash", TRASH, "trash"},
    };
    place_count = 0;
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (!place_icons[i]) {
            place_icons[i] = resource(fixed[i].icon);
        }
        struct place *p = &places[place_count++];
        snprintf(p->label, sizeof(p->label), "%s", fixed[i].label);
        snprintf(p->path, sizeof(p->path), "%s", fixed[i].path);
        p->icon = &place_icons[i];
    }
    disks_start = place_count;
    /* The boot CD, and what's mounted in /mnt. */
    struct vx_stat stat;
    char cdrom[256] = "";
    long got = vx_readlink("/cdrom", cdrom, sizeof(cdrom) - 1);
    cdrom[got > 0 ? got : 0] = '\0';
    if (vx_stat("/cdrom", &stat) == 0 && stat.type == VX_TYPE_DIRECTORY) {
        struct place *p = &places[place_count++];
        snprintf(p->label, sizeof(p->label), "Boot CD");
        snprintf(p->path, sizeof(p->path), "/cdrom");
        p->icon = &disk_icon;
    }
    int handle = vx_open("/mnt", VX_OPEN_READ);
    if (handle >= 0) {
        struct vx_dir_entry entries[16];
        long n;
        while ((n = vx_read_dir(handle, entries, 16)) > 0) {
            for (long i = 0; i < n && place_count < MAX_PLACES; i++) {
                char path[300];
                snprintf(path, sizeof(path), "/mnt/%s", entries[i].name);
                if (entries[i].name[0] == '.' || entries[i].type != VX_TYPE_DIRECTORY ||
                    !strcmp(path, cdrom)) { /* (The boot CD is there already.) */
                    continue;
                }
                struct place *p = &places[place_count++];
                snprintf(p->label, sizeof(p->label), "%s", entries[i].name);
                snprintf(p->path, sizeof(p->path), "/mnt/%s", entries[i].name);
                p->icon = &disk_icon;
            }
        }
        vx_close(handle);
    }
}

static int place_y(int i) {
    return TOOLBAR + 28 + i * 24 + (i >= disks_start ? 28 : 0);
}

static int place_at(int x, int y) {
    if (x >= SIDEBAR) {
        return -1;
    }
    for (int i = 0; i < place_count; i++) {
        if (y >= place_y(i) && y < place_y(i) + 24) {
            return i;
        }
    }
    return -1;
}

/* ---- The folder ---- */

static int compare(const void *a, const void *b) {
    const struct item *x = &items[*(const int *)a], *y = &items[*(const int *)b];
    bool dx = x->kind == K_FOLDER, dy = y->kind == K_FOLDER; /* Folders first. */
    if (dx != dy) {
        return dx ? -1 : 1;
    }
    int order = 0;
    switch (sort_by) {
    case SORT_KIND: order = strcmp(kind_names[x->kind], kind_names[y->kind]); break;
    case SORT_SIZE: order = x->size < y->size ? -1 : x->size > y->size; break;
    case SORT_MODIFIED: order = x->modified < y->modified ? -1 : x->modified > y->modified; break;
    case SORT_NAME: break;
    }
    if (!order) {
        order = strcmp(x->name, y->name);
    }
    return sort_down ? -order : order;
}

/* Works out shown[] from the items: the search, then the order. */
static void arrange(void) {
    shown_count = 0;
    for (int i = 0; i < item_count; i++) {
        if (contains(items[i].name, search)) {
            shown[shown_count++] = i;
        } else {
            items[i].selected = false;
        }
    }
    qsort(shown, (size_t)shown_count, sizeof(shown[0]), compare);
    cursor = anchor = -1;
    for (int i = 0; i < shown_count && cursor < 0; i++) {
        if (items[shown[i]].selected) {
            cursor = anchor = i;
        }
    }
    top = 0;
}

static void free_items(void) {
    for (int i = 0; i < item_count; i++) {
        vx_image_free(items[i].icon);
        vx_image_free(items[i].thumb);
    }
    item_count = 0;
}

static unsigned long folder_signature(void);
static unsigned long folder_seen;

static void load(void) {
    free_items();
    shown_count = 0;
    folder_seen = folder_signature();
    int handle = vx_open(cwd, VX_OPEN_READ);
    if (handle < 0) {
        snprintf(message, sizeof(message), "Can't open %s: %s", cwd, vx_strerror(handle));
        message_ms = vx_uptime();
        return;
    }
    struct vx_dir_entry entries[32];
    long n;
    while ((n = vx_read_dir(handle, entries, 32)) > 0) {
        for (long i = 0; i < n && item_count < MAX_ENTRIES; i++) {
            const char *name = entries[i].name;
            if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && !show_hidden)) {
                continue;
            }
            struct item *item = &items[item_count++];
            memset(item, 0, sizeof(*item));
            snprintf(item->name, sizeof(item->name), "%s", name);
            item->type = entries[i].type;
            item->link = item->type == VX_TYPE_SYMLINK;
            char path[800];
            item_path(path, sizeof(path), item);
            struct vx_stat stat;
            uint32_t type = item->type;
            if (vx_stat(path, &stat) == 0) { /* What a link points to. */
                item->size = stat.size;
                item->modified = stat.modified;
                type = stat.type;
            }
            item->kind = kind_of_name(name, type);
            if (type == VX_TYPE_FILE && !strcmp(cwd, "/bin")) {
                item->kind = K_PROGRAM;
            }
            struct vx_app app;
            if (type == VX_TYPE_DIRECTORY && vx_app_is_bundle(name) && vx_app_load(path, &app) == 0) {
                item->app = true;
                item->kind = K_APP;
                item->icon = app.icon[0] ? vx_image_load(app.icon, VX_IMAGE_ALPHA) : NULL;
            }
        }
    }
    vx_close(handle);
    arrange();
    char title[600];
    snprintf(title, sizeof(title), "%s - Files", cwd);
    vx_window_set_title(window, title);
    find_places();
}

static void select_only(int position);

static void select_names(char names[][256], int count) {
    for (int n = 0; n < count; n++) {
        for (int i = 0; i < shown_count; i++) {
            if (!strcmp(items[shown[i]].name, names[n])) {
                items[shown[i]].selected = true;
                if (n == 0) {
                    cursor = anchor = i;
                }
            }
        }
    }
}

/* After an operation: read the folder again, with these names selected. */
static void reload_selecting(char names[][256], int count) {
    load();
    select_names(names, count);
}

/* What's in the folder, as one number (names, and for small folders sizes
 * and times too): when it changes, something else changed the folder, and
 * it's read again. */
static unsigned long folder_signature(void) {
    unsigned long hash = 5381;
    int handle = vx_open(cwd, VX_OPEN_READ);
    if (handle < 0) {
        return 0;
    }
    struct vx_dir_entry entries[32];
    long n;
    int count = 0;
    while ((n = vx_read_dir(handle, entries, 32)) > 0) {
        for (long i = 0; i < n; i++, count++) {
            unsigned long h = 5381;
            for (const char *c = entries[i].name; *c; c++) {
                h = h * 33 + (unsigned char)*c;
            }
            if (count < 200) {
                char path[800];
                struct vx_stat st;
                vx_join_path(path, sizeof(path), cwd, entries[i].name);
                if (vx_lstat(path, &st) == 0) {
                    h = h * 31 + (unsigned long)st.size;
                    h = h * 31 + (unsigned long)st.modified;
                }
            }
            hash += h; /* The order doesn't matter. */
        }
    }
    vx_close(handle);
    return hash + (unsigned long)count;
}

static long folder_checked_ms;

/* Reads the folder again if something else changed it (keeping what was
 * selected); true if it did. */
static bool refresh_if_changed(void) {
    folder_checked_ms = vx_uptime();
    unsigned long now = folder_signature();
    if (now == folder_seen) {
        return false;
    }
    static char names[64][256];
    int count = 0;
    for (int i = 0; i < item_count && count < 64; i++) {
        if (items[i].selected) {
            snprintf(names[count++], sizeof(names[0]), "%s", items[i].name);
        }
    }
    reload_selecting(names, count);
    printf("files: %s changed; read it again\n", cwd);
    return true;
}

static void go_to(const char *path, bool remember) {
    struct vx_stat stat;
    if (vx_stat(path, &stat) || stat.type != VX_TYPE_DIRECTORY) {
        char text[600];
        snprintf(text, sizeof(text), "Not a folder: %s", path);
        say(text);
        return;
    }
    if (remember && strcmp(path, cwd)) {
        if (back_count == MAX_HISTORY) {
            memmove(back_stack, back_stack + 1, sizeof(back_stack[0]) * (MAX_HISTORY - 1));
            back_count--;
        }
        snprintf(back_stack[back_count++], sizeof(back_stack[0]), "%s", cwd);
        forward_count = 0;
    }
    char resolved[512];
    if (vx_chdir(path) == 0 && vx_getcwd(resolved, sizeof(resolved)) >= 0) {
        snprintf(cwd, sizeof(cwd), "%s", resolved);
    } else {
        snprintf(cwd, sizeof(cwd), "%s", path);
    }
    search[0] = '\0';
    if (field == FIELD_SEARCH) {
        field = FIELD_NONE;
    }
    load();
}

static void go(const char *path) {
    go_to(path, true);
}

static void go_back(void) {
    if (back_count) {
        if (forward_count < MAX_HISTORY) {
            snprintf(forward_stack[forward_count++], sizeof(forward_stack[0]), "%s", cwd);
        }
        char path[512];
        snprintf(path, sizeof(path), "%s", back_stack[--back_count]);
        go_to(path, false);
    }
}

static void go_forward(void) {
    if (forward_count) {
        if (back_count < MAX_HISTORY) {
            snprintf(back_stack[back_count++], sizeof(back_stack[0]), "%s", cwd);
        }
        char path[512];
        snprintf(path, sizeof(path), "%s", forward_stack[--forward_count]);
        go_to(path, false);
    }
}

static void up(void) {
    if (!strcmp(cwd, "/")) {
        return;
    }
    char parent[512], child[256];
    snprintf(parent, sizeof(parent), "%s", cwd);
    snprintf(child, sizeof(child), "%s", base_name(cwd));
    char *slash = strrchr(parent, '/');
    if (slash == parent) {
        parent[1] = '\0';
    } else if (slash) {
        *slash = '\0';
    }
    go(parent);
    char names[1][256];
    snprintf(names[0], sizeof(names[0]), "%s", child);
    select_names(names, 1); /* The folder we came from. */
}

/* ---- Selection ---- */

static int selected_count(void) {
    int n = 0;
    for (int i = 0; i < shown_count; i++) {
        n += items[shown[i]].selected;
    }
    return n;
}

static void clear_selection(void) {
    for (int i = 0; i < item_count; i++) {
        items[i].selected = false;
    }
}

static int columns(void);
static int visible_rows(void);

static void keep_in_view(int position) {
    int per_row = icon_view ? columns() : 1;
    int row = position / per_row;
    if (row < top) {
        top = row;
    } else if (row >= top + visible_rows()) {
        top = row - visible_rows() + 1;
    }
}

static void select_only(int position) {
    clear_selection();
    if (shown_count == 0) {
        cursor = anchor = -1;
        return;
    }
    position = position < 0 ? 0 : position >= shown_count ? shown_count - 1 : position;
    items[shown[position]].selected = true;
    cursor = anchor = position;
    keep_in_view(position);
}

/* Shift: everything from the anchor to here. */
static void select_to(int position) {
    if (shown_count == 0) {
        return;
    }
    position = position < 0 ? 0 : position >= shown_count ? shown_count - 1 : position;
    if (anchor < 0) {
        anchor = position;
    }
    clear_selection();
    int from = anchor < position ? anchor : position, to = anchor < position ? position : anchor;
    for (int i = from; i <= to; i++) {
        items[shown[i]].selected = true;
    }
    cursor = position;
    keep_in_view(position);
}

static void move_cursor(int to) {
    if (shift) {
        select_to(to);
    } else {
        select_only(to);
    }
}

/* The selected items' paths (at most `max`). */
static int selected_paths(char paths[][512], int max) {
    int n = 0;
    for (int i = 0; i < shown_count && n < max; i++) {
        if (items[shown[i]].selected) {
            item_path(paths[n++], 512, &items[shown[i]]);
        }
    }
    return n;
}

/* ---- Opening ---- */

static void started(int process, const char *what) {
    if (process < 0) {
        char text[300];
        snprintf(text, sizeof(text), "Can't start %s: %s", what, vx_strerror(process));
        say(text);
    } else {
        vx_close(process); /* It runs on its own. */
    }
}

static long run_argv(const char **argv, int argc) {
    unsigned long envc = 0;
    while (environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {
        .argv = argv, .argc = (unsigned long)argc, .envp = (const char *const *)environ,
        .envc = envc, .handles = {0, 1, 2}, .flags = VX_SPAWN_NEW_GROUP,
    };
    return vx_spawn(argv[0], &spawn);
}

static void run(const char *program, const char *argument) {
    const char *argv[] = {program, argument};
    started(run_argv(argv, argument ? 2 : 1), program);
}

static void open_item(struct item *item) {
    if (!item) {
        return;
    }
    char path[800];
    item_path(path, sizeof(path), item);
    struct vx_app app;
    if (item->app && vx_app_load(path, &app) == 0) {
        started(vx_app_open(&app, NULL), app.name);
    } else if (item->kind == K_FOLDER) {
        go(path);
    } else if (item->kind == K_PROGRAM) {
        run(path, NULL);
    } else if (vx_app_for_file(path, &app) == 0) {
        started(vx_app_open(&app, path), app.name);
    } else {
        char text[300];
        snprintf(text, sizeof(text), "No app opens %s", item->name);
        say(text);
    }
}

/* Enter: the selection (folders: only the first, which we go into). */
static void open_selection(void) {
    for (int i = 0; i < shown_count; i++) {
        struct item *item = &items[shown[i]];
        if (item->selected && !(item->kind == K_FOLDER && !item->app)) {
            open_item(item);
        }
    }
    for (int i = 0; i < shown_count; i++) {
        struct item *item = &items[shown[i]];
        if (item->selected && item->kind == K_FOLDER && !item->app) {
            open_item(item);
            return;
        }
    }
}

static void open_with(struct item *item, const char *app_name) {
    struct vx_app app;
    char path[800];
    item_path(path, sizeof(path), item);
    if (vx_app_find(app_name, &app) == 0) {
        started(vx_app_open(&app, path), app.name);
    }
}

/* ---- The clipboard: "copy" or "cut", then a path a line. ---- */

#define MAX_CLIP 64

static int read_clipboard(char paths[][512], int max, bool *cut) {
    int handle = vx_open(CLIPBOARD, VX_OPEN_READ);
    if (handle < 0) {
        return 0;
    }
    static char text[MAX_CLIP * 520];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    char *line = text, *next = strchr(line, '\n');
    if (!next) {
        return 0;
    }
    *next++ = '\0';
    *cut = !strcmp(line, "cut");
    int count = 0;
    for (line = next; line && *line && count < max; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (vx_lstat(line, &(struct vx_stat){0}) == 0) {
            snprintf(paths[count++], 512, "%s", line);
        }
    }
    return count;
}

static bool can_paste(void) {
    static char paths[1][512];
    bool cut;
    return read_clipboard(paths, 1, &cut) > 0;
}

static void copy_selection(bool cut) {
    static char paths[MAX_CLIP][512];
    int n = selected_paths(paths, MAX_CLIP);
    if (!n) {
        return;
    }
    int handle = vx_open(CLIPBOARD, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle >= 0) {
        vx_write(handle, cut ? "cut\n" : "copy\n", cut ? 4 : 5);
        for (int i = 0; i < n; i++) {
            vx_write(handle, paths[i], strlen(paths[i]));
            vx_write(handle, "\n", 1);
        }
        vx_close(handle);
    }
    char text[700];
    if (n == 1) {
        snprintf(text, sizeof(text), "%s %s", cut ? "cut" : "copied", paths[0]);
    } else {
        snprintf(text, sizeof(text), "%s %d items", cut ? "cut" : "copied", n);
    }
    say(text);
}

/* Copies or moves paths into a folder; returns how many made it. */
static int transfer(char paths[][512], int count, const char *into, bool move,
                    char names[][256]) {
    int done = 0;
    for (int i = 0; i < count; i++) {
        char name[256], to[800], text[1400];
        /* Not a folder into itself, and moving to where it is does nothing. */
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", paths[i]);
        char *slash = strrchr(parent, '/');
        if (slash) {
            slash == parent ? (void)(parent[1] = '\0') : (void)(*slash = '\0');
        }
        if (move && !strcmp(parent, into)) {
            continue;
        }
        size_t n = strlen(paths[i]);
        if (!strcmp(into, paths[i]) || (!strncmp(into, paths[i], n) && into[n] == '/')) {
            snprintf(text, sizeof(text), "Can't put %s inside itself", base_name(paths[i]));
            say(text);
            continue;
        }
        vx_unique_name(into, base_name(paths[i]), name, sizeof(name));
        vx_join_path(to, sizeof(to), into, name);
        long error = move ? vx_move(paths[i], to) : vx_copy_tree(paths[i], to);
        if (error) {
            snprintf(text, sizeof(text), "Can't %s %s: %s", move ? "move" : "copy", paths[i],
                     vx_strerror(error));
            say(text);
            continue;
        }
        if (names) {
            snprintf(names[done], 256, "%s", name);
        }
        done++;
        snprintf(text, sizeof(text), "%s %s to %s", move ? "moved" : "pasted", paths[i], to);
        say(text);
    }
    return done;
}

static void paste(void) {
    static char paths[MAX_CLIP][512], names[MAX_CLIP][256];
    bool cut;
    int n = read_clipboard(paths, MAX_CLIP, &cut);
    if (!n) {
        say("Nothing to paste");
        return;
    }
    int done = transfer(paths, n, cwd, cut, names);
    if (cut) {
        vx_remove(CLIPBOARD); /* They've moved; there's nothing to paste again. */
    }
    reload_selecting(names, done);
}

static void duplicate(void) {
    static char paths[MAX_CLIP][512], names[MAX_CLIP][256];
    int n = selected_paths(paths, MAX_CLIP);
    reload_selecting(names, transfer(paths, n, cwd, false, names));
}

static void make_alias(void) {
    static char paths[MAX_CLIP][512], names[MAX_CLIP][256];
    int n = selected_paths(paths, MAX_CLIP), done = 0;
    for (int i = 0; i < n; i++) {
        char wanted[300], to[800], text[1400];
        snprintf(wanted, sizeof(wanted), "%s alias", base_name(paths[i]));
        vx_unique_name(cwd, wanted, names[done], 256);
        vx_join_path(to, sizeof(to), cwd, names[done]);
        long error = vx_symlink(paths[i], to);
        snprintf(text, sizeof(text), error ? "Can't make an alias of %s: %s" : "alias %s%s",
                 error ? paths[i] : to, error ? vx_strerror(error) : "");
        say(text);
        done += !error;
    }
    reload_selecting(names, done);
}

static bool in_trash(void) {
    return !strcmp(cwd, TRASH);
}

/* Where a thing in the Trash came from: its path, in .info/<its name>. */
static void trash_info_path(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/.info/%s", TRASH, name);
}

static void trash_selection(void) {
    static char paths[MAX_CLIP][512];
    int n = selected_paths(paths, MAX_CLIP), first = cursor;
    vx_mkdir(TRASH);
    for (int i = 0; i < n; i++) {
        char name[256], to[800], text[1400];
        vx_unique_name(TRASH, base_name(paths[i]), name, sizeof(name));
        vx_join_path(to, sizeof(to), TRASH, name);
        long error = vx_move(paths[i], to);
        if (error) {
            snprintf(text, sizeof(text), "Can't move %s to the Trash: %s", paths[i], vx_strerror(error));
        } else {
            snprintf(text, sizeof(text), "moved %s to the Trash", paths[i]);
            char info[600];
            snprintf(info, sizeof(info), "%s/.info", TRASH);
            vx_mkdir(info);
            trash_info_path(info, sizeof(info), name);
            FILE *f = fopen(info, "w");
            if (f) {
                fputs(paths[i], f);
                fclose(f);
            }
        }
        say(text);
    }
    load();
    if (first >= 0 && shown_count) {
        select_only(first);
    }
}

/* Put Back: things in the Trash go back where they were (under another name
 * if that's taken; to the home folder if where is unknown). */
static void restore_selection(void) {
    static char paths[MAX_CLIP][512];
    int n = selected_paths(paths, MAX_CLIP);
    for (int i = 0; i < n; i++) {
        const char *name = base_name(paths[i]);
        char info[600], from[512] = "", folder[512], to[800], unique[256], text[1400];
        trash_info_path(info, sizeof(info), name);
        FILE *f = fopen(info, "r");
        if (f) {
            if (!fgets(from, sizeof(from), f)) {
                from[0] = '\0';
            }
            fclose(f);
        }
        char *slash = strrchr(from, '/');
        if (slash && slash != from) {
            *slash = '\0';
            snprintf(folder, sizeof(folder), "%s", from);
            name = slash + 1;
        } else {
            snprintf(folder, sizeof(folder), "%s", vx_home());
        }
        vx_unique_name(folder, name, unique, sizeof(unique));
        vx_join_path(to, sizeof(to), folder, unique);
        long error = vx_move(paths[i], to);
        if (error) {
            snprintf(text, sizeof(text), "Can't put back %s: %s", base_name(paths[i]), vx_strerror(error));
        } else {
            vx_remove(info);
            snprintf(text, sizeof(text), "put back %s", to);
        }
        say(text);
    }
    load();
}

static void delete_selection(void) {
    static char paths[MAX_CLIP][512];
    int n = selected_paths(paths, MAX_CLIP);
    for (int i = 0; i < n; i++) {
        char text[700];
        if (in_trash()) {
            char info[600];
            trash_info_path(info, sizeof(info), base_name(paths[i]));
            vx_remove(info);
        }
        long error = vx_remove_tree(paths[i]);
        snprintf(text, sizeof(text), error ? "Can't delete %s: %s" : "deleted %s%s", paths[i],
                 error ? vx_strerror(error) : "");
        say(text);
    }
    load();
}

static void empty_trash(void) {
    long error = vx_remove_tree(TRASH);
    vx_mkdir(TRASH);
    if (in_trash()) {
        load();
    }
    say(error ? "Can't empty the Trash" : "emptied the Trash");
}

/* ---- Renaming, and naming new things ---- */

static char new_name[256];
static int renaming = -1; /* An item (not a position). */

static void start_rename(void) {
    struct item *item = at(cursor);
    if (!item) {
        return;
    }
    select_only(cursor);
    renaming = shown[cursor];
    snprintf(new_name, sizeof(new_name), "%s", item->name);
    mode = RENAMING;
}

static void finish_rename(void) {
    mode = NORMAL;
    if (renaming < 0 || !new_name[0] || !strcmp(new_name, items[renaming].name)) {
        return;
    }
    if (strchr(new_name, '/')) {
        say("A name can't have a / in it");
        return;
    }
    char from[800], to[800], text[1700];
    item_path(from, sizeof(from), &items[renaming]);
    vx_join_path(to, sizeof(to), cwd, new_name);
    long error = vx_move(from, to);
    if (error) {
        snprintf(text, sizeof(text), "Can't rename to %s: %s", new_name, vx_strerror(error));
        say(text);
        return;
    }
    char names[1][256];
    snprintf(names[0], sizeof(names[0]), "%s", new_name);
    reload_selecting(names, 1);
    snprintf(text, sizeof(text), "renamed %s to %s", from, to);
    say(text);
}

/* A new folder or text file, named right away (as Finder does). */
static void new_thing(bool folder) {
    char names[1][256], path[800], text[900];
    vx_unique_name(cwd, folder ? "untitled folder" : "untitled.txt", names[0], sizeof(names[0]));
    vx_join_path(path, sizeof(path), cwd, names[0]);
    long error;
    if (folder) {
        error = vx_mkdir(path);
    } else {
        int handle = vx_open(path, VX_OPEN_WRITE | VX_OPEN_CREATE);
        error = handle < 0 ? handle : 0;
        if (handle >= 0) {
            vx_close(handle);
        }
    }
    if (error) {
        snprintf(text, sizeof(text), "Can't make %s: %s", names[0], vx_strerror(error));
        say(text);
        return;
    }
    reload_selecting(names, 1);
    snprintf(text, sizeof(text), "new %s %s", folder ? "folder" : "file", path);
    say(text);
    start_rename();
}

/* ---- A question before deleting for good ---- */

static enum { ASK_EMPTY_TRASH, ASK_DELETE } question;

static void ask(int what) {
    question = what;
    mode = CONFIRM;
}

static void answer(bool yes) {
    mode = NORMAL;
    if (yes) {
        question == ASK_EMPTY_TRASH ? empty_trash() : delete_selection();
    }
}

/* ---- Get Info ---- */

#define INFO_LINES 9
static char info_labels[INFO_LINES][16], info_values[INFO_LINES][200];
static int info_count;
static struct vx_image *info_icon;

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

/* "2026-09-26 10:54" (UTC) from seconds since 1970. */
static void format_date(char *out, size_t size, long long seconds) {
    if (seconds <= 0) {
        snprintf(out, size, "--");
        return;
    }
    long long days = seconds / 86400, rest = seconds % 86400;
    /* Days to a civil date (Howard Hinnant's algorithm). */
    long long z = days + 719468, era = z / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, size, "%04lld-%02lld-%02lld %02lld:%02lld", y + (m <= 2), m, d, rest / 3600,
             rest / 60 % 60);
}

static struct vx_image *icon_for(const struct item *item);

static void show_info(void) {
    int n = 0, count = selected_count();
    struct item *item = count == 1 ? at(cursor) : NULL;
    char path[800];
#define LINE(label, ...) \
    (snprintf(info_labels[n], 16, "%s", label), snprintf(info_values[n], 200, __VA_ARGS__), n++)
    if (count > 1) { /* Several things: how many, and their size. */
        static char paths[MAX_CLIP][512];
        int got = selected_paths(paths, MAX_CLIP);
        unsigned long long total = 0;
        long files = 0;
        for (int i = 0; i < got; i++) {
            total += vx_tree_size(paths[i], &files);
        }
        char bytes[64];
        format_bytes(bytes, sizeof(bytes), total);
        LINE("Name:", "%d items", count);
        LINE("Kind:", "Selection");
        LINE("Size:", "%s in %ld file%s", bytes, files, files == 1 ? "" : "s");
        LINE("Where:", "%s", cwd);
        info_icon = kind_icons[K_DOCUMENT];
    } else {
        if (item) {
            item_path(path, sizeof(path), item);
        } else {
            snprintf(path, sizeof(path), "%s", cwd); /* Nothing selected: the folder. */
        }
        struct vx_stat stat = {0}, target = {0};
        vx_lstat(path, &stat);
        vx_stat(path, &target);
        bool app = item ? item->app : vx_app_is_bundle(path);
        enum kind kind = item ? item->kind : K_FOLDER;
        info_icon = item ? icon_for(item) : kind_icons[K_FOLDER];
        LINE("Name:", "%s", base_name(path));
        LINE("Kind:", "%s%s", kind_names[kind], stat.type == VX_TYPE_SYMLINK ? " (alias)" : "");
        char bytes[64];
        if (target.type == VX_TYPE_DIRECTORY) {
            long files = 0;
            format_bytes(bytes, sizeof(bytes), vx_tree_size(path, &files));
            LINE("Size:", "%s in %ld file%s", bytes, files, files == 1 ? "" : "s");
        } else {
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
            LINE("Modified:", "%s UTC", date);
        }
        if (stat.type == VX_TYPE_SYMLINK) {
            char to[200];
            long got = vx_readlink(path, to, sizeof(to) - 1);
            to[got > 0 ? got : 0] = '\0';
            LINE("Original:", "%s", to);
        }
        struct vx_app a;
        if (app && vx_app_load(path, &a) == 0) {
            LINE("App name:", "%s", a.name);
            LINE("Program:", "%s", a.executable);
            LINE("Opens:", "%s", a.opens[0] ? a.opens : "(no files)");
        } else if (target.type == VX_TYPE_FILE && vx_app_for_file(path, &a) == 0) {
            LINE("Opens with:", "%s", a.name);
        }
    }
#undef LINE
    info_count = n;
    mode = INFO;
    printf("files: info for %s: %s, %s\n", info_values[0], info_values[1], info_values[2]);
    fflush(stdout);
}

/* ---- Quick Look ---- */

static struct vx_image *preview_image;
static char *preview_text;
static char preview_name[256];

static void close_preview(void) {
    vx_image_free(preview_image);
    free(preview_text);
    preview_image = NULL;
    preview_text = NULL;
    mode = NORMAL;
}

static void quick_look(void) {
    struct item *item = at(cursor);
    if (!item) {
        return;
    }
    char path[800];
    item_path(path, sizeof(path), item);
    snprintf(preview_name, sizeof(preview_name), "%s", item->name);
    if (item->kind == K_IMAGE) {
        preview_image = vx_image_load(path, VX_COLOR_VIEW);
    } else if (item->kind == K_TEXT || item->kind == K_DOCUMENT) {
        int handle = vx_open(path, VX_OPEN_READ);
        if (handle >= 0) {
            preview_text = malloc(8192);
            long n = preview_text ? vx_read(handle, preview_text, 8191) : 0;
            if (preview_text) {
                preview_text[n > 0 ? n : 0] = '\0';
            }
            vx_close(handle);
        }
    }
    mode = PREVIEW;
    printf("files: quick look at %s\n", item->name);
    fflush(stdout);
}

/* ---- Layout ---- */

static int main_x(void) {
    return SIDEBAR + 4;
}

static int main_w(void) {
    return window->surface.width - main_x() - 8;
}

static int list_y(void) {
    return TOOLBAR + 2 + (icon_view ? 0 : HEADER);
}

static int list_h(void) {
    return window->surface.height - STATUS - list_y() - 4;
}

static int columns(void) {
    int n = main_w() / CELL_W;
    return n < 1 ? 1 : n;
}

static int visible_rows(void) {
    int n = list_h() / (icon_view ? CELL_H : ROW);
    return n < 1 ? 1 : n;
}

static int total_rows(void) {
    return icon_view ? (shown_count + columns() - 1) / columns() : shown_count;
}

static void scroll(int by) {
    int max = total_rows() - visible_rows();
    top += by;
    top = top > max ? max : top;
    top = top < 0 ? 0 : top;
}

/* Where a position is drawn (its whole cell or row). */
static bool cell(int position, int *x, int *y, int *w, int *h) {
    if (icon_view) {
        int row = position / columns() - top, col = position % columns();
        *x = main_x() + col * CELL_W;
        *y = list_y() + row * CELL_H;
        *w = CELL_W;
        *h = CELL_H;
        return row >= 0 && row < visible_rows();
    }
    int row = position - top;
    *x = main_x();
    *y = list_y() + row * ROW;
    *w = main_w();
    *h = ROW;
    return row >= 0 && row < visible_rows();
}

static int position_at(int px, int py) {
    if (px < main_x() || py < list_y() || py >= list_y() + list_h()) {
        return -1;
    }
    if (icon_view) {
        int col = (px - main_x()) / CELL_W, row = (py - list_y()) / CELL_H + top;
        int position = row * columns() + col;
        return col < columns() && position < shown_count ? position : -1;
    }
    int position = (py - list_y()) / ROW + top;
    return position < shown_count ? position : -1;
}

/* The toolbar. */
enum { B_BACK, B_FORWARD, B_UP, B_LIST, B_ICONS, BUTTON_COUNT };

static void button_rect(int b, int *x, int *w) {
    int width = window->surface.width;
    switch (b) {
    case B_BACK: *x = 8, *w = 28; return;
    case B_FORWARD: *x = 38, *w = 28; return;
    case B_UP: *x = 68, *w = 28; return;
    case B_LIST: *x = width - 176 - 104, *w = 48; return;
    case B_ICONS: *x = width - 176 - 54, *w = 48; return;
    }
}

static int path_x(void) {
    return 104;
}

static int path_w(void) {
    int x, w;
    button_rect(B_LIST, &x, &w);
    return x - 8 - path_x();
}

static int search_x(void) {
    return window->surface.width - 168;
}

/* List columns: kind, size, modified (from the right). */
#define COL_MODIFIED 136
#define COL_SIZE 80
#define COL_KIND 90

static int column_x(int which) { /* 0 name, 1 kind, 2 size, 3 modified */
    int right = main_x() + main_w() - 12;
    switch (which) {
    case 3: return right - COL_MODIFIED;
    case 2: return right - COL_MODIFIED - COL_SIZE;
    case 1: return right - COL_MODIFIED - COL_SIZE - COL_KIND;
    }
    return main_x() + 30;
}

/* ---- Drawing ---- */

static struct vx_image *icon_for(const struct item *item) {
    if (item->icon) {
        return item->icon;
    }
    if (item->kind == K_FOLDER && !item->link) { /* Home, Pictures and Music have their own. */
        char path[800];
        snprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", item->name);
        struct vx_image *own = !strcmp(path, home_folder) ? home_icon
                               : !strcmp(path, pictures_folder) ? pictures_icon
                               : !strcmp(path, vx_home_folder("Music")) ? music_icon : NULL;
        if (own) {
            return own;
        }
    }
    return kind_icons[item->kind];
}

static bool wants_thumbnail(const struct item *item) {
    return item->kind == K_IMAGE && item->size <= MAX_THUMB_BYTES && !item->thumb_tried;
}

/* Makes a picture's thumbnail (reading a big picture takes a while, so this
 * happens one at a time when nothing else is going on: see main). */
static void make_thumbnail(struct item *item) {
    {
        item->thumb_tried = true;
        char path[800];
        item_path(path, sizeof(path), item);
        struct vx_image *full = vx_image_load(path, VX_IMAGE_ALPHA);
        if (full) {
            int w = full->surface.width, h = full->surface.height;
            int tw = w >= h ? 56 : 56 * w / h, th = w >= h ? 56 * h / w : 56;
            tw = tw < 1 ? 1 : tw;
            th = th < 1 ? 1 : th;
            struct vx_image *thumb = malloc(sizeof(*thumb));
            uint32_t *pixels = thumb ? calloc((size_t)tw * th, 4) : NULL;
            if (pixels) {
                thumb->surface = (struct vx_surface){pixels, tw, th, tw};
                vx_blit_scaled(&thumb->surface, 0, 0, tw, th, &full->surface);
                for (long i = 0; i < (long)tw * th; i++) {
                    pixels[i] |= VX_IMAGE_ALPHA; /* (Scaled pixels keep alpha; show them solid.) */
                }
                item->thumb = thumb;
            } else {
                free(thumb);
            }
            vx_image_free(full);
        }
    }
}

/* The first picture in view without a thumbnail yet, or NULL. */
static struct item *next_thumbnail(void) {
    if (!icon_view || mode != NORMAL) {
        return NULL;
    }
    for (int i = top * columns(); i < shown_count && i < (top + visible_rows()) * columns(); i++) {
        if (wants_thumbnail(&items[shown[i]])) {
            return &items[shown[i]];
        }
    }
    return NULL;
}

static void draw_link_badge(struct vx_surface *s, int x, int y) {
    vx_fill(s, x, y, 9, 9, 0xffffff);
    vx_fill(s, x + 2, y + 5, 4, 2, 0x202020);
    vx_fill(s, x + 4, y + 2, 2, 5, 0x202020);
    vx_fill(s, x + 3, y + 2, 4, 1, 0x202020);
}

static void short_size(char *out, size_t size, const struct item *item) {
    if (item->kind == K_FOLDER || item->kind == K_APP || item->kind == K_DEVICE) {
        snprintf(out, size, "--");
    } else if (item->size < 1024) {
        snprintf(out, size, "%lu B", (unsigned long)item->size);
    } else if (item->size < 1024 * 1024) {
        snprintf(out, size, "%lu KiB", (unsigned long)(item->size / 1024));
    } else {
        snprintf(out, size, "%lu MiB", (unsigned long)(item->size / (1024 * 1024)));
    }
}

/* The name as shown: apps without ".vxapp", as Finder shows them. */
static void shown_name(char *out, size_t size, const struct item *item) {
    snprintf(out, size, "%s", item->name);
    if (item->app && ends_with(out, VX_APP_EXTENSION)) {
        out[strlen(out) - strlen(VX_APP_EXTENSION)] = '\0';
    }
}

static int drop_position = -1; /* A folder a drag would drop into, or -1. */

static void draw_list(struct vx_surface *s) {
    int x0 = main_x(), w = main_w();
    /* Headings: a click sorts; the sorted one has an arrow. */
    /* (Aqua's list headings: the sorted column's is a gel of the accent.) */
    vx_draw_toolbar(s, x0, TOOLBAR + 2, w, HEADER);
    static const char *const headings[] = {"Name", "Kind", "Size", "Modified"};
    for (int c = 0; c < 4; c++) {
        char text[32];
        bool sorted = (int)sort_by == c;
        snprintf(text, sizeof(text), "%s%s", headings[c], sorted ? (sort_down ? " v" : " ^") : "");
        int left = c ? column_x(c) - 6 : x0, right = c < 3 ? column_x(c + 1) - 6 : x0 + w;
        if (sorted) {
            vx_fill(s, left, TOOLBAR + 2, right - left, HEADER - 1, VX_COLOR_ACCENT);
            for (int row = 0; row < HEADER - 1; row++) {
                vx_fill(s, left, TOOLBAR + 2 + row, right - left, 1,
                        vx_gel_color(VX_COLOR_ACCENT, row, HEADER - 1));
            }
        }
        if (c) {
            vx_fill(s, left, TOOLBAR + 4, 1, HEADER - 5, VX_COLOR_LINE);
        }
        vx_draw_text(s, column_x(c), TOOLBAR + 4, text, sorted ? 0xffffff : VX_COLOR_TEXT,
                     VX_TRANSPARENT);
    }
    vx_fill(s, x0, list_y() - 1, w, 1, VX_COLOR_LINE);
    for (int i = top; i < shown_count && i < top + visible_rows(); i++) {
        struct item *item = &items[shown[i]];
        int x, y, cw, ch;
        cell(i, &x, &y, &cw, &ch);
        if (i % 2) {
            vx_fill(s, x, y, cw, ch, vx_theme.stripe); /* Stripes. */
        }
        if (i == drop_position) {
            vx_draw_gel(s, x + 2, y, cw - 4, ch, 6, VX_COLOR_ACCENT);
        } else if (item->selected) {
            vx_draw_selection(s, x + 2, y, cw - 4, ch);
        }
        struct vx_image *icon = icon_for(item);
        if (icon) {
            vx_blit_alpha(s, x + 7, y + 1, 18, 18, &icon->surface);
        }
        if (item->link) {
            draw_link_badge(s, x + 5, y + 11);
        }
        char name[256], size[32], date[32];
        shown_name(name, sizeof(name), item);
        short_size(size, sizeof(size), item);
        format_date(date, sizeof(date), item->modified);
        if (mode == RENAMING && shown[i] == renaming) {
            vx_draw_field(s, column_x(0) - 4, y - 2, column_x(1) - column_x(0) - 4, new_name, true);
        } else {
            vx_draw_text_fit(s, column_x(0), y + 2, column_x(1) - column_x(0) - 12, name,
                             VX_COLOR_TEXT, VX_TRANSPARENT);
        }
        vx_draw_text_fit(s, column_x(1), y + 2, COL_KIND - 8, kind_names[item->kind],
                         VX_COLOR_DIM, VX_TRANSPARENT);
        vx_draw_text(s, column_x(2), y + 2, size, VX_COLOR_DIM, VX_TRANSPARENT);
        vx_draw_text_fit(s, column_x(3), y + 2, COL_MODIFIED, date, VX_COLOR_DIM, VX_TRANSPARENT);
    }
}

static void draw_icons(struct vx_surface *s) {
    for (int i = top * columns(); i < shown_count; i++) {
        struct item *item = &items[shown[i]];
        int x, y, cw, ch;
        if (!cell(i, &x, &y, &cw, &ch)) {
            break;
        }
        bool hot = item->selected || i == drop_position;
        if (hot) { /* (Behind the icon: a soft rounded square.) */
            vx_fill_rounded(s, x + 22, y + 4, 60, 58, 10,
                            i == drop_position ? VX_COLOR_ACCENT : VX_COLOR_SELECTED, 200);
        }
        struct vx_image *thumb = item->thumb;
        if (thumb) {
            int tw = thumb->surface.width, th = thumb->surface.height;
            int tx = x + (cw - tw) / 2, ty = y + 5 + (56 - th) / 2;
            vx_fill(s, tx - 1, ty - 1, tw + 2, th + 2, 0xe6e7ea);
            vx_blit_alpha(s, tx, ty, tw, th, &thumb->surface);
        } else {
            struct vx_image *icon = icon_for(item);
            if (icon) {
                vx_blit_alpha(s, x + (cw - 48) / 2, y + 9, 48, 48, &icon->surface);
            }
        }
        if (item->link) {
            draw_link_badge(s, x + (cw - 48) / 2 + 2, y + 46);
        }
        char name[256];
        shown_name(name, sizeof(name), item);
        if (mode == RENAMING && shown[i] == renaming) {
            vx_draw_field(s, x + 2, y + 64, cw - 4, new_name, true);
            continue;
        }
        /* Two lines of the name, centered (the second ends in an ellipsis if
         * it's still too long). */
        const struct vx_font *font = vx_font_ui();
        const char *rest = name;
        for (int line = 0; line < 2 && *rest; line++) {
            char part[256];
            size_t n = vx_text_fit_bytes(font, rest, cw - 8);
            if (line == 0 && rest[n]) { /* Break after a space, if there's one. */
                size_t space = n;
                while (space > 0 && rest[space - 1] != ' ') {
                    space--;
                }
                n = space > 0 ? space : n;
            }
            if (line == 1 && rest[n]) {
                static const char ellipsis[] = "\xe2\x80\xa6";
                n = vx_text_fit_bytes(font, rest, cw - 8 - vx_text_width(ellipsis));
                snprintf(part, sizeof(part), "%.*s%s", (int)n, rest, ellipsis);
            } else {
                snprintf(part, sizeof(part), "%.*s", (int)n, rest);
            }
            rest += n ? n : strlen(rest);
            int tw = vx_text_width(part);
            int ty = y + 64 + line * (VX_LINE_HEIGHT + 1);
            if (item->selected) {
                vx_draw_gel(s, x + (cw - tw) / 2 - 5, ty - 1, tw + 10, VX_LINE_HEIGHT + 2,
                            (VX_LINE_HEIGHT + 2) / 2, VX_COLOR_ACCENT);
            }
            vx_draw_text(s, x + (cw - tw) / 2, ty, part, item->selected ? 0xffffff : VX_COLOR_TEXT,
                         VX_TRANSPARENT);
        }
    }
}

static void draw_sidebar(struct vx_surface *s) {
    int h = s->height;
    vx_fill(s, 0, TOOLBAR, SIDEBAR, h - TOOLBAR - STATUS, vx_theme.sidebar);
    vx_fill(s, SIDEBAR - 1, TOOLBAR, 1, h - TOOLBAR - STATUS, VX_COLOR_LINE);
    vx_draw_text(s, 12, TOOLBAR + 8, "Places", VX_COLOR_DIM, VX_TRANSPARENT);
    if (place_count > disks_start) {
        vx_draw_text(s, 12, place_y(disks_start) - 22, "Disks", VX_COLOR_DIM, VX_TRANSPARENT);
    }
    for (int i = 0; i < place_count; i++) {
        int y = place_y(i);
        bool here = !strcmp(cwd, places[i].path);
        bool lit = here || i == drop_place;
        if (lit) { /* (The place you're in: a gel of the accent, as in Finder.) */
            vx_draw_gel(s, 4, y, SIDEBAR - 9, 22, 7, VX_COLOR_ACCENT);
        }
        if (*places[i].icon) {
            vx_blit_alpha(s, 12, y + 2, 18, 18, &(*places[i].icon)->surface);
        }
        vx_draw_text_fit(s, 36, y + 3, SIDEBAR - 44, places[i].label, lit ? 0xffffff : VX_COLOR_TEXT,
                         VX_TRANSPARENT);
    }
}

static void draw_toolbar(struct vx_surface *s, int hot) {
    static const char *const labels[] = {"<", ">", "^", "List", "Icons"};
    vx_draw_toolbar(s, 0, 0, s->width, TOOLBAR);
    for (int b = 0; b < BUTTON_COUNT; b++) {
        int x, w;
        button_rect(b, &x, &w);
        bool on = (b == B_LIST && !icon_view) || (b == B_ICONS && icon_view);
        bool off = (b == B_BACK && !back_count) || (b == B_FORWARD && !forward_count) ||
                   (b == B_UP && !strcmp(cwd, "/"));
        vx_draw_button_flags(s, x, 8, w, 24, labels[b],
                             (hot == b || on ? VX_BUTTON_HOT : 0) | (off ? VX_BUTTON_DISABLED : 0));
    }
    vx_draw_field(s, path_x(), 8, path_w(), field == FIELD_PATH ? typed : cwd, field == FIELD_PATH);
    vx_draw_field(s, search_x(), 8, 160, search, field == FIELD_SEARCH);
    if (!search[0] && field != FIELD_SEARCH) {
        vx_draw_text(s, search_x() + 6, 12, "Search", VX_COLOR_DIM, VX_TRANSPARENT);
    }
}

static void draw_status(struct vx_surface *s) {
    char text[240];
    int h = s->height, count = selected_count();
    if (message[0] && vx_uptime() - message_ms < 5000) {
        snprintf(text, sizeof(text), "%s", message);
    } else if (count) {
        snprintf(text, sizeof(text), "%d of %d selected", count, shown_count);
    } else {
        snprintf(text, sizeof(text), "%d item%s%s%s", shown_count, shown_count == 1 ? "" : "s",
                 search[0] ? " matching " : "", search);
    }
    vx_draw_toolbar(s, 0, h - STATUS, s->width, STATUS);
    vx_fill(s, 0, h - STATUS, s->width, 1, VX_COLOR_LINE);
    vx_draw_text_fit(s, 10, h - STATUS + 3, s->width - 20, text, VX_COLOR_DIM, VX_TRANSPARENT);
}

/* A panel in the middle of the window (dialogs). */
static void panel_rect(int width, int height, int *x, int *y) {
    *x = (window->surface.width - width) / 2;
    *y = (window->surface.height - height) / 2;
    *y = *y < TOOLBAR ? TOOLBAR : *y;
}

static void panel(int width, int height, int *x, int *y) {
    struct vx_surface *s = &window->surface;
    panel_rect(width, height, x, y);
    vx_draw_sheet(s, *x, *y, width, height);
}

/* A dialog's buttons, right-aligned at its bottom (0 the rightmost). */
static void dialog_button(int px, int py, int pw, int ph, int i, int *x, int *y) {
    *x = px + pw - 16 - (i + 1) * 96 + 8;
    *y = py + ph - 38;
}

#define CONFIRM_W 400
#define CONFIRM_H 120
#define INFO_W 460

static int info_h(void) {
    return 96 + info_count * (VX_LINE_HEIGHT + 4);
}

static void draw_confirm(struct vx_surface *s) {
    int x, y, bx, by;
    panel(CONFIRM_W, CONFIRM_H, &x, &y);
    char text[300];
    int count = selected_count();
    if (question == ASK_EMPTY_TRASH) {
        snprintf(text, sizeof(text), "Empty the Trash? Its %d item%s go for good.", shown_count,
                 shown_count == 1 ? "" : "s");
    } else if (count == 1 && at(cursor)) {
        snprintf(text, sizeof(text), "Delete \"%s\" for good?", at(cursor)->name);
    } else {
        snprintf(text, sizeof(text), "Delete these %d items for good?", count);
    }
    vx_draw_text_fit(s, x + 16, y + 20, CONFIRM_W - 32, text, VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_text(s, x + 16, y + 44, "This can't be undone.", VX_COLOR_DIM, VX_TRANSPARENT);
    dialog_button(x, y, CONFIRM_W, CONFIRM_H, 0, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, question == ASK_EMPTY_TRASH ? "Empty" : "Delete", false);
    dialog_button(x, y, CONFIRM_W, CONFIRM_H, 1, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, "Cancel", false);
}

static void draw_info(struct vx_surface *s) {
    int x, y, bx, by, h = info_h();
    panel(INFO_W, h, &x, &y);
    if (info_icon) {
        vx_blit_alpha(s, x + 16, y + 16, 48, 48, &info_icon->surface);
    }
    for (int i = 0; i < info_count; i++) {
        int ly = y + 16 + i * (VX_LINE_HEIGHT + 4);
        vx_draw_text(s, x + 80, ly, info_labels[i], VX_COLOR_DIM, VX_TRANSPARENT);
        vx_draw_text_fit(s, x + 80 + 96, ly, INFO_W - 96 - 96,
                         info_values[i], i == 0 ? VX_COLOR_ACCENT : VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    dialog_button(x, y, INFO_W, h, 0, &bx, &by);
    vx_draw_button(s, bx, by, 88, 26, "OK", false);
}

static void draw_preview(struct vx_surface *s) {
    int w = s->width * 3 / 4, h = s->height * 3 / 4, x, y;
    panel(w, h, &x, &y);
    vx_draw_toolbar(s, x + 1, y + 1, w - 2, 26);
    vx_draw_text_fit(s, x + 10, y + 5, w - 20, preview_name, VX_COLOR_TEXT, VX_TRANSPARENT);
    int ax = x + 10, ay = y + 32, aw = w - 20, ah = h - 42;
    if (preview_image) {
        int iw = preview_image->surface.width, ih = preview_image->surface.height;
        int dw = iw, dh = ih; /* Fit, never bigger than it is. */
        if (dw > aw) {
            dh = dh * aw / dw, dw = aw;
        }
        if (dh > ah) {
            dw = dw * ah / dh, dh = ah;
        }
        vx_blit_scaled(s, ax + (aw - dw) / 2, ay + (ah - dh) / 2, dw, dh, &preview_image->surface);
    } else if (preview_text) {
        vx_fill(s, ax, ay, aw, ah, VX_COLOR_VIEW);
        int line_y = ay + 4, col = 0;
        char line[256];
        for (const char *p = preview_text;; p++) {
            if (*p == '\n' || !*p || col >= 255) {
                line[col] = '\0';
                vx_draw_text_fit(s, ax + 4, line_y, aw - 8, line, VX_COLOR_TEXT, VX_TRANSPARENT);
                line_y += VX_LINE_HEIGHT;
                col = 0;
                if (!*p || line_y + VX_LINE_HEIGHT > ay + ah) {
                    break;
                }
                if (*p != '\n') {
                    while (*p && *p != '\n') {
                        p++; /* The rest of a long line. */
                    }
                    if (!*p) {
                        break;
                    }
                }
                continue;
            }
            if (*p == '\t') {
                do {
                    line[col++] = ' ';
                } while (col % 4 && col < 255);
            } else if ((unsigned char)*p >= ' ' && *p != 127) {
                line[col++] = *p;
            }
        }
    } else {
        struct item *item = at(cursor);
        struct vx_image *icon = item ? icon_for(item) : NULL;
        if (icon) {
            vx_blit_alpha(s, x + (w - 96) / 2, y + (h - 96) / 2 - 10, 96, 96, &icon->surface);
        }
        const char *note = "No preview for this kind of file.";
        vx_draw_text(s, x + (w - vx_text_width(note)) / 2, y + h / 2 + 50, note,
                     VX_COLOR_DIM, VX_TRANSPARENT);
    }
}

/* ---- The context menu ---- */

enum action {
    ACT_OPEN, ACT_SHOW_CONTENTS, ACT_OPEN_IN_EDITOR, ACT_QUICK_LOOK, ACT_INFO, ACT_RENAME,
    ACT_DUPLICATE, ACT_ALIAS, ACT_COPY, ACT_CUT, ACT_PASTE, ACT_TRASH, ACT_DELETE,
    ACT_NEW_FOLDER, ACT_NEW_FILE, ACT_TERMINAL, ACT_EMPTY_TRASH, ACT_SHOW_HIDDEN, ACT_RESTORE,
    ACT_OPEN_WITH, ACT_ALWAYS_OPEN_WITH, ACT_WITH_APP, ACT_EXTRACT, ACT_COMPRESS, ACT_NONE
};

#define MAX_MENU 24
static struct vx_menu_item menu[MAX_MENU];
static enum action menu_actions[MAX_MENU];
static int menu_count, menu_x, menu_y, menu_hot = -1;
static int menu_hit;                      /* The item act() was chosen by. */
static struct vx_app menu_apps[MAX_MENU]; /* ACT_WITH_APP: the app of each item. */
static bool menu_always;                  /* ...and it becomes the default. */

static void add(const char *label, const char *keys, enum action action, bool disabled) {
    if (menu_count < MAX_MENU) {
        menu[menu_count] = (struct vx_menu_item){label, keys, disabled};
        menu_actions[menu_count++] = action;
    }
}

/* ---- Archives: archive (the command) does the work ---- */

static bool is_archive(const char *name) {
    static const char *const endings[] = {".zip", ".tar.gz", ".tgz", ".tar", ".gz"};
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); i++) {
        size_t e = strlen(endings[i]);
        if (n > e && !strcasecmp(name + n - e, endings[i])) {
            return true;
        }
    }
    return false;
}

/* Extract Here: into a folder named after the archive, next to it. */
static void extract_selection(void) {
    struct item *item = at(cursor);
    if (!item) {
        return;
    }
    char path[800], text[900];
    item_path(path, sizeof(path), item);
    const char *argv[] = {"/bin/archive", "extract", path};
    snprintf(text, sizeof(text), "extracting %s", item->name);
    say(text);
    started(run_argv(argv, 3), "archive");
}

/* Compress: the selection into a zip here, named after it (one thing) or
 * "Archive.zip". */
static void compress_selection(void) {
    static char paths[MAX_CLIP][512];
    int n = selected_paths(paths, MAX_CLIP);
    if (!n) {
        return;
    }
    char base[300], name[300], out[800], text[900];
    snprintf(base, sizeof(base), "%s.zip", n == 1 ? base_name(paths[0]) : "Archive");
    vx_unique_name(cwd, base, name, sizeof(name));
    vx_join_path(out, sizeof(out), cwd, name);
    const char *argv[MAX_CLIP + 3] = {"/bin/archive", "create", out};
    for (int i = 0; i < n; i++) {
        argv[3 + i] = paths[i];
    }
    snprintf(text, sizeof(text), "compressing into %s", name);
    say(text);
    started(run_argv(argv, n + 3), "archive");
}

static void open_menu(int x, int y) {
    bool paste_ok = can_paste();
    int count = selected_count();
    menu_count = 0;
    if (count) {
        struct item *item = at(cursor);
        add("Open", "Enter", ACT_OPEN, false);
        if (count == 1 && item && item->app) {
            add("Show Package Contents", NULL, ACT_SHOW_CONTENTS, false);
        } else if (count == 1 && item && item->kind != K_FOLDER) {
            add("Open With", NULL, ACT_OPEN_WITH, false);
            add("Always Open With", NULL, ACT_ALWAYS_OPEN_WITH, false);
        }
        if (count == 1 && item && is_archive(item->name)) {
            add("Extract Here", NULL, ACT_EXTRACT, false);
        }
        if (!in_trash()) {
            add("Compress", NULL, ACT_COMPRESS, false);
        }
        add("Quick Look", "Space", ACT_QUICK_LOOK, count != 1);
        add(NULL, NULL, ACT_NONE, false);
        add("Get Info", "Ctrl+I", ACT_INFO, false);
        add("Rename", "F2", ACT_RENAME, count != 1);
        add("Duplicate", "Ctrl+D", ACT_DUPLICATE, false);
        add("Make Alias", NULL, ACT_ALIAS, false);
        add(NULL, NULL, ACT_NONE, false);
        add("Copy", "Ctrl+C", ACT_COPY, false);
        add("Cut", "Ctrl+X", ACT_CUT, false);
        add("Paste", "Ctrl+V", ACT_PASTE, !paste_ok);
        add(NULL, NULL, ACT_NONE, false);
        if (in_trash()) {
            add("Put Back", NULL, ACT_RESTORE, false);
            add("Delete Immediately", "Delete", ACT_DELETE, false);
        } else {
            add("Move to Trash", "Delete", ACT_TRASH, false);
        }
    } else {
        add("New Folder", "Ctrl+Shift+N", ACT_NEW_FOLDER, false);
        add("New Text Document", NULL, ACT_NEW_FILE, false);
        add("Paste", "Ctrl+V", ACT_PASTE, !paste_ok);
        add(NULL, NULL, ACT_NONE, false);
        add("Get Info", "Ctrl+I", ACT_INFO, false);
        add("Open in Terminal", NULL, ACT_TERMINAL, false);
        add(show_hidden ? "Hide Hidden Files" : "Show Hidden Files", "Ctrl+H", ACT_SHOW_HIDDEN, false);
        if (in_trash()) {
            add(NULL, NULL, ACT_NONE, false);
            add("Empty Trash", NULL, ACT_EMPTY_TRASH, shown_count == 0);
        }
    }
    int w, h;
    vx_menu_size(menu, menu_count, &w, &h);
    menu_x = x + w > window->surface.width - 4 ? window->surface.width - w - 4 : x;
    menu_y = y + h > window->surface.height - 4 ? window->surface.height - h - 4 : y;
    menu_x = menu_x < 0 ? 0 : menu_x;
    menu_y = menu_y < 0 ? 0 : menu_y;
    menu_hot = -1;
    mode = MENU;
    printf("files: menu for %s\n", count == 1 && at(cursor) ? at(cursor)->name
                                   : count ? "the selection" : cwd);
    fflush(stdout);
}

/* The file's extension, in lowercase ("" if none). */
static void extension_of(const char *name, char *out, size_t size) {
    const char *dot = strrchr(name, '.');
    out[0] = '\0';
    if (dot && dot != name && strlen(dot + 1) < size) {
        size_t i = 0;
        for (; dot[1 + i]; i++) {
            out[i] = (char)tolower((unsigned char)dot[1 + i]);
        }
        out[i] = '\0';
    }
}

static bool has_word(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *p = list; (p = strstr(p, word)); p += n) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) {
            return true;
        }
    }
    return false;
}

/* Open With: the apps for this kind of file (then those that open
 * anything); Always Open With makes the one chosen its default. */
static void open_apps_menu(bool always) {
    struct item *item = at(cursor);
    static struct vx_app list[64];
    int count = vx_app_list(list, 64);
    char extension[32];
    extension_of(item ? item->name : "", extension, sizeof(extension));
    menu_count = 0;
    menu_always = always;
    add(always ? "Always open with:" : "Open with:", NULL, ACT_NONE, true);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count && menu_count < MAX_MENU; i++) {
            bool fits = pass == 0 ? extension[0] && has_word(list[i].opens, extension)
                                  : has_word(list[i].opens, "*") &&
                                        !(extension[0] && has_word(list[i].opens, extension));
            if (fits && !list[i].is_linux) {
                menu_apps[menu_count] = list[i];
                add(menu_apps[menu_count].name, NULL, ACT_WITH_APP, false);
            }
        }
    }
    int w, h;
    vx_menu_size(menu, menu_count, &w, &h);
    menu_x = menu_x + w > window->surface.width - 4 ? window->surface.width - w - 4 : menu_x;
    menu_y = menu_y + h > window->surface.height - 4 ? window->surface.height - h - 4 : menu_y;
    menu_x = menu_x < 0 ? 0 : menu_x;
    menu_y = menu_y < 0 ? 0 : menu_y;
    menu_hot = -1;
    mode = MENU;
}

static void open_with_app(struct item *item, const struct vx_app *app) {
    char path[800], text[1000];
    item_path(path, sizeof(path), item);
    char extension[32];
    extension_of(item->name, extension, sizeof(extension));
    if (menu_always && extension[0]) {
        /* The bundle's name ("Viewer" for /apps/Viewer.vxapp), as Settings
         * keeps it. */
        char stem[64];
        snprintf(stem, sizeof(stem), "%s", base_name(app->bundle));
        char *dot = strstr(stem, ".vxapp");
        if (dot) {
            *dot = '\0';
        }
        struct vx_settings chosen;
        vx_settings_load(&chosen, VX_APP_DEFAULTS);
        vx_settings_set(&chosen, extension, stem);
        vx_settings_save(&chosen);
        snprintf(text, sizeof(text), ".%s files open with %s now", extension, app->name);
        say(text);
    }
    snprintf(text, sizeof(text), "opened %s with %s", item->name, app->name);
    say(text);
    started(vx_app_open(app, path), app->name);
}

static void toggle_hidden(void) {
    show_hidden = !show_hidden;
    load();
}

static void act(enum action action) {
    mode = NORMAL;
    struct item *item = at(cursor);
    switch (action) {
    case ACT_OPEN: open_selection(); break;
    case ACT_SHOW_CONTENTS:
        if (item) {
            char path[800];
            item_path(path, sizeof(path), item);
            go(path);
        }
        break;
    case ACT_OPEN_IN_EDITOR:
        if (item) {
            open_with(item, "Editor");
        }
        break;
    case ACT_OPEN_WITH: open_apps_menu(false); break;
    case ACT_ALWAYS_OPEN_WITH: open_apps_menu(true); break;
    case ACT_WITH_APP:
        if (item) {
            open_with_app(item, &menu_apps[menu_hit]);
        }
        break;
    case ACT_RESTORE: restore_selection(); break;
    case ACT_EXTRACT: extract_selection(); break;
    case ACT_COMPRESS: compress_selection(); break;
    case ACT_QUICK_LOOK: quick_look(); break;
    case ACT_INFO: show_info(); break;
    case ACT_RENAME: start_rename(); break;
    case ACT_DUPLICATE: duplicate(); break;
    case ACT_ALIAS: make_alias(); break;
    case ACT_COPY: copy_selection(false); break;
    case ACT_CUT: copy_selection(true); break;
    case ACT_PASTE: paste(); break;
    case ACT_TRASH: trash_selection(); break;
    case ACT_DELETE: ask(ASK_DELETE); break;
    case ACT_NEW_FOLDER: new_thing(true); break;
    case ACT_NEW_FILE: new_thing(false); break;
    case ACT_TERMINAL: run("/bin/term", NULL); break; /* It starts here: our folder. */
    case ACT_EMPTY_TRASH: ask(ASK_EMPTY_TRASH); break;
    case ACT_SHOW_HIDDEN: toggle_hidden(); break;
    case ACT_NONE: break;
    }
}

/* ---- Dragging ---- */

static enum { DRAG_NONE, DRAG_MAYBE, DRAG_ON } drag;
static int drag_x, drag_y, pointer_x, pointer_y;

static long reload_at = -1; /* Read the folder again then (something else changed it). */
static void find_drop_target(int px, int py);

/* Files dropped on the window from somewhere else (the desktop, another
 * Files): into the folder or place under the pointer, or this folder. */
static void dropped(const struct vx_gui_event *e) {
    static char paths[MAX_CLIP][512];
    char *text = vx_drop_paths(e);
    vx_remove(e->text);
    if (!text) {
        return;
    }
    int n = 0;
    for (char *line = text, *next; line && *line && n < MAX_CLIP; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (line[0] == '/') {
            snprintf(paths[n++], sizeof(paths[0]), "%s", line);
        }
    }
    free(text);
    find_drop_target(e->x, e->y);
    char into[512];
    if (drop_place >= 0) {
        snprintf(into, sizeof(into), "%s", places[drop_place].path);
    } else if (drop_position >= 0) {
        item_path(into, sizeof(into), at(drop_position));
    } else {
        snprintf(into, sizeof(into), "%s", cwd);
    }
    drop_place = drop_position = -1;
    vx_mkdir(into);
    int done = transfer(paths, n, into, !e->value, NULL);
    printf("files: dropped %d item%s into %s\n", done, done == 1 ? "" : "s", into);
    load();
}

static void drop(void) {
    static char paths[MAX_CLIP][512];
    char into[512];
    if (drop_place >= 0) {
        snprintf(into, sizeof(into), "%s", places[drop_place].path);
    } else if (drop_position >= 0) {
        item_path(into, sizeof(into), at(drop_position));
    } else {
        return;
    }
    int n = selected_paths(paths, MAX_CLIP);
    if (!strcmp(into, TRASH) && !ctrl) {
        trash_selection();
        return;
    }
    vx_mkdir(into); /* (The Trash, the first time.) */
    transfer(paths, n, into, !ctrl, NULL);
    load();
}

/* What the pointer is over while dragging: a folder or app's folder? No:
 * a folder that isn't selected, or a place. */
static void find_drop_target(int px, int py) {
    drop_place = place_at(px, py);
    drop_position = -1;
    if (drop_place < 0) {
        int position = position_at(px, py);
        struct item *item = at(position);
        if (item && item->kind == K_FOLDER && !item->selected) {
            drop_position = position;
        }
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_VIEW);
    if (icon_view) {
        draw_icons(s);
    } else {
        draw_list(s);
    }
    if (total_rows() > visible_rows()) { /* Where we are in a long folder. */
        int area = list_h(), bar = area * visible_rows() / total_rows();
        int y = (area - bar) * top / (total_rows() - visible_rows());
        vx_fill(s, s->width - 12, list_y() + y, 4, bar < 8 ? 8 : bar, VX_COLOR_LINE);
    }
    draw_sidebar(s);
    draw_toolbar(s, -1);
    draw_status(s);
    if (drag == DRAG_ON) { /* What's being dragged, by the pointer. */
        char text[64];
        int count = selected_count();
        snprintf(text, sizeof(text), "%s%d item%s", ctrl ? "+ " : "", count, count == 1 ? "" : "s");
        int tw = vx_text_width(text) + 12;
        vx_draw_gel(s, pointer_x + 14, pointer_y + 10, tw, VX_LINE_HEIGHT + 6,
                    (VX_LINE_HEIGHT + 6) / 2, VX_COLOR_ACCENT);
        vx_draw_text(s, pointer_x + 20, pointer_y + 13, text, 0xffffff, VX_TRANSPARENT);
    }
    if (mode == MENU) {
        vx_draw_menu(s, menu_x, menu_y, menu, menu_count, menu_hot);
    } else if (mode == CONFIRM) {
        draw_confirm(s);
    } else if (mode == INFO) {
        draw_info(s);
    } else if (mode == PREVIEW) {
        draw_preview(s);
    }
    vx_window_present(window, 0, 0, s->width, s->height);
}

/* ---- Keys ---- */

static char typeahead[32];
static long typeahead_ms;

/* Letters typed on the list go to the first name that starts with them. */
static void type_ahead(uint32_t c) {
    long now = vx_uptime();
    size_t n = now - typeahead_ms > 1000 ? 0 : strlen(typeahead);
    typeahead_ms = now;
    char bytes[4];
    int length_of_c = vx_utf8_encode(c, bytes);
    if (n + (size_t)length_of_c < sizeof(typeahead)) {
        memcpy(typeahead + n, bytes, (size_t)length_of_c);
        typeahead[n + (size_t)length_of_c] = '\0';
    }
    size_t length = strlen(typeahead);
    for (int i = 0; i < shown_count; i++) {
        const char *name = items[shown[i]].name;
        size_t k = 0;
        while (k < length && name[k] && same_letters(name[k], typeahead[k])) {
            k++;
        }
        if (k == length) {
            select_only(i);
            return;
        }
    }
}

static void field_key(const struct vx_gui_event *e) {
    char *text = field == FIELD_PATH ? typed : search;
    size_t size = field == FIELD_PATH ? sizeof(typed) : sizeof(search);
    if (e->key == VX_KEY_ENTER) {
        if (field == FIELD_PATH) {
            field = FIELD_NONE;
            go(typed);
        } else {
            field = FIELD_NONE; /* The search stays; Esc clears it. */
        }
    } else if (e->key == VX_KEY_ESC) {
        if (field == FIELD_SEARCH) {
            search[0] = '\0';
            arrange();
        }
        field = FIELD_NONE;
    } else if (vx_field_key(text, size, e) && field == FIELD_SEARCH) {
        arrange(); /* The search is as you type. */
        select_only(0);
    }
}

static void key(const struct vx_gui_event *e) {
    bool down = e->value != 0;
    switch (e->key) {
    case VX_KEY_LEFTCTRL:
    case VX_KEY_RIGHTCTRL: ctrl = down; return;
    case VX_KEY_LEFTSHIFT:
    case VX_KEY_RIGHTSHIFT: shift = down; return;
    case VX_KEY_LEFTALT:
    case VX_KEY_RIGHTALT: alt = down; return;
    }
    if (!down) {
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
    case PREVIEW:
        if (e->key == VX_KEY_SPACE || e->key == VX_KEY_ESC || e->key == VX_KEY_ENTER) {
            close_preview();
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
    if (field != FIELD_NONE) {
        field_key(e);
        return;
    }
    if (ctrl) {
        switch (e->key) {
        case 46: copy_selection(false); break; /* C */
        case 45: copy_selection(true); break;  /* X */
        case 47: paste(); break;               /* V */
        case 32: duplicate(); break;           /* D */
        case 23: show_info(); break;           /* I */
        case 35: toggle_hidden(); break;       /* H */
        case 30:                               /* A: everything */
            for (int i = 0; i < shown_count; i++) {
                items[shown[i]].selected = true;
            }
            break;
        case 33: field = FIELD_SEARCH; break; /* F */
        case 38:                              /* L: type a location */
            field = FIELD_PATH;
            typed[0] = '\0';
            break;
        case 2: icon_view = false, top = 0; break; /* 1 */
        case 3: icon_view = true, top = 0; break;  /* 2 */
        case 49:                                   /* N */
            if (shift) {
                new_thing(true);
            }
            break;
        }
        return;
    }
    if (alt) {
        if (e->key == VX_KEY_LEFT) {
            go_back();
        } else if (e->key == VX_KEY_RIGHT) {
            go_forward();
        } else if (e->key == VX_KEY_UP) {
            up();
        }
        return;
    }
    int per_row = icon_view ? columns() : 1;
    switch (e->key) {
    case VX_KEY_UP: move_cursor(cursor < 0 ? 0 : cursor - per_row); return;
    case VX_KEY_DOWN: move_cursor(cursor < 0 ? 0 : cursor + per_row); return;
    case VX_KEY_LEFT:
        if (icon_view) {
            move_cursor(cursor < 0 ? 0 : cursor - 1);
        }
        return;
    case VX_KEY_RIGHT:
        if (icon_view) {
            move_cursor(cursor < 0 ? 0 : cursor + 1);
        }
        return;
    case VX_KEY_PAGEUP: move_cursor(cursor - visible_rows() * per_row); return;
    case VX_KEY_PAGEDOWN: move_cursor(cursor + visible_rows() * per_row); return;
    case VX_KEY_HOME: move_cursor(0); return;
    case VX_KEY_END: move_cursor(shown_count - 1); return;
    case VX_KEY_ENTER: open_selection(); return;
    case VX_KEY_BACKSPACE: up(); return;
    case VX_KEY_SPACE:
        if (cursor >= 0) {
            quick_look();
        }
        return;
    case VX_KEY_F1 + 1: start_rename(); return; /* F2 */
    case VX_KEY_DELETE:
        if (selected_count()) {
            in_trash() ? ask(ASK_DELETE) : trash_selection();
        }
        return;
    case VX_KEY_ESC:
        clear_selection();
        cursor = anchor = -1;
        return;
    }
    if (e->character > ' ' && e->character != 127) {
        type_ahead((uint32_t)e->character);
    }
}

/* ---- The pointer ---- */

static long last_click_ms;
static int last_click = -1;

static bool dialog_click(int px, int py) {
    int w = mode == INFO ? INFO_W : CONFIRM_W, h = mode == INFO ? info_h() : CONFIRM_H;
    int x, y, bx, by;
    panel_rect(w, h, &x, &y);
    for (int i = 0; i < (mode == INFO ? 1 : 2); i++) {
        dialog_button(x, y, w, h, i, &bx, &by);
        if (vx_inside(px, py, bx, by, 88, 26)) {
            if (mode == INFO) {
                mode = NORMAL;
            } else {
                answer(i == 0);
            }
            return true;
        }
    }
    return false;
}

static void toolbar_click(int px, int py) {
    for (int b = 0; b < BUTTON_COUNT; b++) {
        int x, w;
        button_rect(b, &x, &w);
        if (vx_inside(px, py, x, 8, w, 24)) {
            switch (b) {
            case B_BACK: go_back(); break;
            case B_FORWARD: go_forward(); break;
            case B_UP: up(); break;
            case B_LIST: icon_view = false, top = 0; break;
            case B_ICONS: icon_view = true, top = 0; break;
            }
            return;
        }
    }
    if (vx_inside(px, py, path_x(), 8, path_w(), VX_LINE_HEIGHT + 8)) {
        field = FIELD_PATH;
        snprintf(typed, sizeof(typed), "%s", cwd);
    } else if (vx_inside(px, py, search_x(), 8, 160, VX_LINE_HEIGHT + 8)) {
        field = FIELD_SEARCH;
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    bool release = !(e->buttons & 1) && (*held & 1);
    bool right_click = (e->buttons & 2) && !(*held & 2);
    *held = e->buttons;
    pointer_x = e->x, pointer_y = e->y;
    bool on_field = mode == NORMAL && (vx_inside(e->x, e->y, path_x(), 8, path_w(), VX_LINE_HEIGHT + 8) ||
                                       vx_inside(e->x, e->y, search_x(), 8, 160, VX_LINE_HEIGHT + 8));
    vx_window_set_cursor(window, on_field ? VX_CURSOR_TEXT : VX_CURSOR_ARROW);
    if (mode == MENU) {
        menu_hot = vx_menu_item_at(menu, menu_count, menu_x, menu_y, e->x, e->y);
        if (click || right_click) {
            int hit = menu_hot;
            mode = NORMAL;
            if (hit >= 0) {
                menu_hit = hit;
                act(menu_actions[hit]);
            }
        }
        return;
    }
    if (mode == CONFIRM || mode == INFO) {
        if (click) {
            dialog_click(e->x, e->y);
        }
        return;
    }
    if (mode == PREVIEW) {
        if (click || right_click) {
            close_preview();
        }
        return;
    }
    if (mode == RENAMING && (click || right_click)) {
        finish_rename(); /* Clicking elsewhere keeps the new name, as in Finder. */
    }
    if (e->wheel) {
        scroll(-e->wheel * (icon_view ? 1 : 3));
    }
    /* Dragging the selection. */
    if (drag == DRAG_MAYBE && (e->buttons & 1) &&
        (abs(e->x - drag_x) > 4 || abs(e->y - drag_y) > 4)) {
        drag = DRAG_ON;
    }
    if (drag == DRAG_ON) {
        struct vx_surface *s = &window->surface;
        bool outside = e->x < 0 || e->y < 0 || e->x >= s->width || e->y >= s->height;
        find_drop_target(e->x, e->y);
        if (release && outside) {
            /* Out of the window: the desktop puts them where they were let go. */
            static char paths[MAX_CLIP][512];
            const char *list[MAX_CLIP];
            int n = selected_paths(paths, MAX_CLIP);
            for (int i = 0; i < n; i++) {
                list[i] = paths[i];
            }
            vx_window_drag_files(window, list, n, ctrl);
            printf("files: dragged %d item%s out\n", n, n == 1 ? "" : "s");
            reload_at = vx_uptime() + 700;
        } else if (release) {
            drop();
        }
        if (release) {
            drag = DRAG_NONE;
            drop_place = drop_position = -1;
        }
        return;
    }
    if (release) {
        drag = DRAG_NONE;
    }
    if (right_click) {
        field = FIELD_NONE;
        int position = position_at(e->x, e->y);
        if (position >= 0 && !items[shown[position]].selected) {
            select_only(position); /* A right click on something else selects it. */
        } else if (position < 0 && e->x >= main_x() && e->y > TOOLBAR) {
            clear_selection();
            cursor = anchor = -1;
        }
        if (e->x >= main_x() && e->y > TOOLBAR) {
            open_menu(e->x, e->y);
        }
        return;
    }
    if (!click) {
        return;
    }
    if (e->y < TOOLBAR) {
        field = FIELD_NONE;
        toolbar_click(e->x, e->y);
        return;
    }
    field = FIELD_NONE;
    if (e->x < SIDEBAR) {
        int p = place_at(e->x, e->y);
        if (p >= 0) {
            if (!strcmp(places[p].path, TRASH)) {
                vx_mkdir(TRASH); /* (It's made the first time something goes there.) */
            }
            go(places[p].path);
        }
        return;
    }
    if (!icon_view && e->y >= TOOLBAR + 2 && e->y < list_y()) { /* A heading: sort. */
        int c = 3;
        while (c > 0 && e->x < column_x(c)) {
            c--;
        }
        if ((int)sort_by == c) {
            sort_down = !sort_down;
        } else {
            sort_by = c;
            sort_down = false;
        }
        arrange();
        return;
    }
    int position = position_at(e->x, e->y);
    if (position < 0) {
        clear_selection(); /* A click on nothing. */
        cursor = anchor = -1;
        return;
    }
    long now = vx_uptime();
    if (position == last_click && now - last_click_ms < DOUBLE_CLICK_MS && !ctrl && !shift) {
        last_click = -1;
        select_only(position);
        open_item(at(position));
        return;
    }
    last_click = position;
    last_click_ms = now;
    if (ctrl) { /* Add or take away. */
        items[shown[position]].selected = !items[shown[position]].selected;
        cursor = anchor = position;
    } else if (shift) {
        select_to(position);
    } else {
        if (!items[shown[position]].selected) {
            select_only(position);
        }
        cursor = position;
        drag = DRAG_MAYBE; /* It may become a drag of the selection. */
        drag_x = e->x, drag_y = e->y;
    }
}

int main(int argc, char **argv) {
    window = vx_window_create_flags("Files", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "files: no desktop to open a window on\n");
        return 1;
    }
    snprintf(home_folder, sizeof(home_folder), "%s", vx_home());
    vx_home_path(desktop_folder, sizeof(desktop_folder), "Desktop");
    vx_home_path(documents_folder, sizeof(documents_folder), "Documents");
    vx_home_path(pictures_folder, sizeof(pictures_folder), "Pictures");
    vx_home_path(trash_folder, sizeof(trash_folder), VX_TRASH_NAME);
    snprintf(clipboard_file, sizeof(clipboard_file), "/tmp/.files-clipboard-%u", getuid());
    load_resources();
    read_settings();
    go_to(argc > 1 ? argv[1] : "/", false);
    int held = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        /* Thumbnails are made when there's nothing else to do; and an old
         * message goes from the status bar after a while. */
        struct item *wanting = next_thumbnail();
        long wait = wanting ? 0 : 1500; /* (Looking at the folder for changes.) */
        if (reload_at >= 0) {
            long left = reload_at - vx_uptime();
            wait = left < 0 ? 0 : wait < 0 || left < wait ? left : wait;
        }
        int got = vx_gui_wait(&e, wait);
        if (got < 0) {
            return 0;
        }
        if (got == 0 && reload_at >= 0 && vx_uptime() >= reload_at) {
            reload_at = -1;
            load();
            continue;
        }
        if (got == 0 && wanting) {
            make_thumbnail(wanting);
            continue;
        }
        if (got == 0) {
            if (message[0] && vx_uptime() - message_ms >= 5000) {
                message[0] = '\0';
            }
            if (mode == NORMAL && drag == DRAG_NONE && vx_uptime() - folder_checked_ms >= 1400) {
                refresh_if_changed();
            }
            continue;
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
        case VX_GUI_THEME:
            read_settings();
            break;
        case VX_GUI_DROP:
            dropped(&e);
            break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = shift = alt = false; /* Their key ups go to another window. */
            }
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 420 && e.height >= 240) {
                vx_window_resize(window, e.width, e.height);
                scroll(0);
            }
            break;
        }
    }
}
