/* What Music and Videos draw alike, in Vexa's look: gradients, the dark
 * sidebar tinted with the accent, generated cover art, the transport's
 * icons, gel scrubbers and sliders. (Videos includes it from here.) */
#ifndef MEDIA_LOOK_H
#define MEDIA_LOOK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>

static inline void lk_gradient(struct vx_surface *s, int x, int y, int w, int h, uint32_t top,
                               uint32_t bottom) {
    for (int row = 0; row < h; row++) {
        vx_fill(s, x, y + row, w, 1, vx_mix(top, bottom, h > 1 ? row * 255 / (h - 1) : 0));
    }
}

static inline void lk_circle(struct vx_surface *s, int cx, int cy, int r, uint32_t color,
                             int alpha) {
    vx_fill_rounded(s, cx - r, cy - r, 2 * r, 2 * r, r, color, alpha);
}

/* A line `t` pixels thick. */
static inline void lk_line(struct vx_surface *s, int x0, int y0, int x1, int y1, int t,
                           uint32_t color) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), steps = dx > dy ? dx : dy;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / (steps ? steps : 1);
        int y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        vx_fill(s, x - t / 2, y - t / 2, t, t, color);
    }
}

/* A triangle `h` tall pointing right (or left), its left (or right) edge at x. */
static inline void lk_triangle(struct vx_surface *s, int x, int cy, int h, bool left,
                               uint32_t color) {
    int w = h * 7 / 8;
    for (int i = 0; i < w; i++) {
        int len = h - i * h / w;
        vx_fill(s, left ? x - i : x + i, cy - len / 2, 1, len, color);
    }
}

static inline uint32_t lk_hsv(int h, int sat, int v) {
    h = (h % 360 + 360) % 360;
    int c = v * sat / 255, x = c * (60 - abs(h % 120 - 60)) / 60, m = v - c;
    int r = 0, g = 0, b = 0;
    switch (h / 60) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    return (uint32_t)(r + m) << 16 | (uint32_t)(g + m) << 8 | (uint32_t)(b + m);
}

static inline unsigned lk_hash(const char *text) {
    unsigned h = 2166136261u;
    for (; *text; text++) {
        h = (h ^ (unsigned char)*text) * 16777619u;
    }
    return h;
}

/* The sidebar's colors: dark, tinted with the accent. */
static inline void lk_sidebar_colors(uint32_t *top, uint32_t *bottom) {
    *top = vx_mix(0x1c2033, VX_COLOR_ACCENT, 70);
    *bottom = vx_mix(0x0d0f18, VX_COLOR_ACCENT, 25);
}

static inline void lk_sidebar(struct vx_surface *s, int x, int y, int w, int h) {
    uint32_t top, bottom;
    lk_sidebar_colors(&top, &bottom);
    lk_gradient(s, x, y, w, h, top, bottom);
    for (int row = 0; row < 110 && row < h; row++) {
        vx_fill_rounded(s, x, y + row, w, 1, 0, 0xffffff, (110 - row) * 16 / 110);
    }
    vx_fill(s, x + w - 1, y, 1, h, vx_mix(bottom, 0x000000, 80));
}

static inline void lk_centered(struct vx_surface *s, const struct vx_font *f, int cx, int y,
                               const char *t, uint32_t color) {
    vx_text(s, f, cx - vx_text_width_font(f, t) / 2, y, t, color, VX_TRANSPARENT);
}

/* A note (two eighths joined), in a box of `size`. */
static inline void lk_note(struct vx_surface *s, int x, int y, int size, uint32_t color) {
    int r = size / 7 > 2 ? size / 7 : 2, t = size / 12 > 1 ? size / 12 : 1;
    int ax = x + size * 3 / 10, bx = x + size * 7 / 10;
    int ay = y + size * 3 / 4, by = y + size * 2 / 3;
    vx_fill_rounded(s, ax - r - r / 2, ay - r, 2 * r + r / 2, 2 * r, r, color, 255);
    vx_fill_rounded(s, bx - r - r / 2, by - r, 2 * r + r / 2, 2 * r, r, color, 255);
    int top_a = y + size / 4, top_b = y + size / 6;
    vx_fill(s, ax + r / 2, top_a, t + 1, ay - top_a, color);
    vx_fill(s, bx + r / 2, top_b, t + 1, by - top_b, color);
    lk_line(s, ax + r / 2, top_a, bx + r / 2 + t, top_b, t + 2, color);
}

/* A film frame with a triangle: Videos' mark. */
static inline void lk_film(struct vx_surface *s, int x, int y, int size, uint32_t color) {
    int w = size * 8 / 10, h = size * 6 / 10, fx = x + (size - w) / 2, fy = y + (size - h) / 2;
    vx_fill_rounded(s, fx, fy, w, h, size / 10, color, 255);
    lk_triangle(s, fx + w * 4 / 10, fy + h / 2, h / 2, false, vx_mix(color, 0x000000, 150));
}

/* An app's mark: a gel of the accent with a sign (a note, or a film). */
static inline void lk_logo(struct vx_surface *s, int x, int y, int size, bool film) {
    vx_fill_rounded(s, x + 2, y + 4, size, size, size / 4, 0x000000, 50);
    vx_draw_gel(s, x, y, size, size, size / 4, VX_COLOR_ACCENT);
    if (film) {
        lk_film(s, x, y, size, 0xffffff);
    } else {
        lk_note(s, x + size / 6, y + size / 6, size * 2 / 3, 0xffffff);
    }
}

/* Cover art made up from a name: two colors from it, a sheen, the album's
 * first letter or a note. */
static inline void lk_art(struct vx_surface *s, int x, int y, int size, int radius,
                          const char *seed) {
    unsigned h = lk_hash(seed && seed[0] ? seed : "Vexa");
    int hue = (int)(h % 360);
    uint32_t a = lk_hsv(hue, 150, 235), b = lk_hsv(hue + 35 + (int)(h >> 9) % 50, 200, 120);
    vx_fill_rounded(s, x, y, size, size, radius, vx_mix(a, b, 128), 255);
    for (int row = 2; row < size - 2; row++) {
        int inset = 0, coverage;
        if (row < radius) {
            inset = vx_corner_inset(radius, row, &coverage);
        } else if (row >= size - radius) {
            inset = vx_corner_inset(radius, size - 1 - row, &coverage);
        }
        for (int col = inset; col < size - inset; col += 4) {
            int t = (row + col) * 255 / (2 * size);
            vx_fill(s, x + col, y + row, col + 4 <= size - inset ? 4 : size - inset - col, 1,
                    vx_mix(a, b, t));
        }
    }
    /* Rings, faint, off center: a record's grooves. */
    int cx = x + size * 2 / 3, cy = y + size * 2 / 3;
    for (int r = size / 3; r < size; r += size / 9) {
        for (int k = 0; k < 360; k += 2) {
            static const int sin_table[46] = {
                0,   35,  70,  105, 139, 174, 208, 242, 276, 309, 342, 375, 407, 438, 469, 500,
                530, 559, 588, 616, 643, 669, 695, 719, 743, 766, 788, 809, 829, 848, 866, 883,
                899, 914, 927, 940, 951, 961, 970, 978, 985, 990, 995, 998, 999, 1000};
            int deg = k % 360, q = deg / 90, d = deg % 90;
            int sn = q == 0 ? sin_table[d / 2] : q == 1 ? sin_table[(90 - d) / 2]
                     : q == 2 ? -sin_table[d / 2] : -sin_table[(90 - d) / 2];
            int cs = q == 0 ? sin_table[(90 - d) / 2] : q == 1 ? -sin_table[d / 2]
                     : q == 2 ? -sin_table[(90 - d) / 2] : sin_table[d / 2];
            int px = cx + r * cs / 1000, py = cy + r * sn / 1000;
            if (px >= x + 3 && px < x + size - 3 && py >= y + 3 && py < y + size - 3) {
                vx_fill_rounded(s, px, py, 1, 1, 0, 0xffffff, 26);
            }
        }
    }
    vx_fill_rounded(s, x, y, size, size / 2, radius, 0xffffff, 34); /* The sheen. */
    char letter[2] = {0, 0};
    for (const char *p = seed ? seed : ""; *p && !letter[0]; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) {
            letter[0] = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        }
    }
    if (letter[0] && size >= 40) {
        const struct vx_font *f = vx_font(VX_FACE_BOLD, size / 2);
        lk_centered(s, f, x + size / 2 + 1, y + (size - vx_font_height(f)) / 2 + 2, letter,
                    vx_mix(b, 0x000000, 120));
        lk_centered(s, f, x + size / 2, y + (size - vx_font_height(f)) / 2, letter, 0xffffff);
    } else {
        lk_note(s, x + size / 5, y + size / 5, size * 3 / 5, 0xffffff);
    }
}

/* Play (a triangle) or pause (two bars), centered on (cx, cy). */
static inline void lk_play_sign(struct vx_surface *s, int cx, int cy, int size, bool pause,
                                uint32_t color) {
    if (pause) {
        int bw = size / 4 > 2 ? size / 4 : 2;
        vx_fill_rounded(s, cx - bw - bw / 2, cy - size / 2, bw, size, bw / 3, color, 255);
        vx_fill_rounded(s, cx + bw / 2, cy - size / 2, bw, size, bw / 3, color, 255);
    } else {
        lk_triangle(s, cx - size / 3, cy, size, false, color);
    }
}

/* Back or next: a bar and two triangles. */
static inline void lk_skip_sign(struct vx_surface *s, int cx, int cy, int size, bool back,
                                uint32_t color) {
    int h = size, w = h * 7 / 8;
    if (back) {
        vx_fill(s, cx - w - 2, cy - h / 2, 2, h, color);
        lk_triangle(s, cx, cy, h, true, color);
        lk_triangle(s, cx + w, cy, h, true, color);
    } else {
        lk_triangle(s, cx - w, cy, h, false, color);
        lk_triangle(s, cx, cy, h, false, color);
        vx_fill(s, cx + w, cy - h / 2, 2, h, color);
    }
}

/* Shuffle: two crossing paths with arrowheads. */
static inline void lk_shuffle_sign(struct vx_surface *s, int cx, int cy, uint32_t color) {
    lk_line(s, cx - 8, cy - 5, cx + 6, cy + 5, 2, color);
    lk_line(s, cx - 8, cy + 5, cx + 6, cy - 5, 2, color);
    lk_triangle(s, cx + 5, cy - 5, 7, false, color);
    lk_triangle(s, cx + 5, cy + 5, 7, false, color);
}

/* Repeat: a loop with an arrowhead. */
static inline void lk_repeat_sign(struct vx_surface *s, int cx, int cy, uint32_t color) {
    vx_fill(s, cx - 8, cy - 5, 14, 2, color);
    vx_fill(s, cx - 6, cy + 4, 14, 2, color);
    vx_fill(s, cx - 9, cy - 5, 2, 7, color);
    vx_fill(s, cx + 7, cy - 1, 2, 7, color);
    lk_triangle(s, cx + 4, cy - 4, 7, false, color);
    lk_triangle(s, cx - 3, cy + 5, 7, true, color);
}

/* A speaker, with up to three waves for `level` (0 to 100; muted: a cross). */
static inline void lk_speaker_sign(struct vx_surface *s, int x, int cy, int level,
                                   uint32_t color) {
    vx_fill(s, x, cy - 3, 4, 6, color);
    for (int i = 0; i < 5; i++) {
        vx_fill(s, x + 4 + i, cy - 3 - i, 1, 6 + 2 * i, color);
    }
    if (level <= 0) {
        lk_line(s, x + 12, cy - 4, x + 18, cy + 4, 2, color);
        lk_line(s, x + 18, cy - 4, x + 12, cy + 4, 2, color);
        return;
    }
    for (int w = 0; w < 3 && level > w * 33; w++) {
        int r = 4 + w * 3;
        vx_fill(s, x + 10 + w * 3, cy - r + 2, 2, 2 * r - 4, color);
    }
}

/* A gel track with the done part in the accent and a round knob: a
 * scrubber or a slider. `fraction` from 0 to 1. */
static inline void lk_track(struct vx_surface *s, int x, int cy, int w, double fraction,
                            bool hot, bool dark) {
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    int h = hot ? 8 : 6;
    vx_fill_rounded(s, x, cy - h / 2, w, h, h / 2, dark ? 0xffffff : 0x000000, dark ? 60 : 40);
    int done = (int)(w * fraction);
    if (done > 0) {
        vx_draw_gel(s, x, cy - h / 2, done < h ? h : done, h, h / 2, VX_COLOR_ACCENT);
    }
    int r = hot ? 8 : 6;
    lk_circle(s, x + done, cy + 1, r + 1, 0x000000, 60);
    lk_circle(s, x + done, cy, r, 0xffffff, 255);
    lk_circle(s, x + done, cy, r - 3, VX_COLOR_ACCENT, 255);
}

static inline void lk_time(char *out, size_t size, double seconds) {
    int t = seconds < 0 ? 0 : (int)seconds;
    if (t >= 3600) {
        snprintf(out, size, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    } else {
        snprintf(out, size, "%d:%02d", t / 60, t % 60);
    }
}

/* A small rounded label ("MP3"). Returns its width. */
static inline int lk_chip(struct vx_surface *s, int x, int y, const char *text, uint32_t color,
                          uint32_t ink) {
    const struct vx_font *f = vx_font(VX_FACE_BOLD, 11);
    int w = vx_text_width_font(f, text) + 14;
    vx_fill_rounded(s, x, y, w, 18, 9, color, 255);
    vx_text(s, f, x + 7, y + (18 - vx_font_height(f)) / 2, text, ink, VX_TRANSPARENT);
    return w;
}

#endif
