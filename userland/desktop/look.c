/* The desktop's look: shadows, round corners, glass and gel, smooth scaling,
 * blur, and the pointer's shapes. */
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

static inline uint32_t mix_pixel(uint32_t dst, uint32_t src, unsigned a) {
    uint32_t rb = dst & 0xff00ff, g = dst & 0x00ff00;
    rb += (((src & 0xff00ff) - rb) * a >> 8) & 0xff00ff;
    g += (((src & 0x00ff00) - g) * a >> 8) & 0x00ff00;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

/* Clips a rectangle to a surface: false if nothing is left. */
static bool clip(const struct vx_surface *s, struct rect *r) {
    if (r->x < 0) {
        r->width += r->x, r->x = 0;
    }
    if (r->y < 0) {
        r->height += r->y, r->y = 0;
    }
    if (r->x + r->width > s->width) {
        r->width = s->width - r->x;
    }
    if (r->y + r->height > s->height) {
        r->height = s->height - r->y;
    }
    return r->width > 0 && r->height > 0;
}

void blend_rect(struct vx_surface *view, struct rect r, uint32_t color, int alpha) {
    if (!clip(view, &r) || alpha <= 0) {
        return;
    }
    unsigned a = alpha >= 255 ? 256 : (unsigned)alpha;
    for (int y = r.y; y < r.y + r.height; y++) {
        uint32_t *p = view->pixels + (long)y * view->stride + r.x;
        for (int x = 0; x < r.width; x++) {
            p[x] = mix_pixel(p[x], color, a);
        }
    }
}

/* ---- Shadows ---- */

static uint8_t falloff[SHADOW + 1]; /* 255 at the edge, 0 SHADOW pixels away. */

void draw_shadow(struct vx_surface *view, struct rect r, int strength) {
    if (!falloff[0]) {
        for (int d = 0; d <= SHADOW; d++) {
            int t = 255 * (SHADOW - d) / SHADOW;
            falloff[d] = (uint8_t)(t * t / 255);
        }
    }
    /* The shadow's shape: a little below the window, and a little inside it
     * at the sides. What the window itself covers isn't drawn. */
    struct rect window = r;
    r.y += 4;
    r.x += 2;
    r.width -= 4;
    struct rect area = {r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW, r.height + 2 * SHADOW};
    if (!clip(view, &area)) {
        return;
    }
    for (int y = area.y; y < area.y + area.height; y++) {
        int dy = y < r.y ? r.y - y : y >= r.y + r.height ? y - (r.y + r.height - 1) : 0;
        if (dy > SHADOW) {
            continue;
        }
        bool beside = y >= window.y && y < window.y + window.height;
        uint32_t *row = view->pixels + (long)y * view->stride;
        for (int x = area.x; x < area.x + area.width; x++) {
            if (beside && x >= window.x && x < window.x + window.width) {
                x = window.x + window.width - 1; /* Under the window: skip it. */
                continue;
            }
            int dx = x < r.x ? r.x - x : x >= r.x + r.width ? x - (r.x + r.width - 1) : 0;
            if (dx > SHADOW) {
                continue;
            }
            unsigned a = (unsigned)falloff[dx] * falloff[dy] / 255 * (unsigned)strength / 255;
            if (a) {
                row[x] = mix_pixel(row[x], 0x000000, a);
            }
        }
    }
}

/* ---- Round corners ---- */

#define corner_inset vx_corner_inset

void fill_rounded(struct vx_surface *view, struct rect r, int radius, uint32_t color) {
    if (radius * 2 > r.height) {
        radius = r.height / 2;
    }
    if (radius * 2 > r.width) {
        radius = r.width / 2;
    }
    for (int row = 0; row < r.height; row++) {
        int from_edge = row < radius ? row : r.height - 1 - row < radius ? r.height - 1 - row : -1;
        int inset = 0, coverage = 0;
        if (from_edge >= 0) {
            inset = corner_inset(radius, from_edge, &coverage);
        }
        vx_fill(view, r.x + inset, r.y + row, r.width - 2 * inset, 1, color);
        if (inset > 0 && coverage > 0) {
            blend_rect(view, (struct rect){r.x + inset - 1, r.y + row, 1, 1}, color, coverage);
            blend_rect(view, (struct rect){r.x + r.width - inset, r.y + row, 1, 1}, color, coverage);
        }
    }
}

void outline_rounded(struct vx_surface *view, struct rect r, int radius, uint32_t color) {
    if (radius * 2 > r.height) {
        radius = r.height / 2;
    }
    for (int row = 0; row < r.height; row++) {
        int from_edge = row < radius ? row : r.height - 1 - row < radius ? r.height - 1 - row : -1;
        int inset = 0, coverage = 0;
        if (from_edge >= 0) {
            inset = corner_inset(radius, from_edge, &coverage);
            int next_coverage, next = from_edge + 1 < radius ? corner_inset(radius, from_edge + 1,
                                                                            &next_coverage)
                                                             : 0;
            int span = inset - next > 1 ? inset - next : 1;
            if (from_edge == 0) { /* The top or bottom line. */
                vx_fill(view, r.x + inset, r.y + row, r.width - 2 * inset, 1, color);
            } else {
                vx_fill(view, r.x + inset - (span - 1), r.y + row, span, 1, color);
                vx_fill(view, r.x + r.width - inset - 1, r.y + row, span, 1, color);
            }
        } else {
            vx_fill(view, r.x, r.y + row, 1, 1, color);
            vx_fill(view, r.x + r.width - 1, r.y + row, 1, 1, color);
        }
    }
}

/* ---- Glass (Aero's) and gel (Aqua's) ---- */

#define GLASS_BLUR 7 /* How far the blur behind glass reaches (two passes). */

static uint32_t *glass_pixels;
static size_t glass_room;

/* Blurs a line of n pixels, `step` apart, in place: each the average of the
 * 2 * radius + 1 around it (the ends repeated). */
static void blur_line(uint32_t *p, int n, long step, int radius, uint32_t *tmp) {
    for (int i = 0; i < n; i++) {
        tmp[i] = p[i * step];
    }
    unsigned red = 0, green = 0, blue = 0, count = 2 * (unsigned)radius + 1;
    for (int k = -radius; k <= radius; k++) {
        uint32_t q = tmp[k < 0 ? 0 : k >= n ? n - 1 : k];
        red += q >> 16 & 0xff, green += q >> 8 & 0xff, blue += q & 0xff;
    }
    for (int i = 0; i < n; i++) {
        p[i * step] = (red / count) << 16 | (green / count) << 8 | blue / count;
        int in = i + radius + 1, out = i - radius;
        uint32_t qi = tmp[in >= n ? n - 1 : in], qo = tmp[out < 0 ? 0 : out];
        red += (qi >> 16 & 0xff) - (qo >> 16 & 0xff);
        green += (qi >> 8 & 0xff) - (qo >> 8 & 0xff);
        blue += (qi & 0xff) - (qo & 0xff);
    }
}

void draw_glass(struct vx_surface *view, struct rect r, int top_radius, int bottom_radius,
                uint32_t tint, int tint_alpha, int shine) {
    struct rect c = r;
    if (!clip(view, &c)) {
        return;
    }
    int w = c.width, h = c.height;
    size_t need = (size_t)w * h + (size_t)(w > h ? w : h);
    if (need > glass_room) {
        free(glass_pixels);
        glass_pixels = malloc(need * 4);
        glass_room = glass_pixels ? need : 0;
        if (!glass_pixels) {
            blend_rect(view, c, tint, 255);
            return;
        }
    }
    uint32_t *buf = glass_pixels, *tmp = glass_pixels + (size_t)w * h;
    for (int y = 0; y < h; y++) {
        memcpy(buf + (long)y * w, view->pixels + (long)(c.y + y) * view->stride + c.x,
               (size_t)w * 4);
    }
    /* What's behind, blurred (twice: smoother than once). */
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            blur_line(buf + (long)y * w, w, 1, GLASS_BLUR, tmp);
        }
        for (int x = 0; x < w; x++) {
            blur_line(buf + x, h, w, GLASS_BLUR, tmp);
        }
    }
    /* Tinted, then lit: a shine over the top half, fading down, and a faint
     * glow along the bottom. */
    unsigned ta = tint_alpha >= 255 ? 256 : tint_alpha <= 0 ? 0 : (unsigned)tint_alpha;
    int half = r.height / 2 > 0 ? r.height / 2 : 1;
    for (int y = 0; y < h; y++) {
        int row = c.y + y - r.y;
        int coverage = 0, inset = 0;
        if (row < top_radius) {
            inset = corner_inset(top_radius, row, &coverage);
        } else if (r.height - 1 - row < bottom_radius) {
            inset = corner_inset(bottom_radius, r.height - 1 - row, &coverage);
        }
        int light = row < half ? shine - shine * row * 9 / (half * 20)
                               : shine / 6 * (row - half) / (r.height - half);
        unsigned la = light <= 0 ? 0 : (unsigned)light;
        uint32_t *from = buf + (long)y * w, *to = view->pixels + (long)(c.y + y) * view->stride;
        for (int x = 0; x < w; x++) {
            int col = c.x + x - r.x;
            if (col < inset - 1 || col > r.width - inset) {
                continue; /* Outside a round corner. */
            }
            uint32_t q = mix_pixel(from[x], tint, ta);
            q = la ? mix_pixel(q, 0xffffff, la) : q;
            bool edge = inset > 0 && (col == inset - 1 || col == r.width - inset);
            to[c.x + x] = edge ? mix_pixel(to[c.x + x], q, (unsigned)coverage) : q;
        }
    }
}

void draw_glass_popup(struct vx_surface *view, struct rect r, int radius, int shadow) {
    draw_shadow(view, r, shadow);
    draw_glass(view, r, radius, radius, vx_theme.menu, vx_theme.dark ? 205 : 195, 34);
    outline_rounded(view, r, radius, vx_mix(vx_theme.line, 0x000000, vx_theme.dark ? 60 : 30));
    blend_rect(view, (struct rect){r.x + radius, r.y + 1, r.width - 2 * radius, 1}, 0xffffff,
               vx_theme.dark ? 50 : 160);
}

void draw_orb(struct vx_surface *view, int cx, int cy, int radius, uint32_t color) {
    /* A glossy ball: deeper at the top, lit from inside at the bottom, a
     * darker rim, and a bright highlight over the top. In quarter pixels,
     * for smooth edges. */
    uint32_t top = vx_mix(color, 0x000000, 50), bottom = vx_mix(color, 0xffffff, 70);
    uint32_t rim = vx_mix(color, 0x000000, 130);
    int r4 = radius * 4;
    for (int py = -radius; py < radius; py++) {
        for (int px = -radius; px < radius; px++) {
            int dx = px * 4 + 2, dy = py * 4 + 2; /* The pixel's middle. */
            int d2 = dx * dx + dy * dy;
            if (d2 > (r4 + 2) * (r4 + 2)) {
                continue;
            }
            /* Coverage at the edge: how far inside, over about a pixel. */
            int inside = r4 * r4 - d2; /* > 0 inside */
            int coverage = inside >= 4 * r4 ? 255 : inside <= -4 * r4 ? 0
                                                    : 128 + inside * 127 / (4 * r4);
            if (coverage <= 0) {
                continue;
            }
            int t = (dy + r4) * 255 / (2 * r4);
            uint32_t q = vx_mix(top, bottom, t < 0 ? 0 : t > 255 ? 255 : t);
            int ring = r4 * r4 - (r4 - 5) * (r4 - 5);
            if (inside < ring) { /* The rim. */
                q = vx_mix(q, rim, 170 - (inside > 0 ? inside * 120 / ring : 0));
            }
            /* The highlight: an ellipse over the top half, brightest at its
             * top, its edge soft. (In hundredths of its radii.) */
            int ex = dx * 100 / (r4 * 62 / 100 + 1);
            int ey = (dy + r4 * 45 / 100) * 100 / (r4 * 40 / 100 + 1);
            int e2 = ex * ex + ey * ey;
            if (e2 < 10000) {
                int a = 225 - (ey + 100) * 160 / 200;
                int soft = 10000 - e2 < 3000 ? (10000 - e2) * 255 / 3000 : 255;
                q = vx_mix(q, 0xffffff, a * soft / 255);
            }
            blend_rect(view, (struct rect){cx + px, cy + py, 1, 1}, q, coverage);
        }
    }
}

void fill_gel(struct vx_surface *view, struct rect r, int radius, uint32_t color, int alpha) {
    radius = radius * 2 > r.height ? r.height / 2 : radius;
    for (int row = 0; row < r.height; row++) {
        int from_edge = row < radius ? row : r.height - 1 - row < radius ? r.height - 1 - row : -1;
        int inset = 0, coverage = 0;
        if (from_edge >= 0) {
            inset = corner_inset(radius, from_edge, &coverage);
        }
        uint32_t c = vx_gel_color(color, row, r.height);
        blend_rect(view, (struct rect){r.x + inset, r.y + row, r.width - 2 * inset, 1}, c, alpha);
        if (inset > 0 && coverage > 0) {
            blend_rect(view, (struct rect){r.x + inset - 1, r.y + row, 1, 1}, c, coverage * alpha / 255);
            blend_rect(view, (struct rect){r.x + r.width - inset, r.y + row, 1, 1}, c,
                       coverage * alpha / 255);
        }
    }
}

/* ---- Scaling ---- */

void blit_smooth(struct vx_surface *to, struct rect r, const struct vx_surface *from, int alpha) {
    if (r.width <= 0 || r.height <= 0 || from->width <= 0 || from->height <= 0) {
        return;
    }
    struct rect c = r;
    if (!clip(to, &c)) {
        return;
    }
    unsigned a = alpha >= 255 ? 256 : alpha <= 0 ? 0 : (unsigned)alpha;
    /* Source steps in 16.16 fixed point. */
    long step_x = ((long)from->width << 16) / r.width;
    long step_y = ((long)from->height << 16) / r.height;
    int samples_x = (int)(step_x >> 16), samples_y = (int)(step_y >> 16);
    samples_x = samples_x < 1 ? 1 : samples_x > 4 ? 4 : samples_x;
    samples_y = samples_y < 1 ? 1 : samples_y > 4 ? 4 : samples_y;
    for (int y = c.y; y < c.y + c.height; y++) {
        long sy0 = (long)(y - r.y) * step_y;
        uint32_t *out = to->pixels + (long)y * to->stride;
        for (int x = c.x; x < c.x + c.width; x++) {
            long sx0 = (long)(x - r.x) * step_x;
            unsigned red = 0, green = 0, blue = 0, n = 0;
            for (int j = 0; j < samples_y; j++) {
                int sy = (int)((sy0 + (step_y * j + step_y / 2) / samples_y) >> 16);
                sy = sy >= from->height ? from->height - 1 : sy;
                const uint32_t *row = from->pixels + (long)sy * from->stride;
                for (int i = 0; i < samples_x; i++) {
                    int sx = (int)((sx0 + (step_x * i + step_x / 2) / samples_x) >> 16);
                    uint32_t p = row[sx >= from->width ? from->width - 1 : sx];
                    red += p >> 16 & 0xff;
                    green += p >> 8 & 0xff;
                    blue += p & 0xff;
                    n++;
                }
            }
            uint32_t p = (red / n) << 16 | (green / n) << 8 | blue / n;
            out[x] = a == 256 ? p : mix_pixel(out[x], p, a);
        }
    }
}

/* A box blur of a small surface, in place (rows, then columns). */
static void box_blur(uint32_t *p, int w, int h, int radius) {
    uint32_t *line = malloc((size_t)(w > h ? w : h) * 4);
    if (!line) {
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? h : w, lines = pass ? w : h;
        for (int l = 0; l < lines; l++) {
            for (int i = 0; i < n; i++) {
                unsigned red = 0, green = 0, blue = 0, count = 0;
                for (int k = -radius; k <= radius; k++) {
                    int j = i + k;
                    if (j < 0 || j >= n) {
                        continue;
                    }
                    uint32_t q = pass ? p[(long)j * w + l] : p[(long)l * w + j];
                    red += q >> 16 & 0xff, green += q >> 8 & 0xff, blue += q & 0xff;
                    count++;
                }
                line[i] = (red / count) << 16 | (green / count) << 8 | blue / count;
            }
            for (int i = 0; i < n; i++) {
                if (pass) {
                    p[(long)i * w + l] = line[i];
                } else {
                    p[(long)l * w + i] = line[i];
                }
            }
        }
    }
    free(line);
}

bool make_blurred(struct vx_surface *out, const struct vx_surface *from, int dim) {
    int w = from->width, h = from->height;
    int sw = w / 8 > 0 ? w / 8 : 1, sh = h / 8 > 0 ? h / 8 : 1;
    struct vx_surface small = {malloc((size_t)sw * sh * 4), sw, sh, sw};
    if (!small.pixels) {
        return false;
    }
    blit_smooth(&small, (struct rect){0, 0, sw, sh}, from, 255);
    box_blur(small.pixels, sw, sh, 2);
    box_blur(small.pixels, sw, sh, 2);
    if (!out->pixels || out->width != w || out->height != h) {
        if (out->pixels) {
            vx_unmap(out->pixels, (size_t)out->width * out->height * 4);
        }
        out->pixels = vx_map((size_t)w * h * 4, VX_MAP_WRITE);
        out->width = out->stride = w;
        out->height = h;
        if (!out->pixels) {
            free(small.pixels);
            return false;
        }
    }
    /* Back to full size, bilinear, and darker. */
    for (int y = 0; y < h; y++) {
        long fy = ((long)y * sh << 8) / h - 128;
        fy = fy < 0 ? 0 : fy;
        int y0 = (int)(fy >> 8), y1 = y0 + 1 < sh ? y0 + 1 : y0;
        unsigned ty = (unsigned)(fy & 255);
        for (int x = 0; x < w; x++) {
            long fx = ((long)x * sw << 8) / w - 128;
            fx = fx < 0 ? 0 : fx;
            int x0 = (int)(fx >> 8), x1 = x0 + 1 < sw ? x0 + 1 : x0;
            unsigned tx = (unsigned)(fx & 255);
            uint32_t top = mix_pixel(small.pixels[y0 * sw + x0], small.pixels[y0 * sw + x1], tx);
            uint32_t bottom = mix_pixel(small.pixels[y1 * sw + x0], small.pixels[y1 * sw + x1], tx);
            out->pixels[(long)y * w + x] = mix_pixel(mix_pixel(top, bottom, ty), 0x000000,
                                                     (unsigned)dim);
        }
    }
    free(small.pixels);
    return true;
}

/* ---- The pointer ---- */

#define CURSOR_SIZE 20
/* 0 nothing, 1 white, 2 black. */
static uint8_t shapes[CURSOR_SHAPES][CURSOR_SIZE][CURSOR_SIZE];
static struct {
    int width, height, hot_x, hot_y;
} shape_info[CURSOR_SHAPES];

static const char *const arrow_art[] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
    "#.#  #..#   ", "##    #..#  ", "#     #..#  ", "       ##   ", NULL,
};
static const char *const hand_art[] = {
    "     ##          ", "    #..#         ", "    #..#         ", "    #..#         ",
    "    #..###       ", "    #..#..###    ", "    #..#..#..##  ", " ## #..#..#..#.# ",
    "#..##..........# ", "#...#..........# ", " #.............# ", "  #............# ",
    "  #...........#  ", "   #..........#  ", "    #........#   ", "    #........#   ",
    "    ##########   ", NULL,
};
static const char *const wait_art[] = {
    "###########", "#.........#", " #.......# ", " #.#.#.#.# ", "  #.#.#.#  ", "   #.#.#   ",
    "    #.#    ", "     #     ", "    #.#    ", "   #...#   ", "  #..#..#  ", " #..#.#..# ",
    " #.#.#.#.# ", "#.#.#.#.#.#", "###########", NULL,
};

static void from_art(int shape, const char *const *art, int hot_x, int hot_y) {
    int h = 0, w = 0;
    for (; art[h]; h++) {
        int n = (int)strlen(art[h]);
        w = n > w ? n : w;
        for (int x = 0; x < n && x < CURSOR_SIZE; x++) {
            shapes[shape][h][x] = art[h][x] == '#' ? 2 : art[h][x] == '.' ? 1 : 0;
        }
    }
    shape_info[shape].width = w;
    shape_info[shape].height = h;
    shape_info[shape].hot_x = hot_x;
    shape_info[shape].hot_y = hot_y;
}

/* A shape drawn as black lines, then given a white outline. */
static void black(int shape, int x, int y) {
    if (x >= 0 && y >= 0 && x < 17 && y < 17) {
        shapes[shape][y + 1][x + 1] = 2;
    }
}

static void outline(int shape) {
    uint8_t copy[CURSOR_SIZE][CURSOR_SIZE];
    memcpy(copy, shapes[shape], sizeof(copy));
    for (int y = 0; y < CURSOR_SIZE; y++) {
        for (int x = 0; x < CURSOR_SIZE; x++) {
            if (copy[y][x] == 2) {
                continue;
            }
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && xx >= 0 && yy < CURSOR_SIZE && xx < CURSOR_SIZE &&
                        copy[yy][xx] == 2) {
                        shapes[shape][y][x] = 1;
                    }
                }
            }
        }
    }
    shape_info[shape].width = shape_info[shape].height = 19;
    shape_info[shape].hot_x = shape_info[shape].hot_y = 9;
}

/* A two-headed arrow along (dx, dy) through the middle. */
static void double_arrow(int shape, int dx, int dy) {
    for (int i = -7; i <= 7; i++) {
        black(shape, 8 + i * dx, 8 + i * dy);
    }
    for (int end = -1; end <= 1; end += 2) {
        for (int k = 0; k < 4; k++) {
            for (int s = -k; s <= k; s++) {
                int along = end * (7 - k);
                if (dx && dy) { /* Diagonal heads: a corner. */
                    black(shape, 8 + along * dx + (s < 0 ? s * dx : 0),
                          8 + along * dy + (s > 0 ? -s * dy : 0));
                } else {
                    black(shape, 8 + along * dx + s * dy, 8 + along * dy + s * dx);
                }
            }
        }
    }
}

static void make_shapes(void) {
    from_art(VX_CURSOR_ARROW, arrow_art, 0, 0);
    from_art(VX_CURSOR_HAND, hand_art, 5, 0);
    from_art(VX_CURSOR_WAIT, wait_art, 5, 7);
    /* The text cursor: an I. */
    for (int y = 2; y <= 14; y++) {
        black(VX_CURSOR_TEXT, 8, y);
    }
    for (int x = 6; x <= 10; x++) {
        if (x != 8) {
            black(VX_CURSOR_TEXT, x, 1);
            black(VX_CURSOR_TEXT, x, 15);
        }
    }
    outline(VX_CURSOR_TEXT);
    /* A crosshair, with a gap in the middle. */
    for (int i = 0; i <= 16; i++) {
        if (i < 6 || i > 10) {
            black(VX_CURSOR_CROSS, i, 8);
            black(VX_CURSOR_CROSS, 8, i);
        }
    }
    black(VX_CURSOR_CROSS, 8, 8);
    outline(VX_CURSOR_CROSS);
    double_arrow(CURSOR_RESIZE_EW, 1, 0);
    outline(CURSOR_RESIZE_EW);
    double_arrow(CURSOR_RESIZE_NS, 0, 1);
    outline(CURSOR_RESIZE_NS);
    double_arrow(CURSOR_RESIZE_NWSE, 1, 1);
    outline(CURSOR_RESIZE_NWSE);
    double_arrow(CURSOR_RESIZE_NESW, -1, 1);
    outline(CURSOR_RESIZE_NESW);
    double_arrow(VX_CURSOR_MOVE, 1, 0);
    double_arrow(VX_CURSOR_MOVE, 0, 1);
    outline(VX_CURSOR_MOVE);
}

struct rect cursor_rect(int shape, int x, int y) {
    if (!shape_info[0].width) {
        make_shapes();
    }
    shape = shape < 0 || shape >= CURSOR_SHAPES ? 0 : shape;
    return (struct rect){x - shape_info[shape].hot_x, y - shape_info[shape].hot_y,
                         shape_info[shape].width + 1, shape_info[shape].height + 1};
}

void draw_cursor(struct vx_surface *view, int ox, int oy, int shape, int x, int y) {
    struct rect r = cursor_rect(shape, x, y);
    shape = shape < 0 || shape >= CURSOR_SHAPES ? 0 : shape;
    for (int row = 0; row < shape_info[shape].height; row++) {
        for (int col = 0; col < shape_info[shape].width; col++) {
            uint8_t c = shapes[shape][row][col];
            int px = r.x + col + ox, py = r.y + row + oy;
            if (c && px >= 0 && py >= 0 && px < view->width && py < view->height) {
                view->pixels[(long)py * view->stride + px] = c == 2 ? 0x000000 : 0xffffff;
            }
        }
    }
}

void draw_magnifier(struct vx_surface *view, int x, int y, int size, uint32_t color) {
    /* A ring, and a handle to the bottom right. */
    int radius = size * 3 / 8, cx = x + radius + 1, cy = y + radius + 1;
    for (int py = -radius - 1; py <= radius + 1; py++) {
        for (int px = -radius - 1; px <= radius + 1; px++) {
            int d2 = px * px + py * py;
            int outer = radius * radius, inner = (radius - 2) * (radius - 2);
            if (d2 <= outer + radius && d2 >= inner - radius) {
                int a = d2 > outer ? 255 - (d2 - outer) * 255 / (radius + 1)
                        : d2 < inner ? 255 - (inner - d2) * 255 / (radius + 1) : 255;
                blend_rect(view, (struct rect){cx + px, cy + py, 1, 1}, color, a);
            }
        }
    }
    for (int i = 0; i < size / 3; i++) {
        int hx = cx + radius * 7 / 10 + i, hy = cy + radius * 7 / 10 + i;
        vx_fill(view, hx, hy, 2, 2, color);
    }
}
