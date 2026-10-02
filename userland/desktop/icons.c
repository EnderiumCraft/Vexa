/* The icons on the desktop: the apps that ask for one (or that Settings
 * chose), the files in the Desktop folder (/home/Desktop), as on a Mac,
 * and the Trash in the bottom right corner.
 *
 * A click selects an icon (Ctrl adds to the selection; dragging over the
 * desktop selects those inside), a double click opens it. Files' icons can
 * be dragged: to another place on the desktop, onto a folder or the Trash,
 * or into a window (Files puts them in its folder). Files dragged out of a
 * window onto the desktop go into the Desktop folder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/syscall.h>
#include "shell.h"

#define CELL_W 80
#define CELL_H 76
#define ICON_W 72
#define ICON_H 72
#define MAX_ICONS 96
#define POSITIONS DESKTOP_FOLDER "/.positions"
#define DRAG_START 5 /* Pixels the pointer moves before a press is a drag. */

enum icon_kind { ICON_APP, ICON_FILE, ICON_TRASH };

static struct icon {
    enum icon_kind kind;
    int app;              /* ICON_APP */
    char path[320];       /* ICON_FILE (and the Trash's folder) */
    char label[64];
    bool is_dir;
    int column, row;      /* Its place on the grid (ICON_TRASH: its own corner). */
    bool selected;
    struct vx_image *thumbnail; /* A picture's, made when first drawn. */
    bool thumbnail_tried;
} icons[MAX_ICONS];
static int icon_count;

static bool setting_icons = true;
static char setting_icon_apps[256];

/* What the left button is doing on the desktop. */
static enum { NOTHING, PRESSED, DRAGGING, SELECTING } action;
static int press_x, press_y, pressed_icon = -1;
static long last_click_ms;
static int last_click_icon = -1;
static int drop_target = -1; /* While dragging: the icon (a folder, the Trash) under the pointer. */

/* ---- Icons for files (Files' own pictures) ---- */

static struct vx_image *resources[12];
static const char *const resource_names[] = {
    "folder", "document", "text", "image", "program", "trash", "apps", NULL,
};

static struct vx_image *resource(int i) {
    if (!resources[i] && resource_names[i]) {
        char path[160];
        snprintf(path, sizeof(path), "%s/Files.vxapp/Contents/Resources/%s.png", VX_APPS_DIR,
                 resource_names[i]);
        resources[i] = vx_image_load(path, VX_IMAGE_ALPHA);
    }
    return resources[i];
}

static bool has_extension(const char *name, const char *list) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        return false;
    }
    size_t n = strlen(dot + 1);
    for (const char *p = list; *p;) {
        size_t m = strcspn(p, " ");
        if (m == n && !strncmp(p, dot + 1, n)) {
            return true;
        }
        p += m + (p[m] == ' ');
    }
    return false;
}

struct vx_image *app_named_icon(const char *stem) {
    for (int i = 0; i < app_count; i++) {
        const char *file = strrchr(apps[i].bundle, '/');
        size_t n = strlen(stem);
        if (file && !strncmp(file + 1, stem, n) && !strcmp(file + 1 + n, VX_APP_EXTENSION)) {
            return app_icons[i];
        }
    }
    return NULL;
}

struct vx_image *file_icon(const char *path, bool is_dir) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (has_extension(name, "vxapp")) {
        for (int i = 0; i < app_count; i++) {
            if (!strcmp(apps[i].bundle, path)) {
                return app_icons[i];
            }
        }
        return resource(6);
    }
    if (is_dir) {
        return resource(0);
    }
    if (has_extension(name, "png bmp ppm jpg jpeg gif")) {
        return resource(3);
    }
    if (has_extension(name, "txt md c h py sh conf log cfg ini json xml html css js")) {
        return resource(2);
    }
    return resource(1);
}

/* ---- The grid ---- */

static int rows_per_column(void) {
    int rows = (screen.height - PANEL_HEIGHT - 12 - 8) / CELL_H;
    return rows < 1 ? 1 : rows;
}

static struct rect icon_rect(int i) {
    if (icons[i].kind == ICON_TRASH) {
        return (struct rect){screen.width - ICON_W - 8, screen.height - ICON_H - 8, ICON_W, ICON_H};
    }
    return (struct rect){4 + icons[i].column * CELL_W, PANEL_HEIGHT + 12 + icons[i].row * CELL_H,
                         ICON_W, ICON_H};
}

struct rect icons_area(void) {
    return (struct rect){0, PANEL_HEIGHT, screen.width, screen.height - PANEL_HEIGHT};
}

static bool cell_taken(int column, int row, int except) {
    for (int i = 0; i < icon_count; i++) {
        if (i != except && icons[i].kind != ICON_TRASH && icons[i].column == column &&
            icons[i].row == row) {
            return true;
        }
    }
    return false;
}

/* The first free cell, going down the columns from the left. */
static void free_cell(int *column, int *row, int except) {
    int rows = rows_per_column();
    for (int c = 0;; c++) {
        for (int r = 0; r < rows; r++) {
            if (!cell_taken(c, r, except)) {
                *column = c;
                *row = r;
                return;
            }
        }
    }
}

static void save_positions(void) {
    char text[8192];
    size_t n = 0;
    for (int i = 0; i < icon_count && n < sizeof(text) - 400; i++) {
        if (icons[i].kind == ICON_FILE) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s\t%d\t%d\n",
                                  strrchr(icons[i].path, '/') + 1, icons[i].column, icons[i].row);
        }
    }
    int handle = vx_open(POSITIONS, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle >= 0) {
        vx_write(handle, text, n);
        vx_close(handle);
    }
}

/* Where a file's icon was put (from the positions file): false if nowhere. */
static bool saved_position(const char *positions, const char *name, int *column, int *row) {
    size_t n = strlen(name);
    for (const char *line = positions; line && *line;) {
        const char *tab = strchr(line, '\t');
        const char *end = strchr(line, '\n');
        if (tab && (!end || tab < end) && (size_t)(tab - line) == n && !strncmp(line, name, n)) {
            char *after;
            *column = (int)strtol(tab + 1, &after, 10);
            *row = (int)strtol(after, NULL, 10);
            return *column >= 0 && *row >= 0 && *row < rows_per_column();
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

/* ---- Reading the icons ---- */

static bool listed(const char *list, const struct vx_app *app) {
    const char *file = strrchr(app->bundle, '/');
    file = file ? file + 1 : app->bundle;
    size_t n = strlen(file) - strlen(VX_APP_EXTENSION);
    for (const char *p = list; *p;) {
        size_t m = strcspn(p, ",");
        if (m == n && !strncmp(p, file, n)) {
            return true;
        }
        p += m + (p[m] == ',');
    }
    return false;
}

static void free_icons(void) {
    for (int i = 0; i < icon_count; i++) {
        vx_image_free(icons[i].thumbnail);
    }
    icon_count = 0;
}

static unsigned long folder_seen;

static unsigned long folder_signature(void) {
    unsigned long hash = 5381;
    int handle = vx_open(DESKTOP_FOLDER, VX_OPEN_READ);
    if (handle < 0) {
        return 1;
    }
    struct vx_dir_entry entries[16];
    long n;
    while ((n = vx_read_dir(handle, entries, 16)) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned long h = 5381;
            for (const char *c = entries[i].name; *c; c++) {
                h = h * 33 + (unsigned char)*c;
            }
            hash += h;
        }
    }
    vx_close(handle);
    return hash;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(((const struct icon *)a)->label, ((const struct icon *)b)->label);
}

void icons_load(void) {
    setting_icons = vx_settings_bool(&config, "desktop_icons", true);
    snprintf(setting_icon_apps, sizeof(setting_icon_apps), "%s",
             vx_settings_get(&config, "desktop_apps", ""));
    add_damage(icons_area());
    free_icons();
    int rows = rows_per_column();
    if (setting_icons) {
        for (int i = 0; i < app_count && icon_count < MAX_ICONS; i++) {
            if (setting_icon_apps[0] ? listed(setting_icon_apps, &apps[i]) : apps[i].desktop) {
                struct icon *icon = &icons[icon_count];
                memset(icon, 0, sizeof(*icon));
                icon->kind = ICON_APP;
                icon->app = i;
                /* Its bundle's name, as Files shows it ("Editor" for Editor.vxapp). */
                const char *slash = strrchr(apps[i].bundle, '/');
                snprintf(icon->label, sizeof(icon->label), "%s", slash ? slash + 1 : apps[i].bundle);
                char *dot = strrchr(icon->label, '.');
                if (dot) {
                    *dot = '\0';
                }
                icon->column = icon_count / rows;
                icon->row = icon_count % rows;
                icon_count++;
            }
        }
    }
    /* The Desktop folder's files, by name, where they were put. */
    int first_file = icon_count;
    int handle = vx_open(DESKTOP_FOLDER, VX_OPEN_READ);
    if (handle >= 0) {
        struct vx_dir_entry entries[16];
        long n;
        while ((n = vx_read_dir(handle, entries, 16)) > 0) {
            for (long i = 0; i < n && icon_count < MAX_ICONS - 1; i++) {
                if (entries[i].name[0] == '.') {
                    continue;
                }
                struct icon *icon = &icons[icon_count++];
                memset(icon, 0, sizeof(*icon));
                icon->kind = ICON_FILE;
                vx_join_path(icon->path, sizeof(icon->path), DESKTOP_FOLDER, entries[i].name);
                snprintf(icon->label, sizeof(icon->label), "%s", entries[i].name);
                icon->is_dir = entries[i].type == VX_TYPE_DIRECTORY;
                icon->column = icon->row = -1;
            }
        }
        vx_close(handle);
    }
    qsort(icons + first_file, (size_t)(icon_count - first_file), sizeof(icons[0]), compare_names);
    char positions[8192] = "";
    int ph = vx_open(POSITIONS, VX_OPEN_READ);
    if (ph >= 0) {
        long n = vx_read(ph, positions, sizeof(positions) - 1);
        positions[n > 0 ? n : 0] = '\0';
        vx_close(ph);
    }
    for (int i = first_file; i < icon_count; i++) {
        int column, row;
        if (saved_position(positions, icons[i].label, &column, &row) &&
            !cell_taken(column, row, i)) {
            icons[i].column = column;
            icons[i].row = row;
        }
    }
    for (int i = first_file; i < icon_count; i++) {
        if (icons[i].column < 0) {
            free_cell(&icons[i].column, &icons[i].row, i);
        }
    }
    if (setting_icons) {
        struct icon *trash = &icons[icon_count++];
        memset(trash, 0, sizeof(*trash));
        trash->kind = ICON_TRASH;
        snprintf(trash->path, sizeof(trash->path), "%s", TRASH_FOLDER);
        snprintf(trash->label, sizeof(trash->label), "Trash");
    }
    folder_seen = folder_signature();
    add_damage(icons_area());
}

static long checked_ms;

void icons_check(void) {
    long now = now_ms();
    if (now - checked_ms < 2000 || action != NOTHING) {
        return;
    }
    checked_ms = now;
    if (folder_signature() != folder_seen) {
        /* Keep what was selected. */
        char chosen[8][64];
        int n = 0;
        for (int i = 0; i < icon_count && n < 8; i++) {
            if (icons[i].selected) {
                snprintf(chosen[n++], sizeof(chosen[0]), "%s", icons[i].label);
            }
        }
        icons_load();
        for (int i = 0; i < icon_count; i++) {
            for (int k = 0; k < n; k++) {
                icons[i].selected |= !strcmp(icons[i].label, chosen[k]);
            }
        }
        printf("desktop: desktop folder changed: %d icons\n", icon_count);
    }
}

/* ---- Drawing ---- */

/* Up to two lines of a label, centered under the icon. */
static void draw_label(struct vx_surface *view, struct rect r, const char *label, bool selected) {
    const struct vx_font *f = vx_font_ui();
    const char *rest = label;
    for (int line = 0; line < 2 && *rest; line++) {
        char part[128];
        size_t n = vx_text_fit_bytes(f, rest, CELL_W - 6);
        if (line == 0 && rest[n]) {
            size_t space = n;
            while (space > 0 && rest[space - 1] != ' ') {
                space--;
            }
            n = space ? space : n;
        }
        if (line == 1 && rest[n]) {
            static const char ellipsis[] = "\xe2\x80\xa6";
            n = vx_text_fit_bytes(f, rest, CELL_W - 6 - vx_text_width(ellipsis));
            snprintf(part, sizeof(part), "%.*s%s", (int)n, rest, ellipsis);
        } else {
            snprintf(part, sizeof(part), "%.*s", (int)n, rest);
        }
        rest += n ? n : strlen(rest);
        while (*rest == ' ') {
            rest++;
        }
        int w = vx_text_width(part);
        int x = r.x + (r.width - w) / 2, y = r.y + 53 + line * 15;
        if (selected) {
            fill_rounded(view, (struct rect){x - 4, y - 1, w + 8, 17}, 5, vx_theme.accent);
        } else {
            /* A shadow keeps it readable on a picture. */
            vx_draw_text(view, x + 1, y + 1, part, 0x000000, VX_TRANSPARENT);
        }
        vx_draw_text(view, x, y, part, 0xffffff, VX_TRANSPARENT);
    }
}

static struct vx_image *picture_of(struct icon *icon) {
    if (icon->kind == ICON_APP) {
        return app_icons[icon->app];
    }
    if (icon->kind == ICON_TRASH) {
        return resource(5);
    }
    if (!icon->is_dir && has_extension(icon->label, "png bmp ppm") && !icon->thumbnail_tried) {
        icon->thumbnail_tried = true;
        struct vx_stat st;
        if (vx_stat(icon->path, &st) == 0 && st.size < 4 * 1024 * 1024) {
            icon->thumbnail = vx_image_load(icon->path, 0x202028);
        }
    }
    return icon->thumbnail ? NULL : file_icon(icon->path, icon->is_dir);
}

static void draw_icon(struct vx_surface *view, struct icon *icon, struct rect r, int alpha) {
    if (icon->selected || (action == DRAGGING && drop_target >= 0 && icon == &icons[drop_target])) {
        struct rect box = {r.x + (r.width - 56) / 2, r.y - 1, 56, 54};
        blend_rect(view, box, 0xffffff, 50);
    }
    int ix = r.x + (r.width - 48) / 2, iy = r.y + 3;
    struct vx_image *picture = picture_of(icon);
    if (icon->thumbnail) {
        /* A picture of the picture, its shape kept, with a white edge. */
        struct vx_surface *t = &icon->thumbnail->surface;
        int w = 48, h = t->width ? t->height * 48 / t->width : 48;
        if (h > 48) {
            h = 48;
            w = t->height ? t->width * 48 / t->height : 48;
        }
        struct rect place = {ix + (48 - w) / 2, iy + (48 - h) / 2, w, h};
        vx_fill(view, place.x - 2, place.y - 2, w + 4, h + 4, 0xffffff);
        blit_smooth(view, place, t, alpha);
    } else if (picture) {
        vx_blit_alpha(view, ix, iy, 48, 48, &picture->surface);
    } else {
        vx_fill(view, ix + 4, iy + 4, 40, 40, vx_theme.line);
    }
    draw_label(view, r, icon->label, icon->selected);
}

static struct rect band(void) {
    int x0 = press_x < pointer_x ? press_x : pointer_x;
    int y0 = press_y < pointer_y ? press_y : pointer_y;
    return (struct rect){x0, y0 < PANEL_HEIGHT ? PANEL_HEIGHT : y0,
                         abs(pointer_x - press_x) + 1, abs(pointer_y - press_y) + 1};
}

void icons_draw(struct vx_surface *view, int ox, int oy) {
    for (int i = 0; i < icon_count; i++) {
        struct rect r = icon_rect(i);
        r.x += ox, r.y += oy;
        draw_icon(view, &icons[i], r, 255);
    }
    if (action == SELECTING) {
        struct rect b = band();
        b.x += ox, b.y += oy;
        blend_rect(view, b, vx_theme.accent, 60);
        vx_draw_outline(view, b.x, b.y, b.width, b.height, vx_theme.accent);
    }
}

/* What's being dragged, by the pointer (drawn over the windows). */
void icons_draw_drag(struct vx_surface *view, int ox, int oy) {
    if (action != DRAGGING) {
        return;
    }
    int n = 0;
    for (int i = 0; i < icon_count; i++) {
        if (icons[i].selected && icons[i].kind == ICON_FILE) {
            struct rect r = icon_rect(i);
            r.x += pointer_x - press_x + ox;
            r.y += pointer_y - press_y + oy;
            struct vx_image *picture = picture_of(&icons[i]);
            if (picture) {
                vx_blit_alpha(view, r.x + (r.width - 48) / 2, r.y + 3, 48, 48, &picture->surface);
            }
            n++;
        }
    }
    if (n > 1) {
        char count[16];
        snprintf(count, sizeof(count), "%d", n);
        int w = vx_text_width(count) + 12;
        fill_rounded(view, (struct rect){pointer_x + 14 + ox, pointer_y + 4 + oy, w, 18}, 9, 0xff4d4d);
        vx_draw_text(view, pointer_x + 20 + ox, pointer_y + 5 + oy, count, 0xffffff, VX_TRANSPARENT);
    }
}

/* ---- Doing things ---- */

int icons_at(int x, int y) {
    for (int i = icon_count - 1; i >= 0; i--) {
        if (inside(icon_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static void open_icon(int i) {
    struct icon *icon = &icons[i];
    printf("desktop: starting %s\n", icon->label);
    if (icon->kind == ICON_APP) {
        run_app(icon->app);
    } else {
        open_path(icon->path);
    }
}

/* Moves (or copies) files into a folder; true if any went. */
static int move_into(const char *const *paths, int count, const char *folder, bool copy) {
    int moved = 0;
    vx_mkdir(folder);
    for (int i = 0; i < count; i++) {
        const char *name = strrchr(paths[i], '/');
        name = name ? name + 1 : paths[i];
        if (!strcmp(paths[i], folder) || !*name) {
            continue;
        }
        /* Already there: nothing to do. */
        char dir[320];
        snprintf(dir, sizeof(dir), "%.*s", (int)(name - 1 - paths[i]), paths[i]);
        if (!strcmp(dir[0] ? dir : "/", folder) && !copy) {
            continue;
        }
        char unique[256], to[600];
        vx_unique_name(folder, name, unique, sizeof(unique));
        vx_join_path(to, sizeof(to), folder, unique);
        long error = copy ? vx_copy_tree(paths[i], to) : vx_move(paths[i], to);
        if (error && !copy) {
            error = vx_copy_tree(paths[i], to); /* Can't be moved (read-only): a copy. */
        }
        if (error) {
            char note[200];
            snprintf(note, sizeof(note), "Desktop: couldn't put %s there (%s)", name,
                     vx_strerror((int)error));
            add_note(note);
        } else {
            printf("desktop: %s %s to %s\n", copy ? "copied" : "moved", paths[i], to);
            moved++;
        }
    }
    return moved;
}

static int selected_paths(const char **paths, int max) {
    int n = 0;
    for (int i = 0; i < icon_count && n < max; i++) {
        if (icons[i].selected && icons[i].kind == ICON_FILE) {
            paths[n++] = icons[i].path;
        }
    }
    return n;
}

static void reload_soon(void) {
    checked_ms = 0;
    folder_seen = 0;
    icons_check();
}

void icons_drop(const char *list, bool copy, int x, int y) {
    int handle = vx_open(list, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    char text[8192];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    vx_remove(list);
    text[n > 0 ? n : 0] = '\0';
    const char *paths[64];
    int count = 0;
    for (char *line = text, *next; line && *line && count < 64; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (line[0] == '/') {
            paths[count++] = line;
        }
    }
    /* Onto a folder or the Trash, or into the Desktop folder at the pointer. */
    int target = icons_at(x, y);
    const char *folder = DESKTOP_FOLDER;
    if (target >= 0 && (icons[target].kind == ICON_TRASH ||
                        (icons[target].kind == ICON_FILE && icons[target].is_dir))) {
        folder = icons[target].path;
    }
    vx_mkdir(HOME);
    int moved = move_into(paths, count, folder, copy && strcmp(folder, TRASH_FOLDER));
    if (moved && !strcmp(folder, DESKTOP_FOLDER)) {
        /* The first where it was dropped. */
        int column = (x - 4) / CELL_W, row = (y - PANEL_HEIGHT - 12) / CELL_H;
        row = row < 0 ? 0 : row >= rows_per_column() ? rows_per_column() - 1 : row;
        column = column < 0 ? 0 : column;
        const char *base = strrchr(paths[0], '/') + 1;
        reload_soon();
        for (int i = 0; i < icon_count; i++) {
            if (icons[i].kind == ICON_FILE && !strcmp(icons[i].label, base) &&
                !cell_taken(column, row, i)) {
                icons[i].column = column;
                icons[i].row = row;
            }
        }
        save_positions();
        add_damage(icons_area());
    } else {
        reload_soon();
    }
}

void icons_new_folder(void) {
    vx_mkdir(HOME);
    vx_mkdir(DESKTOP_FOLDER);
    char name[256], path[400];
    vx_unique_name(DESKTOP_FOLDER, "untitled folder", name, sizeof(name));
    vx_join_path(path, sizeof(path), DESKTOP_FOLDER, name);
    long error = vx_mkdir(path);
    printf("desktop: new folder %s%s\n", path, error ? " failed" : "");
    reload_soon();
}

static void select_only(int i) {
    for (int k = 0; k < icon_count; k++) {
        icons[k].selected = k == i;
    }
    add_damage(icons_area());
}

/* The right-click menu of an icon. */
enum { MENU_OPEN, MENU_SHOW, MENU_TRASH, MENU_EMPTY };

int icons_menu_items(int icon, const char **labels, int max) {
    (void)max;
    if (icon < 0 || icon >= icon_count) {
        return 0;
    }
    if (!icons[icon].selected) {
        select_only(icon);
    }
    switch (icons[icon].kind) {
    case ICON_APP:
        labels[0] = "Open";
        labels[1] = "Show in Files";
        return 2;
    case ICON_FILE:
        labels[0] = "Open";
        labels[1] = "Show in Files";
        labels[2] = "Move to Trash";
        return 3;
    case ICON_TRASH:
        labels[0] = "Open";
        labels[1] = "Empty Trash";
        return 2;
    }
    return 0;
}

void icons_menu(int icon, int item) {
    if (icon < 0 || icon >= icon_count) {
        return;
    }
    struct icon *i = &icons[icon];
    if (item == 0) {
        open_icon(icon);
    } else if (i->kind == ICON_TRASH) {
        long error = vx_remove_tree(TRASH_FOLDER);
        vx_mkdir(TRASH_FOLDER);
        add_note(error ? "Trash: couldn't empty it" : "Trash: emptied");
    } else if (item == 1) {
        run_named("Files", i->kind == ICON_APP ? VX_APPS_DIR : DESKTOP_FOLDER);
    } else if (item == 2) {
        const char *paths[64];
        int n = selected_paths(paths, 64);
        move_into(paths, n, TRASH_FOLDER, false);
        reload_soon();
    }
}

bool icons_dragging(void) {
    return action == DRAGGING || action == SELECTING;
}

bool icons_button(int bit, bool down, int double_click_ms) {
    if (bit != 1) {
        return false;
    }
    if (down) {
        int hit = icons_at(pointer_x, pointer_y);
        long now = now_ms();
        if (hit >= 0 && hit == last_click_icon && now - last_click_ms < double_click_ms) {
            last_click_icon = -1;
            open_icon(hit);
            action = NOTHING;
            return true;
        }
        last_click_icon = hit;
        last_click_ms = now;
        press_x = pointer_x;
        press_y = pointer_y;
        pressed_icon = hit;
        if (hit >= 0) {
            if (ctrl) {
                icons[hit].selected = !icons[hit].selected;
            } else if (!icons[hit].selected) {
                select_only(hit);
            }
            add_damage(icons_area());
            action = PRESSED;
        } else {
            if (!ctrl) {
                select_only(-1);
            }
            action = SELECTING;
        }
        return true;
    }
    /* Let go. */
    if (action == SELECTING) {
        action = NOTHING;
        add_damage(icons_area());
        return true;
    }
    if (action == PRESSED) {
        action = NOTHING;
        if (!ctrl && pressed_icon >= 0) {
            select_only(pressed_icon);
        }
        return true;
    }
    if (action != DRAGGING) {
        return false;
    }
    action = NOTHING;
    damage_all();
    const char *paths[64];
    int n = selected_paths(paths, 64);
    struct window *w = window_at(pointer_x, pointer_y);
    if (w) {
        /* Into the window: its program puts them where it likes. */
        char list[64];
        snprintf(list, sizeof(list), "/tmp/.drag-desktop-%ld", now_ms());
        int handle = vx_open(list, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
        if (handle >= 0) {
            for (int i = 0; i < n; i++) {
                vx_write(handle, paths[i], strlen(paths[i]));
                vx_write(handle, "\n", 1);
            }
            vx_close(handle);
            drop_on_window(w, list, ctrl);
        }
        return true;
    }
    if (drop_target >= 0) {
        printf("desktop: dropped %d item%s on %s\n", n, n == 1 ? "" : "s", icons[drop_target].label);
        move_into(paths, n, icons[drop_target].path, ctrl && icons[drop_target].kind != ICON_TRASH);
        drop_target = -1;
        reload_soon();
        return true;
    }
    /* Somewhere else on the desktop: the icons move there (by the grid). */
    int dx = pointer_x - press_x, dy = pointer_y - press_y;
    int moved_count = 0;
    for (int i = 0; i < icon_count; i++) {
        if (!icons[i].selected || icons[i].kind != ICON_FILE) {
            continue;
        }
        struct rect r = icon_rect(i);
        int column = (r.x + dx + CELL_W / 2 - 4) / CELL_W;
        int row = (r.y + dy + CELL_H / 2 - PANEL_HEIGHT - 12) / CELL_H;
        column = column < 0 ? 0 : column > (screen.width - 4) / CELL_W - 1
                                      ? (screen.width - 4) / CELL_W - 1 : column;
        row = row < 0 ? 0 : row >= rows_per_column() ? rows_per_column() - 1 : row;
        if (!cell_taken(column, row, i)) {
            icons[i].column = column;
            icons[i].row = row;
            moved_count++;
        }
    }
    printf("desktop: moved %d icon%s\n", moved_count, moved_count == 1 ? "" : "s");
    save_positions();
    add_damage(icons_area());
    return true;
}

void icons_pointer(void) {
    if (action == PRESSED && pressed_icon >= 0 &&
        (abs(pointer_x - press_x) > DRAG_START || abs(pointer_y - press_y) > DRAG_START)) {
        /* Only files move; an app's or the Trash's icon stays. */
        bool any = false;
        for (int i = 0; i < icon_count; i++) {
            any |= icons[i].selected && icons[i].kind == ICON_FILE;
        }
        action = any ? DRAGGING : NOTHING;
    }
    if (action == DRAGGING) {
        int target = icons_at(pointer_x, pointer_y);
        if (target >= 0 && (icons[target].selected ||
                            !(icons[target].kind == ICON_TRASH ||
                              (icons[target].kind == ICON_FILE && icons[target].is_dir)))) {
            target = -1;
        }
        if (window_at(pointer_x, pointer_y)) {
            target = -1;
        }
        drop_target = target;
        damage_all();
    } else if (action == SELECTING) {
        struct rect b = band();
        for (int i = 0; i < icon_count; i++) {
            struct rect r = icon_rect(i);
            bool in = r.x < b.x + b.width && b.x < r.x + r.width && r.y < b.y + b.height &&
                      b.y < r.y + r.height;
            icons[i].selected = in || (ctrl && icons[i].selected);
        }
        add_damage(icons_area());
    }
}
