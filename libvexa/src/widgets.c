#include <stdbool.h>
#include <string.h>
#include <vexa/font.h>
#include <vexa/gui.h>

/* Drawing for the widgets of Vexa's own apps (see <vexa/gui.h>). */

void vx_draw_outline(struct vx_surface *s, int x, int y, int width, int height, uint32_t color) {
    vx_fill(s, x, y, width, 1, color);
    vx_fill(s, x, y + height - 1, width, 1, color);
    vx_fill(s, x, y, 1, height, color);
    vx_fill(s, x + width - 1, y, 1, height, color);
}

void vx_draw_text_fit(struct vx_surface *s, int x, int y, int width, const char *text,
                      uint32_t fg, uint32_t bg) {
    int fits = width / FONT_WIDTH;
    int length = (int)strlen(text);
    if (fits <= 0) {
        return;
    }
    if (length <= fits) {
        vx_draw_text(s, x, y, text, fg, bg);
        return;
    }
    char line[256];
    if (fits > (int)sizeof(line) - 1) {
        fits = (int)sizeof(line) - 1;
    }
    int keep = fits > 3 ? fits - 3 : fits;
    memcpy(line, text, (size_t)keep);
    strcpy(line + keep, fits > 3 ? "..." : "");
    vx_draw_text(s, x, y, line, fg, bg);
}

void vx_draw_button(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                    bool hot) {
    vx_fill(s, x, y, width, height, hot ? VX_COLOR_BUTTON_HOT : VX_COLOR_BUTTON);
    vx_draw_outline(s, x, y, width, height, VX_COLOR_LINE);
    int text = (int)strlen(label) * FONT_WIDTH;
    vx_draw_text_fit(s, x + (width > text ? (width - text) / 2 : 4), y + (height - FONT_HEIGHT) / 2,
                     width - 8, label, VX_COLOR_TEXT, VX_TRANSPARENT);
}

void vx_draw_field(struct vx_surface *s, int x, int y, int width, const char *text, bool focused) {
    int height = FONT_HEIGHT + 8;
    vx_fill(s, x, y, width, height, VX_COLOR_VIEW);
    vx_draw_outline(s, x, y, width, height, focused ? VX_COLOR_ACCENT : VX_COLOR_LINE);
    int fits = (width - 12) / FONT_WIDTH;
    int length = (int)strlen(text);
    const char *shown = length > fits ? text + (length - fits) : text; /* Its end. */
    int end = vx_draw_text(s, x + 4, y + 4, shown, VX_COLOR_TEXT, VX_TRANSPARENT);
    if (focused) {
        vx_fill(s, end, y + 4, 2, FONT_HEIGHT, VX_COLOR_ACCENT);
    }
}

bool vx_field_key(char *text, size_t size, const struct vx_gui_event *event) {
    if (event->type != VX_GUI_KEY || event->value == 0) {
        return false;
    }
    size_t length = strlen(text);
    if (event->character == '\b') {
        if (length) {
            text[length - 1] = '\0';
            return true;
        }
        return false;
    }
    if (event->character >= ' ' && event->character < 127 && length + 1 < size) {
        text[length] = (char)event->character;
        text[length + 1] = '\0';
        return true;
    }
    return false;
}

bool vx_inside(int px, int py, int x, int y, int width, int height) {
    return px >= x && py >= y && px < x + width && py < y + height;
}
