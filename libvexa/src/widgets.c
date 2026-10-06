#include <stdbool.h>
#include <string.h>
#include <vexa/gui.h>

/* Drawing for the widgets of Vexa's own apps (see <vexa/gui.h>). */

/* ---- Round, glossy shapes ---- */

/* `a` (0 to 256) of src over dst, red and blue together, green on its own.
 * (Each lane's weighted sum fits its 16 bits, so nothing carries into the
 * next one: unlike adding a masked difference, which borrows when src is
 * darker than dst and leaves a red cast.) */
static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a) {
    uint32_t rb = (((dst & 0xff00ff) * (256 - a) + (src & 0xff00ff) * a) >> 8) & 0xff00ff;
    uint32_t g = (((dst & 0x00ff00) * (256 - a) + (src & 0x00ff00) * a) >> 8) & 0x00ff00;
    return rb | g;
}

/* `alpha` of `color` over a row of pixels, clipped to the surface. */
static void blend_span(struct vx_surface *s, int x, int y, int width, uint32_t color, int alpha) {
    if (y < 0 || y >= s->height || alpha <= 0) {
        return;
    }
    if (x < 0) {
        width += x, x = 0;
    }
    if (x + width > s->width) {
        width = s->width - x;
    }
    uint32_t *p = s->pixels + (long)y * s->stride + x;
    unsigned a = alpha >= 255 ? 256 : (unsigned)alpha;
    for (int i = 0; i < width; i++) {
        p[i] = a == 256 ? color : blend(p[i], color, a);
    }
}

int vx_corner_inset(int radius, int row, int *coverage) {
    /* The circle's x at the middle of the row, from its center. */
    int dy2 = (2 * (radius - row) - 1) * (2 * (radius - row) - 1); /* (2*dy)^2 */
    int r2 = 4 * radius * radius;
    int inset = 0;
    while (inset < radius) {
        int dx = 2 * (radius - inset) - 1;
        if (dx * dx + dy2 <= r2) {
            break;
        }
        inset++;
    }
    /* The pixel just outside: part of it is in. */
    *coverage = 0;
    if (inset > 0) {
        int dx = 2 * (radius - inset) + 1;
        int over = dx * dx + dy2 - r2; /* How far past the edge. */
        int span = 4 * (radius - inset) + 2;
        *coverage = over >= span ? 0 : 255 - over * 255 / span;
    }
    return inset;
}

/* Where row `row` of a rounded rectangle starts (inset from each side), and
 * the coverage of the pixel outside that. */
static int row_inset(int radius, int height, int row, int *coverage) {
    int from_edge = row < radius ? row : height - 1 - row < radius ? height - 1 - row : -1;
    *coverage = 0;
    return from_edge >= 0 ? vx_corner_inset(radius, from_edge, coverage) : 0;
}

static int fit_radius(int radius, int width, int height) {
    radius = radius * 2 > height ? height / 2 : radius;
    return radius * 2 > width ? width / 2 : radius;
}

void vx_fill_rounded(struct vx_surface *s, int x, int y, int width, int height, int radius,
                     uint32_t color, int alpha) {
    radius = fit_radius(radius, width, height);
    for (int row = 0; row < height; row++) {
        int coverage, inset = row_inset(radius, height, row, &coverage);
        blend_span(s, x + inset, y + row, width - 2 * inset, color, alpha);
        if (inset > 0 && coverage > 0) {
            int edge = coverage * (alpha >= 255 ? 255 : alpha) / 255;
            blend_span(s, x + inset - 1, y + row, 1, color, edge);
            blend_span(s, x + width - inset, y + row, 1, color, edge);
        }
    }
}

uint32_t vx_gel_color(uint32_t color, int row, int height) {
    int t = height > 1 ? row * 255 / (height - 1) : 0;
    if (t < 128) { /* The shine: bright at the top, fading to the middle. */
        return vx_mix(color, 0xffffff, 150 - t * 90 / 128);
    }
    /* Below it, a little deeper, and lighter again at the bottom: a glow. */
    return vx_mix(vx_mix(color, 0x000000, 20), 0xffffff, (t - 128) * 100 / 127);
}

void vx_draw_gel(struct vx_surface *s, int x, int y, int width, int height, int radius,
                 uint32_t color) {
    if (width < 3 || height < 3) {
        return;
    }
    vx_fill_rounded(s, x, y, width, height, radius, vx_mix(color, 0x000000, 100), 255);
    x++, y++, width -= 2, height -= 2;
    radius = fit_radius(radius > 1 ? radius - 1 : radius, width, height);
    for (int row = 0; row < height; row++) {
        int coverage, inset = row_inset(radius, height, row, &coverage);
        uint32_t c = vx_gel_color(color, row, height);
        blend_span(s, x + inset, y + row, width - 2 * inset, c, 255);
        if (inset > 0 && coverage > 0) {
            blend_span(s, x + inset - 1, y + row, 1, c, coverage);
            blend_span(s, x + width - inset, y + row, 1, c, coverage);
        }
    }
    /* A thin highlight along the top, inside the edge. */
    int coverage, inset = row_inset(radius, height, 0, &coverage);
    blend_span(s, x + inset + 1, y, width - 2 * inset - 2, 0xffffff, 120);
}

/* The neutral color of buttons (gel), for the theme. */
static uint32_t button_color(bool hot) {
    return hot ? VX_COLOR_ACCENT : vx_theme.dark ? 0x46464a : 0xdcdce4;
}

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

void vx_draw_button_flags(struct vx_surface *s, int x, int y, int width, int height,
                          const char *label, unsigned flags) {
    bool hot = (flags & VX_BUTTON_HOT) && !(flags & VX_BUTTON_DISABLED);
    uint32_t color = button_color(hot);
    if (flags & VX_BUTTON_DISABLED) {
        color = vx_mix(color, VX_COLOR_WINDOW, 120);
    }
    vx_draw_gel(s, x, y, width, height, height / 2 < 9 ? height / 2 : 9, color);
    int text = vx_text_width(label);
    int tx = x + (width > text + 8 ? (width - text) / 2 : 4), ty = y + (height - VX_LINE_HEIGHT) / 2;
    /* On the accent: white, over a darker copy (the text looks set in). */
    if (hot) {
        vx_draw_text_fit(s, tx, ty + 1, width - 8, label, vx_mix(color, 0x000000, 110),
                         VX_TRANSPARENT);
    }
    uint32_t fg = hot ? 0xffffff : flags & VX_BUTTON_DISABLED ? VX_COLOR_DIM : VX_COLOR_TEXT;
    vx_draw_text_fit(s, tx, ty, width - 8, label, fg, VX_TRANSPARENT);
}

void vx_draw_button(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                    bool hot) {
    vx_draw_button_flags(s, x, y, width, height, label, hot ? VX_BUTTON_HOT : 0);
}

void vx_draw_toolbar(struct vx_surface *s, int x, int y, int width, int height) {
    uint32_t top = vx_theme.dark ? 0x48484c : 0xf4f5f8, bottom = vx_theme.dark ? 0x2a2a2d : 0xd6d9e0;
    for (int row = 0; row < height - 1; row++) {
        vx_fill(s, x, y + row, width, 1, vx_mix(top, bottom, row * 255 / (height > 2 ? height - 2 : 1)));
    }
    vx_fill(s, x, y, width, 1, vx_mix(top, 0xffffff, vx_theme.dark ? 30 : 160));
    vx_fill(s, x, y + height - 1, width, 1, vx_mix(VX_COLOR_LINE, 0x000000, vx_theme.dark ? 80 : 40));
}

void vx_draw_tab(struct vx_surface *s, int x, int y, int width, int height, const char *label,
                 bool chosen) {
    if (chosen) {
        vx_draw_gel(s, x, y, width, height, 7, VX_COLOR_ACCENT);
        vx_draw_text_fit(s, x + 9, y + (height - VX_LINE_HEIGHT) / 2 + 1, width - 30, label,
                         vx_mix(VX_COLOR_ACCENT, 0x000000, 110), VX_TRANSPARENT);
    } else {
        vx_fill_rounded(s, x, y, width, height, 7, vx_theme.dark ? 0xffffff : 0x000000, 18);
    }
    vx_draw_text_fit(s, x + 9, y + (height - VX_LINE_HEIGHT) / 2, width - 30, label,
                     chosen ? 0xffffff : VX_COLOR_TEXT, VX_TRANSPARENT);
}

void vx_draw_selection(struct vx_surface *s, int x, int y, int width, int height) {
    vx_draw_gel(s, x, y, width, height, height / 2 < 6 ? height / 2 : 6, VX_COLOR_SELECTED);
}

void vx_draw_progress(struct vx_surface *s, int x, int y, int width, int height,
                      unsigned long long done, unsigned long long total) {
    int radius = height / 2;
    vx_fill_rounded(s, x, y, width, height, radius, vx_mix(VX_COLOR_LINE, 0x000000, 40), 255);
    vx_fill_rounded(s, x + 1, y + 1, width - 2, height - 2, radius - 1, VX_COLOR_BUTTON_HOT, 255);
    int filled = total ? (int)((unsigned long long)width * (done > total ? total : done) / total) : 0;
    if (filled >= height) {
        vx_draw_gel(s, x, y, filled, height, radius, VX_COLOR_ACCENT);
    }
}

void vx_draw_sheet(struct vx_surface *s, int x, int y, int width, int height) {
    vx_fill_rounded(s, x - 2, y + 1, width + 4, height + 5, 12, 0x000000, 28);
    vx_fill_rounded(s, x - 1, y + 2, width + 2, height + 2, 11, 0x000000, 40);
    vx_fill_rounded(s, x, y, width, height, 10, vx_mix(VX_COLOR_LINE, 0x000000, 30), 255);
    vx_fill_rounded(s, x + 1, y + 1, width - 2, height - 2, 9, VX_COLOR_WINDOW, 255);
    vx_fill(s, x + 10, y + 1, width - 20, 1, vx_mix(VX_COLOR_WINDOW, 0xffffff, 120));
}

void vx_draw_check(struct vx_surface *s, int x, int y, bool on) {
    if (!on) {
        vx_fill_rounded(s, x, y, 16, 16, 4, vx_mix(VX_COLOR_LINE, 0x000000, 30), 255);
        vx_fill_rounded(s, x + 1, y + 1, 14, 14, 3, VX_COLOR_VIEW, 255);
        vx_fill(s, x + 3, y + 1, 10, 1, vx_mix(VX_COLOR_VIEW, 0x000000, 28));
        return;
    }
    vx_draw_gel(s, x, y, 16, 16, 4, VX_COLOR_ACCENT);
    for (int i = 0; i < 4; i++) {
        vx_fill(s, x + 3 + i, y + 7 + i, 2, 2, 0xffffff);
    }
    for (int i = 0; i < 7; i++) {
        vx_fill(s, x + 6 + i, y + 10 - i, 2, 2, 0xffffff);
    }
}

void vx_draw_field(struct vx_surface *s, int x, int y, int width, const char *text, bool focused) {
    int height = VX_LINE_HEIGHT + 8;
    /* A sunken, rounded box; focused: a ring of the accent. */
    vx_fill_rounded(s, x, y, width, height, 5, focused ? VX_COLOR_ACCENT : VX_COLOR_LINE, 255);
    if (focused) {
        vx_fill_rounded(s, x + 1, y + 1, width - 2, height - 2, 4,
                        vx_mix(VX_COLOR_ACCENT, VX_COLOR_VIEW, 120), 255);
    }
    vx_fill_rounded(s, x + 2, y + 2, width - 4, height - 4, 3, VX_COLOR_VIEW, 255);
    vx_fill(s, x + 4, y + 2, width - 8, 1, vx_mix(VX_COLOR_VIEW, 0x000000, 28));
    const char *shown = text; /* Its end, if it's long. */
    while (*shown && vx_text_width(shown) > width - 12) {
        vx_utf8_next(&shown);
    }
    struct vx_surface inside = {s->pixels, x + width - 2 < s->width ? x + width - 2 : s->width,
                                s->height, s->stride};
    int end = vx_draw_text(&inside, x + 5, y + 4, shown, VX_COLOR_TEXT, VX_TRANSPARENT);
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
    /* A soft shadow, then the menu: rounded, a little lighter at the top. */
    vx_fill_rounded(s, x + 1, y + 2, width + 2, height + 1, 8, 0x000000, 40);
    vx_fill_rounded(s, x + 2, y + 3, width, height, 7, 0x000000, 50);
    vx_fill_rounded(s, x, y, width, height, 7, VX_COLOR_LINE, 255);
    vx_fill_rounded(s, x + 1, y + 1, width - 2, height - 2, 6, VX_COLOR_VIEW, 255);
    vx_fill(s, x + 6, y + 1, width - 12, 1, vx_mix(VX_COLOR_VIEW, 0xffffff, 70));
    int top = y + 4;
    for (int i = 0; i < count; i++) {
        int h = menu_item_height(&items[i]);
        if (!items[i].label) {
            vx_fill(s, x + 8, top + h / 2, width - 16, 1, VX_COLOR_LINE);
        } else {
            bool lit = i == hot && !items[i].disabled;
            if (lit) {
                vx_draw_gel(s, x + 4, top, width - 8, h, 5, VX_COLOR_ACCENT);
            }
            uint32_t color = lit ? 0xffffff : items[i].disabled ? VX_COLOR_DIM : VX_COLOR_TEXT;
            if (lit) {
                vx_draw_text(s, x + 14, top + (h - VX_LINE_HEIGHT) / 2 + 1, items[i].label,
                             vx_mix(VX_COLOR_ACCENT, 0x000000, 120), VX_TRANSPARENT);
            }
            vx_draw_text(s, x + 14, top + (h - VX_LINE_HEIGHT) / 2, items[i].label, color,
                         VX_TRANSPARENT);
            if (items[i].keys) {
                int keys = vx_text_width(items[i].keys);
                vx_draw_text(s, x + width - keys - 14, top + (h - VX_LINE_HEIGHT) / 2, items[i].keys,
                             lit ? vx_mix(VX_COLOR_ACCENT, 0xffffff, 190) : VX_COLOR_DIM,
                             VX_TRANSPARENT);
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
