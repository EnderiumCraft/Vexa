/* Alt+Tab: while Alt is held, a row of the windows (each a small picture of
 * it, most recently used first); Tab goes to the next, Shift+Tab back, and
 * letting go of Alt brings the chosen one to the front. */
#include <stdio.h>
#include <string.h>
#include "shell.h"

#define TILE_W 184
#define TILE_H 148
#define THUMB_W 164
#define THUMB_H 104
#define PAD 16

bool switcher_open;
static struct window *mru[MAX_WINDOWS];
static int mru_count;
static struct window *list[MAX_WINDOWS];
static int count, chosen;
static int columns, rows_shown;

void mru_touch(struct window *w) {
    mru_forget(w);
    memmove(mru + 1, mru, (size_t)mru_count * sizeof(mru[0]));
    mru[0] = w;
    mru_count++;
}

void mru_forget(struct window *w) {
    for (int i = 0; i < mru_count; i++) {
        if (mru[i] == w) {
            memmove(mru + i, mru + i + 1, (size_t)(mru_count - i - 1) * sizeof(mru[0]));
            mru_count--;
            break;
        }
    }
    for (int i = 0; i < count; i++) {
        if (list[i] == w) {
            memmove(list + i, list + i + 1, (size_t)(count - i - 1) * sizeof(list[0]));
            count--;
            chosen = chosen >= count ? count - 1 : chosen;
            if (switcher_open) {
                add_damage(switcher_rect());
            }
            if (!count) {
                switcher_open = false;
            }
            break;
        }
    }
}

struct rect switcher_rect(void) {
    int width = columns * TILE_W + 2 * PAD, height = rows_shown * TILE_H + 2 * PAD;
    return (struct rect){(screen.width - width) / 2, (screen.height - height) / 2, width, height};
}

static struct rect tile_rect(int i) {
    struct rect r = switcher_rect();
    return (struct rect){r.x + PAD + i % columns * TILE_W, r.y + PAD + i / columns * TILE_H, TILE_W,
                         TILE_H};
}

void switcher_start(bool backwards) {
    count = 0;
    for (int i = 0; i < mru_count; i++) {
        if (!mru[i]->popup) {
            list[count++] = mru[i];
        }
    }
    if (count == 0) {
        return;
    }
    int fit = (screen.width - 80) / TILE_W;
    columns = count < fit ? count : fit > 0 ? fit : 1;
    rows_shown = (count + columns - 1) / columns;
    switcher_open = true;
    chosen = count == 1 ? 0 : backwards ? count - 1 : 1;
    add_damage(switcher_rect());
    printf("desktop: switcher with %d window%s\n", count, count == 1 ? "" : "s");
}

void switcher_step(bool backwards) {
    if (!switcher_open || !count) {
        return;
    }
    chosen = (chosen + (backwards ? count - 1 : 1)) % count;
    add_damage(switcher_rect());
}

void switcher_finish(bool choose) {
    if (!switcher_open) {
        return;
    }
    add_damage(switcher_rect());
    switcher_open = false;
    if (choose && chosen >= 0 && chosen < count) {
        printf("desktop: switched to window %d\n", list[chosen]->id);
        activate(list[chosen]);
    }
}

/* A click on a tile chooses it; anywhere else, nothing. */
void switcher_click(void) {
    for (int i = 0; i < count; i++) {
        if (inside(tile_rect(i), pointer_x, pointer_y)) {
            chosen = i;
            switcher_finish(true);
            return;
        }
    }
    switcher_finish(false);
}

void switcher_draw(struct vx_surface *view, int ox, int oy) {
    struct rect r = switcher_rect();
    r.x += ox, r.y += oy;
    draw_glass_popup(view, r, 12, 160);
    for (int i = 0; i < count; i++) {
        struct window *w = list[i];
        struct rect t = tile_rect(i);
        t.x += ox, t.y += oy;
        if (i == chosen) {
            fill_gel(view, (struct rect){t.x + 2, t.y + 2, t.width - 4, t.height - 4}, 8,
                     vx_theme.accent, 150);
            outline_rounded(view, (struct rect){t.x + 2, t.y + 2, t.width - 4, t.height - 4}, 8,
                            vx_theme.accent);
        }
        /* The window's picture, its shape kept. */
        int cw = w->content.width, ch = w->content.height + (w->undecorated ? 0 : TITLE_HEIGHT);
        int tw = THUMB_W, th = cw ? ch * THUMB_W / cw : THUMB_H;
        if (th > THUMB_H) {
            th = THUMB_H;
            tw = ch ? cw * THUMB_H / ch : THUMB_W;
        }
        int tx = t.x + (TILE_W - tw) / 2, ty = t.y + 10 + (THUMB_H - th) / 2;
        struct rect content = {tx, ty, tw, th};
        if (!w->undecorated && ch) {
            int title = TITLE_HEIGHT * th / ch;
            vx_fill(view, tx, ty, tw, title, w == focused ? vx_theme.title_focused : vx_theme.title);
            content.y += title;
            content.height -= title;
        }
        blit_smooth(view, content, &w->content, w->minimized ? 140 : 255);
        vx_draw_outline(view, tx, ty, tw, th, vx_theme.line);
        char title[80];
        snprintf(title, sizeof(title), "%s", w->title);
        int width = vx_text_width(title);
        int text_x = width < TILE_W - 16 ? t.x + (TILE_W - width) / 2 : t.x + 8;
        vx_draw_text_fit(view, text_x, t.y + TILE_H - 30, TILE_W - 16, title,
                         i == chosen ? vx_theme.text : vx_theme.dim, VX_TRANSPARENT);
    }
}
