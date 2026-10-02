/* The desktop's look: shadows, round corners, smooth scaling, blur, and the
 * pointer's shapes. */
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
    /* A little below the window, and a little inside it at the sides. */
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
        uint32_t *row = view->pixels + (long)y * view->stride;
        for (int x = area.x; x < area.x + area.width; x++) {
            int dx = x < r.x ? r.x - x : x >= r.x + r.width ? x - (r.x + r.width - 1) : 0;
            if (dx > SHADOW || (dx == 0 && dy == 0)) {
                if (dx == 0 && dy == 0) {
                    x = r.x + r.width - 1; /* Skip what the window covers. */
                }
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

/* How far in from the side row `row` of a corner of `radius` starts, and
 * how much of the pixel there is inside (0-255), for a smooth edge. */
static int corner_inset(int radius, int row, int *coverage) {
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
    if (inset > 0) {
        int dx = 2 * (radius - inset) + 1;
        int over = dx * dx + dy2 - r2; /* How far past the edge. */
        int span = 4 * (radius - inset) + 2;
        *coverage = over >= span ? 0 : 255 - over * 255 / span;
    } else {
        *coverage = 0;
    }
    return inset;
}

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
