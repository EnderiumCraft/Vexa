/* devmgr: Device Manager. Every device the kernel found, by type (or as it's
 * connected: what's on which bus, hub or controller), with what it is and
 * which driver has it; devices without a driver stand out. It follows
 * along as things are plugged in and out.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 780
#define HEIGHT 520
#define TOOLBAR 40
#define ROW 22
#define MAX_DEVICES 512
#define MAX_ROWS (MAX_DEVICES + VX_DEVICE_KIND_COUNT + 8)

static struct vx_window *window;
static struct vx_device_info devices[MAX_DEVICES];
static int count;
static unsigned long long generation;

static bool by_connection;           /* The tree, instead of groups by type. */
static bool collapsed[24];           /* Groups (by type) folded away. */
static unsigned selected_id;
static int top, hot_button = -1;

/* What the list shows: a group's heading, or a device (with its depth). */
struct row {
    int group;  /* >= 0: a heading. */
    int device; /* Index into devices[] (for device rows). */
    int depth;
};
static struct row rows[MAX_ROWS];
static int row_count;

/* The groups, in the order shown. */
struct group {
    const char *title;
    int kinds[3];
    int bus; /* -1: any; for "USB devices" (USB, of no other kind). */
};
static const struct group groups[] = {
    {"Processors", {VX_DEVICE_PROCESSOR, -1, -1}, -1},
    {"Display adapters", {VX_DEVICE_DISPLAY, -1, -1}, -1},
    {"Disk drives", {VX_DEVICE_DISK, -1, -1}, -1},
    {"Storage controllers", {VX_DEVICE_STORAGE, -1, -1}, -1},
    {"Keyboards", {VX_DEVICE_KEYBOARD, -1, -1}, -1},
    {"Mice and other pointers", {VX_DEVICE_POINTER, -1, -1}, -1},
    {"Other input devices", {VX_DEVICE_INPUT, -1, -1}, -1},
    {"Sound", {VX_DEVICE_SOUND, -1, -1}, -1},
    {"Network adapters", {VX_DEVICE_NETWORK, -1, -1}, -1},
    {"USB controllers", {VX_DEVICE_USB_CONTROLLER, VX_DEVICE_USB_HUB, -1}, -1},
    {"USB devices", {VX_DEVICE_OTHER, -1, -1}, VX_BUS_USB},
    {"Ports", {VX_DEVICE_SERIAL, -1, -1}, -1},
    {"System devices", {VX_DEVICE_SYSTEM, VX_DEVICE_BRIDGE, -1}, -1},
    {"Other devices", {VX_DEVICE_OTHER, -1, -1}, -2}, /* (Not USB.) */
};
#define GROUP_COUNT (int)(sizeof(groups) / sizeof(groups[0]))

static const char *kind_names[VX_DEVICE_KIND_COUNT] = {
    [VX_DEVICE_OTHER] = "Other device",       [VX_DEVICE_COMPUTER] = "Computer",
    [VX_DEVICE_PROCESSOR] = "Processor",      [VX_DEVICE_BRIDGE] = "Bridge",
    [VX_DEVICE_DISPLAY] = "Display adapter",  [VX_DEVICE_STORAGE] = "Storage controller",
    [VX_DEVICE_DISK] = "Disk drive",          [VX_DEVICE_NETWORK] = "Network adapter",
    [VX_DEVICE_SOUND] = "Sound",              [VX_DEVICE_KEYBOARD] = "Keyboard",
    [VX_DEVICE_POINTER] = "Pointer",          [VX_DEVICE_INPUT] = "Input device",
    [VX_DEVICE_USB_CONTROLLER] = "USB controller", [VX_DEVICE_USB_HUB] = "USB hub",
    [VX_DEVICE_SERIAL] = "Port",              [VX_DEVICE_SYSTEM] = "System device",
};
static const char *bus_names[] = {"-", "Built in", "PCI", "USB", "-"};

/* A color per kind, for the little badge in front of each device. */
static uint32_t kind_color(int kind) {
    switch (kind) {
    case VX_DEVICE_PROCESSOR: return 0x7f8fa6;
    case VX_DEVICE_DISPLAY: return 0x4a90e2;
    case VX_DEVICE_DISK: case VX_DEVICE_STORAGE: return 0xe0a030;
    case VX_DEVICE_KEYBOARD: case VX_DEVICE_POINTER: case VX_DEVICE_INPUT: return 0x9b6bdf;
    case VX_DEVICE_SOUND: return 0xe0608a;
    case VX_DEVICE_NETWORK: return 0x3fbf6f;
    case VX_DEVICE_USB_CONTROLLER: case VX_DEVICE_USB_HUB: return 0x2fb3c8;
    default: return 0x8a8a8a;
    }
}

static bool needs_driver(const struct vx_device_info *d) {
    return (d->bus == VX_BUS_PCI || d->bus == VX_BUS_USB) && !d->driver[0] &&
           d->kind != VX_DEVICE_BRIDGE;
}

static int index_of(unsigned id) {
    for (int i = 0; i < count; i++) {
        if (devices[i].id == id) {
            return i;
        }
    }
    return -1;
}

static bool in_group(const struct vx_device_info *d, const struct group *g) {
    if (g->bus == VX_BUS_USB && d->bus != VX_BUS_USB) {
        return false;
    }
    if (g->bus == -2 && d->bus == VX_BUS_USB) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        if (g->kinds[k] == d->kind) {
            return true;
        }
    }
    return false;
}

static void add_tree(unsigned parent, int depth) {
    for (int i = 0; i < count && row_count < MAX_ROWS; i++) {
        if (devices[i].parent == parent) {
            rows[row_count++] = (struct row){-1, i, depth};
            add_tree(devices[i].id, depth + 1);
        }
    }
}

static void build_rows(void) {
    row_count = 0;
    if (by_connection) {
        add_tree(0, 0);
        return;
    }
    for (int g = 0; g < GROUP_COUNT; g++) {
        int first = row_count;
        rows[row_count++] = (struct row){g, -1, 0};
        int members = 0;
        for (int i = 0; i < count && row_count < MAX_ROWS; i++) {
            if (devices[i].kind != VX_DEVICE_COMPUTER && in_group(&devices[i], &groups[g])) {
                members++;
                if (!collapsed[g]) {
                    rows[row_count++] = (struct row){-1, i, 1};
                }
            }
        }
        if (members == 0) {
            row_count = first; /* Empty groups aren't shown. */
        }
    }
}

static int group_size(int g) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        n += devices[i].kind != VX_DEVICE_COMPUTER && in_group(&devices[i], &groups[g]);
    }
    return n;
}

static void load(void) {
    unsigned long long now;
    long n = vx_device_list(devices, MAX_DEVICES, &now);
    if (n < 0) {
        n = 0;
    }
    bool changed = generation && now != generation;
    count = n < MAX_DEVICES ? (int)n : MAX_DEVICES;
    generation = now;
    if (selected_id && index_of(selected_id) < 0) {
        selected_id = 0; /* (It was unplugged.) */
    }
    build_rows();
    if (changed) {
        printf("devmgr: devices changed (%d)\n", count);
    } else {
        printf("devmgr: %d devices\n", count);
    }
    fflush(stdout);
}

/* ---- Drawing ---- */

static int list_width(void) {
    return window->surface.width * 52 / 100;
}

static int visible_rows(void) {
    return (window->surface.height - TOOLBAR) / ROW;
}

static void draw_list(struct vx_surface *s) {
    int lw = list_width(), h = s->height;
    vx_fill(s, 0, TOOLBAR, lw, h - TOOLBAR, VX_COLOR_VIEW);
    int visible = visible_rows();
    top = top > row_count - visible ? row_count - visible : top;
    top = top < 0 ? 0 : top;
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, VX_UI_FONT_SIZE);
    for (int r = 0; r < visible && top + r < row_count; r++) {
        const struct row *row = &rows[top + r];
        int y = TOOLBAR + r * ROW;
        if (row->group >= 0) {
            char title[64];
            snprintf(title, sizeof title, "%s %s  (%d)", collapsed[row->group] ? "\xe2\x96\xb8" : "\xe2\x96\xbe",
                     groups[row->group].title, group_size(row->group));
            vx_text(s, bold, 10, y + 3, title, VX_COLOR_TEXT, VX_TRANSPARENT);
            continue;
        }
        const struct vx_device_info *d = &devices[row->device];
        if (d->id == selected_id) {
            vx_fill(s, 0, y, lw, ROW, VX_COLOR_SELECTED);
        }
        int x = 12 + row->depth * 18;
        vx_fill(s, x, y + 6, 10, 10, kind_color(d->kind));
        x += 16;
        uint32_t color = needs_driver(d) ? 0xe08a2a : VX_COLOR_TEXT;
        char label[96];
        snprintf(label, sizeof label, "%s%s", d->name, needs_driver(d) ? "  (no driver)" : "");
        vx_draw_text_fit(s, x, y + 3, lw - x - 8, label, color, VX_TRANSPARENT);
    }
    vx_fill(s, lw, TOOLBAR, 1, h - TOOLBAR, VX_COLOR_LINE);
}

static int detail_line(struct vx_surface *s, int x, int y, int w, const char *label,
                       const char *value) {
    if (!value || !value[0]) {
        return y;
    }
    vx_draw_text(s, x, y, label, VX_COLOR_DIM, VX_TRANSPARENT);
    /* Long values wrap at commas onto the lines below. */
    const char *p = value;
    int vx = x + 110, vw = w - 110;
    while (*p) {
        size_t fit = vx_text_fit_bytes(vx_font_ui(), p, vw);
        size_t len = strlen(p);
        if (fit < len) {
            size_t cut = fit;
            while (cut > 0 && p[cut] != ' ') {
                cut--;
            }
            fit = cut ? cut : fit;
        }
        char piece[160];
        size_t n = fit < sizeof piece - 1 ? fit : sizeof piece - 1;
        memcpy(piece, p, n);
        piece[n] = '\0';
        vx_draw_text(s, vx, y, piece, VX_COLOR_TEXT, VX_TRANSPARENT);
        y += VX_LINE_HEIGHT + 2;
        p += n;
        while (*p == ' ') {
            p++;
        }
        if (n == 0) {
            break;
        }
    }
    return y + 6;
}

static void draw_details(struct vx_surface *s) {
    int x = list_width() + 20, w = s->width - x - 20, y = TOOLBAR + 18;
    int at = index_of(selected_id);
    if (at < 0) {
        vx_draw_text(s, x, y, "Choose a device to see what it is.", VX_COLOR_DIM, VX_TRANSPARENT);
        return;
    }
    const struct vx_device_info *d = &devices[at];
    vx_fill(s, x, y + 4, 22, 22, kind_color(d->kind));
    const struct vx_font *big = vx_font(VX_FACE_BOLD, 17);
    int tx = x + 32;
    size_t fit = vx_text_fit_bytes(big, d->name, w - 32);
    char name[64];
    snprintf(name, sizeof name, "%.*s", (int)fit, d->name);
    vx_text(s, big, tx, y + 2, name, VX_COLOR_TEXT, VX_TRANSPARENT);
    y += vx_font_height(big) + 4;
    const char *kind = d->kind < VX_DEVICE_KIND_COUNT ? kind_names[d->kind] : "";
    if (d->bus == VX_BUS_USB && d->kind == VX_DEVICE_OTHER) {
        kind = "USB device";
    }
    vx_draw_text(s, tx, y, kind, VX_COLOR_DIM, VX_TRANSPARENT);
    y += VX_LINE_HEIGHT + 18;

    char text[160];
    if (d->driver[0]) {
        y = detail_line(s, x, y, w, "Driver", d->driver);
    } else {
        y = detail_line(s, x, y, w, "Driver",
                        needs_driver(d) ? "None: Vexa has no driver for it yet" : "-");
    }
    y = detail_line(s, x, y, w, "Status",
                    needs_driver(d) ? "Not in use" : "Working");
    int parent = index_of(d->parent);
    y = detail_line(s, x, y, w, "Connected to", parent >= 0 ? devices[parent].name : "");
    y = detail_line(s, x, y, w, "Bus", d->bus < 5 ? bus_names[d->bus] : "-");
    y = detail_line(s, x, y, w, "Location", d->location);
    if (d->vendor_id || d->product_id) {
        snprintf(text, sizeof text, "%04x:%04x", d->vendor_id, d->product_id);
        y = detail_line(s, x, y, w, "Vendor:device", text);
    }
    y = detail_line(s, x, y, w, "Details", d->details);
    if (d->flags & VX_DEVICE_REMOVABLE) {
        y = detail_line(s, x, y, w, "Removable", "Yes: it can be unplugged");
    }
    int children = 0;
    for (int i = 0; i < count; i++) {
        children += devices[i].parent == d->id;
    }
    if (children) {
        snprintf(text, sizeof text, "%d", children);
        detail_line(s, x, y, w, "Devices on it", text);
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_WINDOW);
    vx_draw_button(s, 10, 8, 90, 24, by_connection ? "By type" : "By type \xe2\x9c\x93",
                   hot_button == 0);
    vx_draw_button(s, 106, 8, 130, 24, by_connection ? "By connection \xe2\x9c\x93" : "By connection",
                   hot_button == 1);
    int missing = 0;
    for (int i = 0; i < count; i++) {
        missing += needs_driver(&devices[i]);
    }
    char line[96];
    if (missing) {
        snprintf(line, sizeof line, "%d devices, %d without a driver", count, missing);
    } else {
        snprintf(line, sizeof line, "%d devices", count);
    }
    vx_draw_text(s, w - 12 - vx_text_width(line), 12, line, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_fill(s, 0, TOOLBAR - 1, w, 1, VX_COLOR_LINE);
    draw_list(s);
    draw_details(s);
    vx_window_present(window, 0, 0, w, h);
}

/* ---- Input ---- */

static void set_view(bool connection) {
    by_connection = connection;
    top = 0;
    build_rows();
}

static void select_row(int r) {
    if (r < 0 || r >= row_count) {
        return;
    }
    if (rows[r].group >= 0) {
        collapsed[rows[r].group] = !collapsed[rows[r].group];
        build_rows();
        return;
    }
    selected_id = devices[rows[r].device].id;
    printf("devmgr: showing \"%s\"\n", devices[rows[r].device].name);
    fflush(stdout);
}

static void move_selection(int step) {
    int at = -1;
    for (int r = 0; r < row_count; r++) {
        if (rows[r].group < 0 && devices[rows[r].device].id == selected_id) {
            at = r;
        }
    }
    for (int r = at + step; r >= 0 && r < row_count; r += step) {
        if (rows[r].group < 0) {
            select_row(r);
            if (r < top) {
                top = r;
            } else if (r >= top + visible_rows()) {
                top = r - visible_rows() + 1;
            }
            return;
        }
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    hot_button = vx_inside(e->x, e->y, 10, 8, 90, 24) ? 0 : vx_inside(e->x, e->y, 106, 8, 130, 24) ? 1 : -1;
    if (e->wheel) {
        top -= e->wheel * 3;
    }
    if (!click) {
        return;
    }
    if (hot_button >= 0) {
        set_view(hot_button == 1);
        return;
    }
    if (e->y >= TOOLBAR && e->x < list_width()) {
        select_row(top + (e->y - TOOLBAR) / ROW);
    }
}

int main(void) {
    window = vx_window_create_flags("Device Manager", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "devmgr: no desktop to open a window on\n");
        return 1;
    }
    load();
    int held = 0;
    long next = vx_uptime() + 1000;
    for (;;) {
        draw();
        struct vx_gui_event e;
        long left = next - vx_uptime();
        int got = vx_gui_wait(&e, left < 0 ? 0 : left);
        if (got < 0) {
            return 0;
        }
        if (got == 0) { /* Each second: has anything been plugged in or out? */
            unsigned long long now = 0;
            vx_device_list(NULL, 0, &now);
            if (now != generation) {
                load();
            }
            next = vx_uptime() + 1000;
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_POINTER:
            pointer(&e, &held);
            break;
        case VX_GUI_KEY:
            if (!e.value) {
                break;
            }
            if (e.key == VX_KEY_UP || e.key == VX_KEY_DOWN) {
                move_selection(e.key == VX_KEY_DOWN ? 1 : -1);
            } else if (e.key == VX_KEY_TAB) {
                set_view(!by_connection);
            }
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 560 && e.height >= 320) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
