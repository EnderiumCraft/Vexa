#include <stdbool.h>
#include <string.h>
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
    const struct vx_font *f = vx_font_ui();
    if (width <= 0) {
        return;
    }
    if (vx_text_width_font(f, text) <= width) {
        vx_draw_text(s, x, y, text, fg, bg);
        return;
    }
    static const char ellipsis[] = "\xe2\x80\xa6"; /* U+2026 */
    char line[512];
    size_t keep = vx_text_fit_bytes(f, text, width - vx_text_width_font(f, ellipsis));
    if (keep > sizeof(line) - sizeof(ellipsis)) {
        keep = sizeof(line) - sizeof(ellipsis);
    }
    while (keep > 0 && text[keep - 1] == ' ') {
        keep--;
    }
    memcpy(line, text, keep);
    strcpy(line + keep, ellipsis);
    vx_draw_text(s, x, y, line, fg, bg);
}

void vx_draw_button(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                    bool hot) {
    vx_fill(s, x, y, width, height, hot ? VX_COLOR_BUTTON_HOT : VX_COLOR_BUTTON);
    vx_draw_outline(s, x, y, width, height, VX_COLOR_LINE);
    int text = vx_text_width(label);
    vx_draw_text_fit(s, x + (width > text + 8 ? (width - text) / 2 : 4),
                     y + (height - VX_LINE_HEIGHT) / 2, width - 8, label, VX_COLOR_TEXT,
                     VX_TRANSPARENT);
}

void vx_draw_field(struct vx_surface *s, int x, int y, int width, const char *text, bool focused) {
    int height = VX_LINE_HEIGHT + 8;
    vx_fill(s, x, y, width, height, VX_COLOR_VIEW);
    vx_draw_outline(s, x, y, width, height, focused ? VX_COLOR_ACCENT : VX_COLOR_LINE);
    const char *shown = text; /* Its end, if it's long. */
    while (*shown && vx_text_width(shown) > width - 12) {
        vx_utf8_next(&shown);
    }
    struct vx_surface inside = {s->pixels, x + width - 2 < s->width ? x + width - 2 : s->width,
                                s->height, s->stride};
    int end = vx_draw_text(&inside, x + 4, y + 4, shown, VX_COLOR_TEXT, VX_TRANSPARENT);
    if (focused) {
        vx_fill(s, end + 1, y + 4, 1, VX_LINE_HEIGHT, VX_COLOR_ACCENT);
    }
}

bool vx_field_key(char *text, size_t size, const struct vx_gui_event *event) {
    if (event->type != VX_GUI_KEY || event->value == 0) {
        return false;
    }
    size_t length = strlen(text);
    if (event->character == '\b') {
        if (length) {
            text[vx_utf8_previous(text, length)] = '\0';
            return true;
        }
        return false;
    }
    if (event->character >= ' ' && event->character != 127) {
        char bytes[4];
        int n = vx_utf8_encode((uint32_t)event->character, bytes);
        if (length + (size_t)n < size) {
            memcpy(text + length, bytes, (size_t)n);
            text[length + (size_t)n] = '\0';
            return true;
        }
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
            int w = vx_text_width(items[i].label);
            if (items[i].keys) {
                w += vx_text_width(items[i].keys) + 24;
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
    vx_fill(s, x + 3, y + 3, width, height, vx_theme.shadow); /* A shadow. */
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
            vx_draw_text(s, x + 14, top + (h - VX_LINE_HEIGHT) / 2, items[i].label, color,
                         VX_TRANSPARENT);
            if (items[i].keys) {
                int keys = vx_text_width(items[i].keys);
                vx_draw_text(s, x + width - keys - 14, top + (h - VX_LINE_HEIGHT) / 2, items[i].keys,
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
