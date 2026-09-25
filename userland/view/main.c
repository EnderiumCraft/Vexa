/* view: an image viewer window. `view file` shows a PNG, BMP or PPM image,
 * fitted to the window (never enlarged beyond its size unless zoomed);
 * + and - zoom, 0 fits it again.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vexa/font.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define STATUS 22
#define BACKGROUND 0x0e0818

static struct vx_window *window;
static struct vx_image *image;
static int zoom_percent; /* 0: fit to the window. */
static const char *path;

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height - STATUS;
    vx_fill(s, 0, 0, s->width, s->height, BACKGROUND);
    char status[256];
    if (image) {
        int iw = image->surface.width, ih = image->surface.height;
        int percent = zoom_percent;
        if (!percent) { /* Fit: the largest size that fits, at most 100%. */
            int px = w * 100 / iw, py = h * 100 / ih;
            percent = px < py ? px : py;
            percent = percent > 100 ? 100 : percent < 1 ? 1 : percent;
        }
        int dw = iw * percent / 100, dh = ih * percent / 100;
        dw = dw < 1 ? 1 : dw;
        dh = dh < 1 ? 1 : dh;
        struct vx_surface view = {s->pixels, w, h, s->stride};
        vx_blit_scaled(&view, (w - dw) / 2, (h - dh) / 2, dw, dh, &image->surface);
        snprintf(status, sizeof(status), "%dx%d   %d%%   + and - zoom, 0 fits", iw, ih, percent);
    } else {
        const char *text = path ? "Can't read this image (PNG, BMP and PPM are known)."
                                : "Open an image from Files, or: view <file>";
        vx_draw_text(s, 16, h / 2 - FONT_HEIGHT / 2, text, VX_COLOR_DIM, VX_TRANSPARENT);
        snprintf(status, sizeof(status), "%s", path ? path : "");
    }
    vx_fill(s, 0, s->height - STATUS, s->width, STATUS, VX_COLOR_WINDOW);
    vx_draw_text_fit(s, 8, s->height - STATUS + 3, s->width - 16, status, VX_COLOR_DIM,
                     VX_TRANSPARENT);
    vx_window_present(window, 0, 0, s->width, s->height);
}

int main(int argc, char **argv) {
    path = argc > 1 ? argv[1] : NULL;
    if (path) {
        image = vx_image_load(path, BACKGROUND);
    }
    /* The window: the image's size, within reason. */
    int w = 480, h = 320;
    if (image) {
        w = image->surface.width < 240 ? 240 : image->surface.width > 1000 ? 1000
                                                                           : image->surface.width;
        h = image->surface.height < 120 ? 120 : image->surface.height > 640
                                                    ? 640 : image->surface.height;
    }
    char title[300];
    const char *name = path ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    snprintf(title, sizeof(title), "%s%sImage Viewer", name ? name : "", name ? " - " : "");
    window = vx_window_create_flags(title, w, h + STATUS, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "view: no desktop to open a window on\n");
        return 1;
    }
    for (;;) {
        draw();
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            return 0;
        }
        if (e.type == VX_GUI_CLOSE) {
            vx_window_destroy(window);
            return 0;
        }
        if (e.type == VX_GUI_RESIZE && e.width >= 120 && e.height >= 80) {
            vx_window_resize(window, e.width, e.height);
        }
        if (e.type == VX_GUI_KEY && e.value && image) {
            int current = zoom_percent ? zoom_percent : 100;
            if (e.character == '+' || e.character == '=') {
                zoom_percent = current * 5 / 4 > 800 ? 800 : current * 5 / 4;
            } else if (e.character == '-') {
                zoom_percent = current * 4 / 5 < 5 ? 5 : current * 4 / 5;
            } else if (e.character == '0') {
                zoom_percent = 0;
            }
        }
    }
}
