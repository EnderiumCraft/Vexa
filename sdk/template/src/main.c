/* A Vexa app: a window with some text and a button that counts clicks.
 * Start from here: draw() paints the window, main() handles events.
 * <vexa/gui.h> has the drawing and windows; the C library is the usual one
 * (stdio, stdlib, string, math, pthreads, time...). */
#include <stdio.h>
#include <string.h>
#include <vexa/gui.h>

#define BUTTON_WIDTH 120
#define BUTTON_HEIGHT 30

struct app {
    struct vx_window *window;
    int clicks;
    bool button_hot;   /* The pointer is over the button. */
    char typed[64];    /* The last keys typed. */
};

static void button_rect(const struct app *app, int *x, int *y) {
    *x = (app->window->surface.width - BUTTON_WIDTH) / 2;
    *y = app->window->surface.height - BUTTON_HEIGHT - 24;
}

static void draw(struct app *app) {
    struct vx_surface *s = &app->window->surface;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);

    const struct vx_font *big = vx_font(VX_FACE_BOLD, 28);
    vx_text(s, big, 24, 20, "Hello, Vexa!", VX_COLOR_ACCENT, VX_TRANSPARENT);
    int y = 20 + vx_font_height(big) + 8;
    vx_draw_text(s, 24, y, "This app was built with the Vexa SDK.", VX_COLOR_TEXT, VX_TRANSPARENT);
    y += VX_LINE_HEIGHT + 6;

    char line[128];
    snprintf(line, sizeof line, "Clicks: %d", app->clicks);
    vx_draw_text(s, 24, y, line, VX_COLOR_TEXT, VX_TRANSPARENT);
    y += VX_LINE_HEIGHT + 6;
    snprintf(line, sizeof line, "Typed: %s", app->typed[0] ? app->typed : "(type something)");
    vx_draw_text_fit(s, 24, y, s->width - 48, line, VX_COLOR_DIM, VX_TRANSPARENT);

    int bx, by;
    button_rect(app, &bx, &by);
    vx_draw_button(s, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, "Click me", app->button_hot);
    vx_window_present(app->window, 0, 0, s->width, s->height);
}

static void typed(struct app *app, int character) {
    char bytes[4];
    int n = vx_utf8_encode((unsigned)character, bytes);
    size_t used = strlen(app->typed);
    if (used + (size_t)n >= sizeof app->typed) { /* Full: keep the end. */
        memmove(app->typed, app->typed + 16, used - 16 + 1);
        used -= 16;
    }
    memcpy(app->typed + used, bytes, (size_t)n);
    app->typed[used + (size_t)n] = '\0';
}

int main(void) {
    struct app app = {0};
    app.window = vx_window_create_flags("Hello", 420, 220, VX_WINDOW_RESIZABLE);
    if (!app.window) {
        fprintf(stderr, "hello: no desktop to open a window on\n");
        return 1;
    }
    printf("hello: running\n");
    draw(&app);

    struct vx_gui_event e;
    while (vx_gui_wait(&e, -1) > 0) {
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(app.window);
            return 0;
        case VX_GUI_RESIZE:
            if (vx_window_resize(app.window, e.width, e.height) == 0) {
                draw(&app);
            }
            break;
        case VX_GUI_THEME:
            draw(&app);
            break;
        case VX_GUI_POINTER: {
            int bx, by;
            button_rect(&app, &bx, &by);
            bool over = vx_inside(e.x, e.y, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT);
            bool pressed = (e.buttons & 1) && over;
            if (pressed) {
                app.clicks++;
            }
            if (pressed || over != app.button_hot) {
                app.button_hot = over;
                draw(&app);
            }
            break;
        }
        case VX_GUI_KEY:
            if (e.value != 0 && e.character >= ' ') {
                typed(&app, e.character);
                draw(&app);
            }
            break;
        }
    }
    return 0; /* The desktop is gone. */
}
