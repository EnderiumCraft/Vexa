/* view: the Image Viewer. `view [file]` shows a PNG, BMP or PPM picture,
 * fitted to the window.
 *
 *     Left/Right, Page Up/Down      the previous, next picture in its folder
 *     + and -, the wheel, 0         zoom in, out, fit again (1: actual size)
 *     dragging, the arrows (zoomed) move it around
 *     R, Shift+R                    turn it right, left
 *     Space or S                    a slideshow (any key stops it)
 *     Ctrl+O                        open another
 *
 * The toolbar has the same.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/files.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/users.h>

#define TOOLBAR 36
#define STATUS 22
#define SLIDE_MS 3000
#define MAX_PICTURES 512
#define BACKGROUND (vx_theme.dark ? 0x111113u : 0xe8e9edu)

static struct vx_window *window;
static struct vx_image *image;
static char path[512];
static int zoom_percent;        /* 0: fit to the window. */
static int pan_x, pan_y;        /* The picture's offset from the middle. */
static bool slideshow;
static long next_slide;
static int hot = -1;
static bool ctrl, shift;

/* The pictures in the folder, by name. */
static char (*pictures)[256];
static int picture_count, picture_index = -1;

enum { B_PREVIOUS, B_NEXT, B_ZOOM_OUT, B_FIT, B_ZOOM_IN, B_LEFT, B_RIGHT, B_SLIDES, B_OPEN, BUTTONS };
static const char *const labels[BUTTONS] = {"<", ">", "-", "Fit", "+", "Turn L", "Turn R",
                                            "Slideshow", "Open"};
static const int widths[BUTTONS] = {28, 28, 28, 40, 28, 56, 56, 80, 54};

static bool is_picture(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    char ext[8] = "";
    for (int i = 0; i < 7 && dot[i + 1]; i++) {
        ext[i] = (char)(dot[i + 1] | 0x20);
    }
    return !strcmp(ext, "png") || !strcmp(ext, "bmp") || !strcmp(ext, "ppm");
}

static int compare(const void *a, const void *b) {
    return strcmp(a, b);
}

static void folder_of(char *out, size_t size) {
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) {
        slash == out ? (void)(out[1] = '\0') : (void)(*slash = '\0');
    } else {
        snprintf(out, size, ".");
    }
}

static void read_folder(void) {
    picture_count = 0;
    picture_index = -1;
    if (!pictures) {
        pictures = malloc(MAX_PICTURES * sizeof(*pictures));
    }
    if (!pictures || !path[0]) {
        return;
    }
    char folder[512];
    folder_of(folder, sizeof(folder));
    int handle = vx_open(folder, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    struct vx_dir_entry entries[32];
    long n;
    while ((n = vx_read_dir(handle, entries, 32)) > 0) {
        for (long i = 0; i < n && picture_count < MAX_PICTURES; i++) {
            if (entries[i].name[0] != '.' && is_picture(entries[i].name)) {
                snprintf(pictures[picture_count++], 256, "%s", entries[i].name);
            }
        }
    }
    vx_close(handle);
    qsort(pictures, (size_t)picture_count, sizeof(pictures[0]), compare);
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    for (int i = 0; i < picture_count; i++) {
        if (!strcmp(pictures[i], name)) {
            picture_index = i;
        }
    }
}

static void set_title(void) {
    char title[300];
    const char *name = path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    snprintf(title, sizeof(title), "%s%sImage Viewer", name ? name : "", name ? " - " : "");
    vx_window_set_title(window, title);
}

static void load(const char *file, bool same_folder) {
    snprintf(path, sizeof(path), "%s", file);
    vx_image_free(image);
    image = vx_image_load(path, BACKGROUND);
    zoom_percent = pan_x = pan_y = 0;
    if (!same_folder) {
        read_folder();
    }
    if (window) {
        set_title();
    }
    printf("view: showing %s%s\n", path, image ? "" : " (can't read it)");
    fflush(stdout);
}

static void step(int by) {
    if (picture_count < 1) {
        return;
    }
    picture_index = ((picture_index < 0 ? 0 : picture_index) + by + picture_count) % picture_count;
    char folder[512], file[800];
    folder_of(folder, sizeof(folder));
    vx_join_path(file, sizeof(file), folder, pictures[picture_index]);
    load(file, true);
}

/* Turns the picture a quarter: right (clockwise) or left. */
static void rotate(bool right) {
    if (!image) {
        return;
    }
    struct vx_surface *s = &image->surface;
    int w = s->width, h = s->height;
    uint32_t *turned = malloc((size_t)w * h * 4);
    if (!turned) {
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int nx = right ? h - 1 - y : y, ny = right ? x : w - 1 - x;
            turned[ny * h + nx] = s->pixels[y * s->stride + x];
        }
    }
    memcpy(s->pixels, turned, (size_t)w * h * 4);
    free(turned);
    s->width = s->stride = h;
    s->height = w;
    pan_x = pan_y = 0;
}

/* The zoom now, in percent (fit: the largest that fits, at most 100). */
static int percent_now(void) {
    if (!image) {
        return 100;
    }
    if (zoom_percent) {
        return zoom_percent;
    }
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height - TOOLBAR - STATUS;
    int px = w * 100 / image->surface.width, py = h * 100 / image->surface.height;
    int p = px < py ? px : py;
    return p > 100 ? 100 : p < 1 ? 1 : p;
}

static void zoom(int percent) {
    zoom_percent = percent < 5 ? 5 : percent > 1600 ? 1600 : percent;
}

static int button_x(int b) {
    int x = 8;
    for (int i = 0; i < b; i++) {
        x += widths[i] + (i == B_NEXT || i == B_ZOOM_IN || i == B_RIGHT ? 14 : 4);
    }
    return x;
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height - TOOLBAR - STATUS;
    vx_fill(s, 0, 0, s->width, s->height, BACKGROUND);
    char status[400];
    if (image) {
        int iw = image->surface.width, ih = image->surface.height;
        int percent = percent_now();
        int dw = iw * percent / 100, dh = ih * percent / 100;
        dw = dw < 1 ? 1 : dw;
        dh = dh < 1 ? 1 : dh;
        /* Panning stays within the picture. */
        int max_x = dw > w ? (dw - w) / 2 : 0, max_y = dh > h ? (dh - h) / 2 : 0;
        pan_x = pan_x > max_x ? max_x : pan_x < -max_x ? -max_x : pan_x;
        pan_y = pan_y > max_y ? max_y : pan_y < -max_y ? -max_y : pan_y;
        struct vx_surface view = {s->pixels + (long)TOOLBAR * s->stride, w, h, s->stride};
        int x = (w - dw) / 2 - pan_x, y = (h - dh) / 2 - pan_y;
        if (percent < 100) {
            /* Smaller: averaged, so it isn't speckled. */
            struct vx_surface *from = &image->surface;
            for (int ty = y < 0 ? 0 : y; ty < y + dh && ty < h; ty++) {
                int sy0 = (ty - y) * ih / dh, sy1 = (ty - y + 1) * ih / dh;
                sy1 = sy1 <= sy0 ? sy0 + 1 : sy1;
                for (int tx = x < 0 ? 0 : x; tx < x + dw && tx < w; tx++) {
                    int sx0 = (tx - x) * iw / dw, sx1 = (tx - x + 1) * iw / dw;
                    sx1 = sx1 <= sx0 ? sx0 + 1 : sx1;
                    unsigned r = 0, g = 0, b = 0, n = 0;
                    int stride_y = (sy1 - sy0) > 3 ? (sy1 - sy0) / 3 : 1;
                    int stride_x = (sx1 - sx0) > 3 ? (sx1 - sx0) / 3 : 1;
                    for (int sy = sy0; sy < sy1; sy += stride_y) {
                        for (int sx = sx0; sx < sx1; sx += stride_x) {
                            uint32_t p = from->pixels[sy * from->stride + sx];
                            r += p >> 16 & 255, g += p >> 8 & 255, b += p & 255, n++;
                        }
                    }
                    view.pixels[(long)ty * view.stride + tx] = (r / n) << 16 | (g / n) << 8 | b / n;
                }
            }
        } else {
            vx_blit_scaled(&view, x, y, dw, dh, &image->surface);
        }
        snprintf(status, sizeof(status), "%dx%d   %d%%%s   %d of %d%s", iw, ih, percent,
                 zoom_percent ? "" : " (fit)", picture_index + 1, picture_count,
                 slideshow ? "   slideshow" : "");
    } else {
        const char *text = path[0] ? "Can't read this picture (PNG, BMP and PPM are known)."
                                   : "Open a picture: the Open button, Ctrl+O, or drop one here.";
        vx_draw_text(s, 16, TOOLBAR + h / 2 - VX_LINE_HEIGHT / 2, text, VX_COLOR_DIM, VX_TRANSPARENT);
        snprintf(status, sizeof(status), "%s", path);
    }
    /* The toolbar. */
    vx_draw_toolbar(s, 0, 0, s->width, TOOLBAR);
    for (int b = 0; b < BUTTONS; b++) {
        if (button_x(b) + widths[b] > s->width - 4) {
            break;
        }
        vx_draw_button(s, button_x(b), 6, widths[b], 24, labels[b],
                       hot == b || (b == B_SLIDES && slideshow) || (b == B_FIT && !zoom_percent));
    }
    vx_draw_toolbar(s, 0, s->height - STATUS, s->width, STATUS);
    vx_draw_text_fit(s, 8, s->height - STATUS + 3, s->width - 16, status, VX_COLOR_DIM,
                     VX_TRANSPARENT);
    vx_window_present(window, 0, 0, s->width, s->height);
}

static void open_another(void) {
    char folder[512], file[512];
    folder_of(folder, sizeof(folder));
    if (vx_open_dialog("Open a Picture", path[0] ? folder : vx_home_folder("Pictures"), file, sizeof(file))) {
        load(file, false);
    }
}

static void press(int b) {
    int p = percent_now();
    switch (b) {
    case B_PREVIOUS: step(-1); break;
    case B_NEXT: step(1); break;
    case B_ZOOM_OUT: zoom(p * 4 / 5); break;
    case B_FIT: zoom_percent = pan_x = pan_y = 0; break;
    case B_ZOOM_IN: zoom(p * 5 / 4 + 1); break;
    case B_LEFT: rotate(false); break;
    case B_RIGHT: rotate(true); break;
    case B_SLIDES:
        slideshow = !slideshow;
        next_slide = vx_uptime() + SLIDE_MS;
        printf("view: slideshow %s\n", slideshow ? "on" : "off");
        fflush(stdout);
        break;
    case B_OPEN: open_another(); break;
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
    if (slideshow) { /* Any key stops it. */
        press(B_SLIDES);
        if (e->character != ' ' && e->key != 31) {
            return;
        }
        return;
    }
    if (ctrl && e->key == 24) { /* O */
        open_another();
        return;
    }
    bool zoomed = image && percent_now() > 0 &&
                  (image->surface.width * percent_now() / 100 > window->surface.width ||
                   image->surface.height * percent_now() / 100 >
                       window->surface.height - TOOLBAR - STATUS);
    switch (e->key) {
    case VX_KEY_LEFT: zoomed ? (void)(pan_x -= 40) : step(-1); return;
    case VX_KEY_RIGHT: zoomed ? (void)(pan_x += 40) : step(1); return;
    case VX_KEY_UP: pan_y -= 40; return;
    case VX_KEY_DOWN: pan_y += 40; return;
    case VX_KEY_PAGEUP: step(-1); return;
    case VX_KEY_PAGEDOWN: step(1); return;
    }
    switch (e->character) {
    case '+':
    case '=': press(B_ZOOM_IN); break;
    case '-': press(B_ZOOM_OUT); break;
    case '0': press(B_FIT); break;
    case '1': zoom(100); break;
    case 'r': rotate(true); break;
    case 'R': rotate(false); break;
    case ' ':
    case 's': press(B_SLIDES); break;
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    static int drag_x, drag_y;
    static bool dragging;
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    hot = -1;
    for (int b = 0; b < BUTTONS; b++) {
        if (vx_inside(e->x, e->y, button_x(b), 6, widths[b], 24)) {
            hot = b;
        }
    }
    if (e->wheel && e->y >= TOOLBAR) {
        int p = percent_now();
        zoom(e->wheel > 0 ? p * 5 / 4 + 1 : p * 4 / 5);
    }
    if (click && hot >= 0) {
        press(hot);
        return;
    }
    if (click && e->y >= TOOLBAR) {
        dragging = true;
        drag_x = e->x, drag_y = e->y;
        vx_window_set_cursor(window, VX_CURSOR_MOVE);
    } else if (dragging && (e->buttons & 1)) {
        pan_x -= e->x - drag_x;
        pan_y -= e->y - drag_y;
        drag_x = e->x, drag_y = e->y;
    } else if (!(e->buttons & 1) && dragging) {
        dragging = false;
        vx_window_set_cursor(window, VX_CURSOR_ARROW);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        load(argv[1], false);
    }
    /* The window: the picture's size, within reason. */
    int w = 640, h = 420;
    if (image) {
        w = image->surface.width < 560 ? 560 : image->surface.width > 800 ? 800
                                                                          : image->surface.width;
        h = image->surface.height < 200 ? 200 : image->surface.height > 500
                                                    ? 500 : image->surface.height;
    }
    char title[300];
    const char *name = path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    snprintf(title, sizeof(title), "%s%sImage Viewer", name ? name : "", name ? " - " : "");
    window = vx_window_create_flags(title, w, h + TOOLBAR + STATUS, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "view: no desktop to open a window on\n");
        return 1;
    }
    int held = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        long wait = slideshow ? next_slide - vx_uptime() : -1;
        int got = vx_gui_wait(&e, wait < 0 && slideshow ? 0 : wait);
        if (got < 0) {
            return 0;
        }
        if (got == 0) {
            if (slideshow && vx_uptime() >= next_slide) {
                step(1);
                next_slide = vx_uptime() + SLIDE_MS;
            }
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_RESIZE:
            if (e.width >= 200 && e.height >= 120) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = shift = false;
            }
            break;
        case VX_GUI_THEME: break;
        case VX_GUI_DROP: {
            char *paths = vx_drop_paths(&e);
            vx_remove(e.text);
            if (paths) {
                char *end = strchr(paths, '\n');
                if (end) {
                    *end = '\0';
                }
                if (paths[0] == '/') {
                    load(paths, false);
                }
                free(paths);
            }
            break;
        }
        }
    }
}
