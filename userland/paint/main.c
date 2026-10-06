/* paint: Paint. Pencil, brush, line, rectangle, ellipse, fill and eraser, a
 * colour picker, four sizes, sixteen colours; pictures open and save as PNG
 * (BMP and PPM open too). `paint [file]`.
 *
 *     Ctrl+Z, Ctrl+Y   undo, redo
 *     Ctrl+N, O, S     new, open, save (Ctrl+Shift+S: save as)
 *     P B L R E F X I  the tools; [ and ] the size
 *     a right click    draws in the second colour (white, to begin with)
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

#define WIDTH 860
#define HEIGHT 600
#define TOOLS 64   /* The tool column on the left. */
#define BAR 44     /* Colours and sizes along the top. */
#define CANVAS_W 720
#define CANVAS_H 480
#define MAX_UNDO 16

enum tool { PENCIL, BRUSH, LINE, RECTANGLE, ELLIPSE, FILL, ERASER, PICKER, TOOL_COUNT };
static const char *const tool_names[TOOL_COUNT] = {"Pencil", "Brush", "Line", "Rect", "Ellipse",
                                                   "Fill", "Eraser", "Pick"};
static const char tool_keys[TOOL_COUNT] = {'p', 'b', 'l', 'r', 'e', 'f', 'x', 'i'};

static const uint32_t palette[16] = {
    0x000000, 0x5a5a5a, 0xa0a0a0, 0xffffff, 0xe53935, 0xfb8c00, 0xfdd835, 0x43a047,
    0x00acc1, 0x1e88e5, 0x3949ab, 0x8e24aa, 0xd81b60, 0x6d4c41, 0xf8bbd0, 0xb2ebf2,
};
static const int sizes[4] = {1, 3, 7, 15};

static struct vx_window *window;
static struct vx_surface canvas;
static enum tool tool = BRUSH;
static uint32_t colors[2] = {0x000000, 0xffffff};
static int size_index = 1;
static char path[512];
static bool modified, ctrl, shift;
static int view_x, view_y; /* Where the canvas is drawn in the window (its corner). */

/* Undo: whole pictures (they're small). */
static uint32_t *undo[MAX_UNDO], *redo[MAX_UNDO];
static int undo_count, redo_count;

/* A drag: where it started, the last point, and which colour. */
static bool drawing;
static int start_x, start_y, last_x, last_y, drawing_color;
static uint32_t *before_drag; /* For lines and shapes: the picture before. */

static size_t canvas_bytes(void) {
    return (size_t)canvas.width * canvas.height * 4;
}

static void push(uint32_t **stack, int *count) {
    if (*count == MAX_UNDO) {
        free(stack[0]);
        memmove(stack, stack + 1, sizeof(stack[0]) * (MAX_UNDO - 1));
        (*count)--;
    }
    uint32_t *copy = malloc(canvas_bytes());
    if (copy) {
        memcpy(copy, canvas.pixels, canvas_bytes());
        stack[(*count)++] = copy;
    }
}

static void clear_stack(uint32_t **stack, int *count) {
    while (*count) {
        free(stack[--*count]);
    }
}

static void remember(void) {
    push(undo, &undo_count);
    clear_stack(redo, &redo_count);
    modified = true;
}

static void step_back(uint32_t **from, int *from_count, uint32_t **to, int *to_count) {
    if (!*from_count) {
        return;
    }
    push(to, to_count);
    uint32_t *picture = from[--*from_count];
    memcpy(canvas.pixels, picture, canvas_bytes());
    free(picture);
    modified = true;
}

static void new_canvas(int w, int h) {
    clear_stack(undo, &undo_count);
    clear_stack(redo, &redo_count);
    free(canvas.pixels);
    free(before_drag);
    canvas.pixels = malloc((size_t)w * h * 4);
    before_drag = malloc((size_t)w * h * 4);
    canvas.width = canvas.stride = w;
    canvas.height = h;
    for (int i = 0; i < w * h; i++) {
        canvas.pixels[i] = 0xffffff;
    }
    modified = false;
}

/* ---- Drawing on the canvas ---- */

static void plot(int x, int y, int size, uint32_t color) {
    int r = size / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (size > 2 && dx * dx + dy * dy > r * r + r) {
                continue; /* Round brushes. */
            }
            int px = x + dx, py = y + dy;
            if (px >= 0 && py >= 0 && px < canvas.width && py < canvas.height) {
                canvas.pixels[py * canvas.stride + px] = color;
            }
        }
    }
}

static void line(int x0, int y0, int x1, int y1, int size, uint32_t color) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        plot(x0, y0, size, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy, x0 += sx;
        }
        if (e2 <= dx) {
            err += dx, y0 += sy;
        }
    }
}

static void rectangle(int x0, int y0, int x1, int y1, int size, uint32_t color) {
    line(x0, y0, x1, y0, size, color);
    line(x1, y0, x1, y1, size, color);
    line(x1, y1, x0, y1, size, color);
    line(x0, y1, x0, y0, size, color);
}

static long isqrt(long v) {
    if (v < 2) {
        return v;
    }
    long r = v, next = (r + v / r) / 2; /* Newton's way. */
    while (next < r) {
        r = next;
        next = (r + v / r) / 2;
    }
    return r;
}

static void ellipse(int x0, int y0, int x1, int y1, int size, uint32_t color) {
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = abs(x1 - x0) / 2, ry = abs(y1 - y0) / 2;
    if (!rx || !ry) {
        line(x0, y0, x1, y1, size, color);
        return;
    }
    /* Across the width, each column's top and bottom; joined to the last. */
    int last_y = 0;
    for (int dx = -rx; dx <= rx; dx++) {
        int dy = (int)(ry * isqrt((long)(rx * rx - dx * dx) * 10000) / (rx * 100));
        if (dx > -rx) {
            line(cx + dx - 1, cy - last_y, cx + dx, cy - dy, size, color);
            line(cx + dx - 1, cy + last_y, cx + dx, cy + dy, size, color);
        }
        last_y = dy;
    }
}

static void flood_fill(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) {
        return;
    }
    uint32_t target = canvas.pixels[y * canvas.stride + x];
    if (target == color) {
        return;
    }
    /* Spans, with a stack of points. */
    int capacity = 4096, count = 0;
    int *stack = malloc((size_t)capacity * 2 * sizeof(int));
    if (!stack) {
        return;
    }
    stack[count * 2] = x, stack[count * 2 + 1] = y, count++;
    while (count) {
        count--;
        int px = stack[count * 2], py = stack[count * 2 + 1];
        uint32_t *row = canvas.pixels + py * canvas.stride;
        if (row[px] != target) {
            continue;
        }
        int left = px, right = px;
        while (left > 0 && row[left - 1] == target) {
            left--;
        }
        while (right + 1 < canvas.width && row[right + 1] == target) {
            right++;
        }
        for (int i = left; i <= right; i++) {
            row[i] = color;
        }
        for (int d = -1; d <= 1; d += 2) {
            int ny = py + d;
            if (ny < 0 || ny >= canvas.height) {
                continue;
            }
            uint32_t *next = canvas.pixels + ny * canvas.stride;
            for (int i = left; i <= right; i++) {
                if (next[i] == target && (i == left || next[i - 1] != target)) {
                    if (count == capacity) {
                        int *more = realloc(stack, (size_t)capacity * 4 * sizeof(int));
                        if (!more) {
                            free(stack);
                            return;
                        }
                        stack = more;
                        capacity *= 2;
                    }
                    stack[count * 2] = i, stack[count * 2 + 1] = ny, count++;
                }
            }
        }
    }
    free(stack);
}

/* ---- Files ---- */

static void set_title(void) {
    char title[600];
    const char *name = path[0] ? strrchr(path, '/') ? strrchr(path, '/') + 1 : path : "Untitled";
    snprintf(title, sizeof(title), "%s%s - Paint", modified ? "*" : "", name);
    static char shown[600];
    if (strcmp(shown, title)) {
        snprintf(shown, sizeof(shown), "%s", title);
        vx_window_set_title(window, title);
    }
}

static bool open_file(const char *file) {
    struct vx_image *image = vx_image_load(file, 0xffffff);
    if (!image) {
        return false;
    }
    int w = image->surface.width > 4000 ? 4000 : image->surface.width;
    int h = image->surface.height > 4000 ? 4000 : image->surface.height;
    new_canvas(w, h);
    vx_blit(&canvas, 0, 0, &image->surface, 0, 0, w, h);
    vx_image_free(image);
    snprintf(path, sizeof(path), "%s", file);
    view_x = view_y = 0;
    printf("paint: opened %s (%dx%d)\n", file, w, h);
    fflush(stdout);
    return true;
}

static void save_as(void);

static void save(void) {
    if (!path[0]) {
        save_as();
        return;
    }
    int error = vx_image_save_png(path, &canvas);
    if (!error) {
        modified = false;
        printf("paint: saved %s\n", path);
        fflush(stdout);
    }
}

static void save_as(void) {
    char file[512];
    if (vx_save_dialog("Save Picture", vx_home_folder("Pictures"), path[0] ? strrchr(path, '/') + 1 : "Untitled.png",
                       file, sizeof(file))) {
        size_t n = strlen(file);
        if (n < 4 || strcmp(file + n - 4, ".png")) {
            snprintf(file + n, sizeof(file) - n, ".png");
        }
        snprintf(path, sizeof(path), "%s", file);
        save();
    }
}

/* Before the picture is replaced or closed: if it has changes that aren't
 * saved, asks whether to save them (Save, Don't Save, Cancel). False if it's
 * to stay as it is: Cancel, or a Save that didn't happen. */
static bool may_discard(const char *verb) {
    if (!modified) {
        return true;
    }
    const char *name = path[0] ? strrchr(path, '/') ? strrchr(path, '/') + 1 : path : "Untitled";
    switch (vx_ask_save_changes(name, verb)) {
    case VX_SAVE_YES:
        save();
        return !modified;
    case VX_SAVE_NO: return true;
    default: return false;
    }
}

static void new_picture(void) {
    if (may_discard("start a new one")) {
        new_canvas(CANVAS_W, CANVAS_H);
        path[0] = '\0';
    }
}

static void open_dialog(void) {
    char file[512];
    if (vx_open_dialog("Open Picture", vx_home_folder("Pictures"), file, sizeof(file)) &&
        may_discard("open another")) {
        open_file(file);
    }
}

/* ---- The window ---- */

static int canvas_left(void) {
    return TOOLS + 12;
}

static int canvas_top(void) {
    return BAR + 12;
}

static void tool_rect(int i, int *x, int *y) {
    *x = 6;
    *y = BAR + 8 + i * 44;
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, vx_theme.dark ? 0x2a2a30 : 0xc8c8d0);
    /* The canvas, with a shadow, scrolled if it's bigger than the window. */
    int cl = canvas_left(), ct = canvas_top();
    int room_w = w - cl - 12, room_h = h - ct - 12;
    int show_w = canvas.width < room_w ? canvas.width : room_w;
    int show_h = canvas.height < room_h ? canvas.height : room_h;
    view_x = view_x > canvas.width - show_w ? canvas.width - show_w : view_x < 0 ? 0 : view_x;
    view_y = view_y > canvas.height - show_h ? canvas.height - show_h : view_y < 0 ? 0 : view_y;
    vx_fill(s, cl + 4, ct + 4, show_w, show_h, 0x101014);
    vx_blit(s, cl, ct, &canvas, view_x, view_y, show_w, show_h);
    /* The tools. */
    vx_fill(s, 0, BAR, TOOLS, h - BAR, vx_theme.sidebar);
    vx_fill(s, TOOLS - 1, BAR, 1, h - BAR, VX_COLOR_LINE);
    for (int i = 0; i < TOOL_COUNT; i++) {
        int x, y;
        tool_rect(i, &x, &y);
        vx_draw_button(s, x, y, TOOLS - 12, 38, tool_names[i], i == (int)tool);
    }
    /* Colours and sizes. */
    vx_draw_toolbar(s, 0, 0, w, BAR);
    /* The two colours (back one behind), then the palette: rounded wells. */
    vx_fill_rounded(s, 21, 17, 22, 22, 5, VX_COLOR_LINE, 255);
    vx_fill_rounded(s, 22, 18, 20, 20, 4, colors[1], 255);
    vx_fill_rounded(s, 9, 5, 22, 22, 5, VX_COLOR_TEXT, 255);
    vx_fill_rounded(s, 10, 6, 20, 20, 4, colors[0], 255);
    for (int i = 0; i < 16; i++) {
        int x = 56 + (i % 8) * 20, y = 4 + (i / 8) * 18;
        bool chosen = palette[i] == colors[0];
        vx_fill_rounded(s, x, y, 18, 16, 4, chosen ? VX_COLOR_ACCENT : VX_COLOR_LINE, 255);
        vx_fill_rounded(s, x + (chosen ? 2 : 1), y + (chosen ? 2 : 1), chosen ? 14 : 16,
                        chosen ? 12 : 14, 3, palette[i], 255);
        vx_fill(s, x + 3, y + 1 + (chosen ? 1 : 0), 12 - (chosen ? 2 : 0), 1,
                vx_mix(palette[i], 0xffffff, 110)); /* (A little shine.) */
    }
    for (int i = 0; i < 4; i++) {
        int x = 230 + i * 34;
        vx_draw_button(s, x, 8, 30, 28, "", i == size_index);
        int d = sizes[i] > 14 ? 14 : sizes[i] < 2 ? 2 : sizes[i];
        vx_fill(s, x + 15 - d / 2, 22 - d / 2, d, d, VX_COLOR_TEXT);
    }
    vx_draw_button(s, 380, 8, 50, 28, "New", false);
    vx_draw_button(s, 434, 8, 56, 28, "Open", false);
    vx_draw_button(s, 494, 8, 50, 28, "Save", false);
    vx_draw_button(s, 554, 8, 50, 28, "Undo", false);
    vx_draw_button(s, 608, 8, 50, 28, "Redo", false);
    char info[48];
    snprintf(info, sizeof(info), "%dx%d", canvas.width, canvas.height);
    vx_draw_text(s, w - 12 - vx_text_width(info), 14, info, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_window_present(window, 0, 0, w, h);
    set_title();
}

static bool to_canvas(int px, int py, int *x, int *y) {
    *x = px - canvas_left() + view_x;
    *y = py - canvas_top() + view_y;
    return px >= canvas_left() && py >= canvas_top() && *x < canvas.width + 40 && *y < canvas.height + 40;
}

static void act_button(int px, int py) {
    for (int i = 0; i < TOOL_COUNT; i++) {
        int x, y;
        tool_rect(i, &x, &y);
        if (vx_inside(px, py, x, y, TOOLS - 12, 38)) {
            tool = (enum tool)i;
            return;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (vx_inside(px, py, 56 + (i % 8) * 20, 4 + (i / 8) * 18, 18, 16)) {
            colors[0] = palette[i];
            return;
        }
    }
    if (vx_inside(px, py, 10, 6, 32, 32)) { /* Swap the two colours. */
        uint32_t t = colors[0];
        colors[0] = colors[1], colors[1] = t;
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (vx_inside(px, py, 230 + i * 34, 8, 30, 28)) {
            size_index = i;
            return;
        }
    }
    if (vx_inside(px, py, 380, 8, 50, 28)) {
        new_picture();
    } else if (vx_inside(px, py, 434, 8, 56, 28)) {
        open_dialog();
    } else if (vx_inside(px, py, 494, 8, 50, 28)) {
        save();
    } else if (vx_inside(px, py, 554, 8, 50, 28)) {
        step_back(undo, &undo_count, redo, &redo_count);
    } else if (vx_inside(px, py, 608, 8, 50, 28)) {
        step_back(redo, &redo_count, undo, &undo_count);
    }
}

static void shape_to(int x, int y) {
    memcpy(canvas.pixels, before_drag, canvas_bytes());
    uint32_t color = colors[drawing_color];
    int size = sizes[size_index];
    if (tool == LINE) {
        line(start_x, start_y, x, y, size, color);
    } else if (tool == RECTANGLE) {
        int x1 = x, y1 = y;
        if (shift) { /* A square. */
            int d = abs(x - start_x) > abs(y - start_y) ? abs(x - start_x) : abs(y - start_y);
            x1 = start_x + (x >= start_x ? d : -d), y1 = start_y + (y >= start_y ? d : -d);
        }
        rectangle(start_x, start_y, x1, y1, size, color);
    } else if (tool == ELLIPSE) {
        ellipse(start_x, start_y, x, y, size, color);
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    int pressed = e->buttons & ~*held & 3;
    *held = e->buttons;
    int x, y;
    bool on_canvas = to_canvas(e->x, e->y, &x, &y);
    vx_window_set_cursor(window, on_canvas && x < canvas.width && y < canvas.height
                                     ? VX_CURSOR_CROSS : VX_CURSOR_ARROW);
    if (e->wheel) {
        view_y -= e->wheel * 40;
    }
    if (pressed && !on_canvas) {
        act_button(e->x, e->y);
        return;
    }
    if (pressed && on_canvas) {
        drawing_color = (pressed & 2) ? 1 : 0;
        if (tool == PICKER) {
            if (x < canvas.width && y < canvas.height) {
                colors[drawing_color] = canvas.pixels[y * canvas.stride + x];
            }
            return;
        }
        remember();
        if (tool == FILL) {
            flood_fill(x, y, colors[drawing_color]);
            return;
        }
        drawing = true;
        start_x = last_x = x, start_y = last_y = y;
        memcpy(before_drag, canvas.pixels, canvas_bytes());
        if (tool == PENCIL || tool == BRUSH || tool == ERASER) {
            uint32_t color = tool == ERASER ? 0xffffff : colors[drawing_color];
            plot(x, y, tool == PENCIL ? 1 : sizes[size_index] * (tool == ERASER ? 2 : 1), color);
        }
        return;
    }
    if (drawing && (e->buttons & 3)) {
        if (tool == PENCIL || tool == BRUSH || tool == ERASER) {
            uint32_t color = tool == ERASER ? 0xffffff : colors[drawing_color];
            line(last_x, last_y, x, y, tool == PENCIL ? 1 : sizes[size_index] * (tool == ERASER ? 2 : 1),
                 color);
        } else {
            shape_to(x, y);
        }
        last_x = x, last_y = y;
    }
    if (drawing && !(e->buttons & 3)) {
        drawing = false;
    }
}

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
        return;
    }
    if (e->key == VX_KEY_LEFTSHIFT || e->key == VX_KEY_RIGHTSHIFT) {
        shift = e->value != 0;
        return;
    }
    if (!e->value) {
        return;
    }
    if (ctrl) {
        switch (e->key) {
        case 44: shift ? step_back(redo, &redo_count, undo, &undo_count)
                       : step_back(undo, &undo_count, redo, &redo_count); return; /* Z */
        case 21: step_back(redo, &redo_count, undo, &undo_count); return;         /* Y */
        case 49: new_picture(); return;                                           /* N */
        case 24: open_dialog(); return;                                          /* O */
        case 31: shift ? save_as() : save(); return;                              /* S */
        }
        return;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (e->character == tool_keys[i]) {
            tool = (enum tool)i;
        }
    }
    if (e->character == '[' && size_index > 0) {
        size_index--;
    } else if (e->character == ']' && size_index < 3) {
        size_index++;
    }
    switch (e->key) {
    case VX_KEY_UP: view_y -= 40; break;
    case VX_KEY_DOWN: view_y += 40; break;
    case VX_KEY_LEFT: view_x -= 40; break;
    case VX_KEY_RIGHT: view_x += 40; break;
    }
}

int main(int argc, char **argv) {
    new_canvas(CANVAS_W, CANVAS_H);
    if (argc > 1 && !open_file(argv[1])) {
        snprintf(path, sizeof(path), "%s", argv[1]); /* A new picture, to be saved there. */
    }
    window = vx_window_create_flags("Paint", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "paint: no desktop to open a window on\n");
        return 1;
    }
    int held = 0;
    for (;;) {
        draw();
        vx_window_set_modified(window, modified); /* (The desktop warns before logging out.) */
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            return 0;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            if (may_discard("close")) {
                vx_window_destroy(window);
                return 0;
            }
            break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = shift = false;
            }
            break;
        case VX_GUI_DROP: {
            char *paths = vx_drop_paths(&e);
            vx_remove(e.text);
            if (paths) {
                char *end = strchr(paths, '\n');
                if (end) {
                    *end = '\0';
                }
                if (may_discard("open another")) {
                    open_file(paths);
                }
                free(paths);
            }
            break;
        }
        case VX_GUI_RESIZE:
            if (e.width >= 500 && e.height >= 360) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
