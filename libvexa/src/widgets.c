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

static int menu_item_height(const struct vx_menu_item *item) {
    return item->label ? VX_MENU_ITEM_HEIGHT : VX_MENU_SEPARATOR_HEIGHT;
}

void vx_menu_size(const struct vx_menu_item *items, int count, int *width, int *height) {
    int widest = 0;
    *height = 8;
    for (int i = 0; i < count; i++) {
        *height += menu_item_height(&items[i]);
        if (items[i].label) {
            int w = (int)strlen(items[i].label) * FONT_WIDTH;
            if (items[i].keys) {
                w += (int)(strlen(items[i].keys) + 3) * FONT_WIDTH;
            }
            widest = w > widest ? w : widest;
        }
    }
    *width = widest + 32 < 160 ? 160 : widest + 32;
}

void vx_draw_menu(struct vx_surface *s, int x, int y, const struct vx_menu_item *items, int count,
                  int hot) {
    int width, height;
    vx_menu_size(items, count, &width, &height);
    vx_fill(s, x + 3, y + 3, width, height, 0x06030c); /* A shadow. */
    vx_fill(s, x, y, width, height, VX_COLOR_VIEW);
    vx_draw_outline(s, x, y, width, height, VX_COLOR_LINE);
    int top = y + 4;
    for (int i = 0; i < count; i++) {
        int h = menu_item_height(&items[i]);
        if (!items[i].label) {
            vx_fill(s, x + 8, top + h / 2, width - 16, 1, VX_COLOR_LINE);
        } else {
            if (i == hot && !items[i].disabled) {
                vx_fill(s, x + 4, top, width - 8, h, VX_COLOR_SELECTED);
            }
            uint32_t color = items[i].disabled ? VX_COLOR_DIM : VX_COLOR_TEXT;
            vx_draw_text(s, x + 14, top + (h - FONT_HEIGHT) / 2, items[i].label, color,
                         VX_TRANSPARENT);
            if (items[i].keys) {
                int keys = (int)strlen(items[i].keys) * FONT_WIDTH;
                vx_draw_text(s, x + width - keys - 14, top + (h - FONT_HEIGHT) / 2, items[i].keys,
                             VX_COLOR_DIM, VX_TRANSPARENT);
            }
        }
        top += h;
    }
}

int vx_menu_item_at(const struct vx_menu_item *items, int count, int x, int y, int px, int py) {
    int width, height;
    vx_menu_size(items, count, &width, &height);
    if (!vx_inside(px, py, x, y, width, height)) {
        return -1;
    }
    int top = y + 4;
    for (int i = 0; i < count; i++) {
        int h = menu_item_height(&items[i]);
        if (py >= top && py < top + h) {
            return items[i].label && !items[i].disabled ? i : -1;
        }
        top += h;
    }
    return -1;
}
