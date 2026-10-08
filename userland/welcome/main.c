/* welcome: the first-run experience (see docs/USER-GUIDE.md, "Welcome").
 *
 * A setup in steps, like Windows XP's, over the whole screen: the wallpaper, and
 * in its middle a frosted card, with the steps down its left (the ones done ticked, the one you're on lit up, and
 * any done one clickable to go back to), and the step's page beside them, which
 * slides in. The steps: a greeting, the look (theme and accent), the keyboard
 * and time zone, the network (and the computer's name), a short tour (search,
 * the Vexa menu, window keys, screenshots) and a summary. Back and Next move
 * between them. The desktop starts it the first time an account logs in on an
 * installed Vexa (and the Vexa menu has it, for another go). Choices apply as
 * they're made, the same way Settings does it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include <vexa/time.h>
#include <vexa/users.h>

#define CARD_W 920
#define CARD_H 480
#define SIDEBAR 230                 /* The steps, down the left (as in the Installer). */
#define PAGE_W (CARD_W - SIDEBAR)   /* A page's width: what's beside them. */
#define MARGIN 44
#define FOOTER_Y (CARD_H - 60)      /* Back and Next, under a line. */
#define SLIDE_MS 340

/* The window is the whole screen; the card is in its middle (see layout). */
static int win_w, win_h, card_x, card_y;
static bool whole_window;   /* Everything needs drawing and showing (at first, and after a change). */

enum page { P_WELCOME, P_LOOK, P_REGION, P_NETWORK, P_TOUR, P_DONE, PAGE_COUNT };

enum hit_kind {
    H_NEXT, H_BACK, H_SKIP, H_THEME, H_ACCENT, H_LAYOUT, H_ZONE, H_ZONE_FIELD, H_TRY_FIELD,
    H_NAME_FIELD, H_CHECK, H_SETTINGS, H_SOFTWARE, H_FINISH, H_STEP,
};

struct hit {
    int x, y, w, h;
    enum hit_kind kind;
    int index;
};

static struct vx_window *window;
static struct vx_surface backdrop;      /* The wallpaper and the card's shadow. */
static struct vx_surface card_bg;       /* What's behind a page: the frosted card. */
static struct vx_surface pages[2];      /* A page being drawn (two while one slides). */
static uint32_t theme_key;              /* Which theme backdrop and card_bg are for. */

static struct hit hits[96];
static int hit_count;
static int hit_dx;                      /* Added to a hit's place: a page's is beside the steps. */
static int hover = -1;                  /* The hit under the pointer. */
static struct vx_surface *S;            /* What's being drawn on. */
static long page_age;                   /* How long the page being drawn has been there. */

static int page, leaving = -1, slide_dir;
static long page_since, slide_since;

static struct vx_user me;
static char name_field[64], try_field[96], zone_search[48];
static int focus = -1;                  /* The text field with the keyboard, or -1. */
static int zone_top;
static int pointer_x, pointer_y;
static bool name_changed;

/* The network page: the internet check runs in a thread of its own. */
enum { CHECK_IDLE, CHECK_RUNNING, CHECK_ONLINE, CHECK_OFFLINE };
static volatile int check_state = CHECK_IDLE;
static struct vx_thread *check_thread_handle;

static const char *const layouts[][2] = {
    {"us", "English (US)"}, {"gb", "English (UK)"}, {"de", "German"},
    {"fr", "French"},       {"es", "Spanish"},      {"dvorak", "Dvorak"},
};
#define LAYOUT_COUNT 6

static long now_ms(void) {
    return vx_uptime();
}

/* 0 to 1000, easing out. */
static int ease(long t, long duration) {
    if (t <= 0) {
        return 0;
    }
    if (t >= duration) {
        return 1000;
    }
    long left = 1000 - t * 1000 / duration;
    return (int)(1000 - left * left * left / 1000000);
}

/* How far down item `i` of a page still is while it settles in (pixels). */
static int rise(int i) {
    int t = ease(page_age - i * 70, 420);
    return (1000 - t) * 16 / 1000;
}

static bool shown(int i) {
    return page_age - i * 70 > 0;
}

/* ---- Pictures: the backdrop and the frosted card ---- */

/* `src` made to cover dst (its middle, any size): each pixel an average. */
static void cover(struct vx_surface *dst, const struct vx_surface *src) {
    int dw = dst->width, dh = dst->height, sw = src->width, sh = src->height;
    int fx = 0, fy = 0, fw = sw, fh = sh;
    if ((long)sw * dh > (long)sh * dw) {
        fw = (int)((long)sh * dw / dh);
        fx = (sw - fw) / 2;
    } else {
        fh = (int)((long)sw * dh / dw);
        fy = (sh - fh) / 2;
    }
    for (int y = 0; y < dh; y++) {
        int y0 = fy + y * fh / dh, y1 = fy + (y + 1) * fh / dh;
        y1 = y1 > y0 ? y1 : y0 + 1;
        int sy = (y1 - y0 + 2) / 3;
        for (int x = 0; x < dw; x++) {
            int x0 = fx + x * fw / dw, x1 = fx + (x + 1) * fw / dw;
            x1 = x1 > x0 ? x1 : x0 + 1;
            int sx = (x1 - x0 + 2) / 3;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += sy) {
                const uint32_t *row = src->pixels + (long)yy * src->stride;
                for (int xx = x0; xx < x1; xx += sx) {
                    uint32_t c = row[xx];
                    r += (c >> 16) & 0xff, g += (c >> 8) & 0xff, b += c & 0xff, n++;
                }
            }
            dst->pixels[(long)y * dst->stride + x] = (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

static struct vx_surface make_surface(int w, int h) {
    return (struct vx_surface){malloc((size_t)w * h * 4), w, h, w};
}

/* A box blur of `radius` pixels, three times (about a Gaussian). */
static void blur(struct vx_surface *s, int radius) {
    int w = s->width, h = s->height;
    uint32_t *line = malloc((size_t)(w > h ? w : h) * 4);
    if (!line) {
        return;
    }
    for (int pass = 0; pass < 3; pass++) {
        for (int axis = 0; axis < 2; axis++) {
            int count = axis ? w : h, length = axis ? h : w;
            for (int i = 0; i < count; i++) {
                uint32_t *base = axis ? s->pixels + i : s->pixels + (long)i * s->stride;
                long step = axis ? s->stride : 1;
                long r = 0, g = 0, b = 0;
                int n = 0;
                for (int k = -radius; k <= radius; k++) {
                    int j = k < 0 ? 0 : k >= length ? length - 1 : k;
                    uint32_t c = base[j * step];
                    r += (c >> 16) & 0xff, g += (c >> 8) & 0xff, b += c & 0xff, n++;
                }
                for (int j = 0; j < length; j++) {
                    line[j] = (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
                    int out = j - radius < 0 ? 0 : j - radius;
                    int in = j + radius + 1 >= length ? length - 1 : j + radius + 1;
                    uint32_t co = base[out * step], ci = base[in * step];
                    r += (long)((ci >> 16) & 0xff) - (long)((co >> 16) & 0xff);
                    g += (long)((ci >> 8) & 0xff) - (long)((co >> 8) & 0xff);
                    b += (long)(ci & 0xff) - (long)(co & 0xff);
                }
                for (int j = 0; j < length; j++) {
                    base[j * step] = line[j];
                }
            }
        }
    }
    free(line);
}

/* Smooth enlarging (bilinear). */
static void enlarge(struct vx_surface *dst, const struct vx_surface *src) {
    for (int y = 0; y < dst->height; y++) {
        int fy = y * 256 * (src->height - 1) / (dst->height > 1 ? dst->height - 1 : 1);
        int y0 = fy >> 8, y1 = y0 + 1 < src->height ? y0 + 1 : y0, wy = fy & 255;
        for (int x = 0; x < dst->width; x++) {
            int fx = x * 256 * (src->width - 1) / (dst->width > 1 ? dst->width - 1 : 1);
            int x0 = fx >> 8, x1 = x0 + 1 < src->width ? x0 + 1 : x0, wx = fx & 255;
            uint32_t a = src->pixels[(long)y0 * src->stride + x0], b = src->pixels[(long)y0 * src->stride + x1];
            uint32_t c = src->pixels[(long)y1 * src->stride + x0], d = src->pixels[(long)y1 * src->stride + x1];
            uint32_t top = vx_mix(a, b, wx), bottom = vx_mix(c, d, wx);
            dst->pixels[(long)y * dst->stride + x] = vx_mix(top, bottom, wy);
        }
    }
}

/* The corners of (x, y, w, h) of `s` made round (radius r), showing `from`'s
 * pixels at them, smooth. */
static void round_corners(struct vx_surface *s, const struct vx_surface *from, int x, int y, int w,
                          int h, int r) {
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int cx = r - dx, cy = r - dy, out = 0;
            for (int sub = 0; sub < 4; sub++) {
                int sx = cx * 4 - (sub & 1) * 2 - 1, sy = cy * 4 - (sub >> 1) * 2 - 1;
                out += sx * sx + sy * sy > r * r * 16;
            }
            if (!out) {
                continue;
            }
            int px[4] = {x + dx, x + w - 1 - dx, x + dx, x + w - 1 - dx};
            int py[4] = {y + dy, y + dy, y + h - 1 - dy, y + h - 1 - dy};
            for (int c = 0; c < 4; c++) {
                if (px[c] < 0 || py[c] < 0 || px[c] >= s->width || py[c] >= s->height) {
                    continue;
                }
                long at = (long)py[c] * s->stride + px[c];
                s->pixels[at] = vx_mix(s->pixels[at], from->pixels[(long)py[c] * from->stride + px[c]],
                                       out * 255 / 4);
            }
        }
    }
}

/* The backdrop (the wallpaper in the theme's colors, with the card's shadow)
 * and the card's frosted glass (that part of it, blurred, in the window's color). */
static void make_scenery(void) {
    char path[96];
    vx_theme_wallpaper(&vx_theme, path, sizeof(path));
    struct vx_image *image = vx_image_load(path, 0);
    if (!backdrop.pixels || backdrop.width != win_w || backdrop.height != win_h) {
        free(backdrop.pixels);
        backdrop = make_surface(win_w, win_h);
    }
    if (!card_bg.pixels) {
        card_bg = make_surface(CARD_W, CARD_H);
        pages[0] = make_surface(PAGE_W, CARD_H);
        pages[1] = make_surface(PAGE_W, CARD_H);
    }
    whole_window = true;
    if (image) {
        cover(&backdrop, &image->surface);
        vx_image_free(image);
    } else {
        for (int y = 0; y < win_h; y++) {
            vx_fill(&backdrop, 0, y, win_w, 1, vx_mix(vx_mix(vx_theme.accent, 0, 70),
                                                   vx_mix(vx_theme.accent, 0, 200), y * 255 / (win_h - 1)));
        }
    }
    /* The frosted glass: the part behind the card, smaller, blurred, enlarged,
     * and mostly the window's color. */
    struct vx_surface small = make_surface(CARD_W / 4, CARD_H / 4);
    for (int y = 0; y < small.height; y++) {
        for (int x = 0; x < small.width; x++) {
            small.pixels[y * small.stride + x] =
                backdrop.pixels[(long)(card_y + y * 4 + 2) * backdrop.stride + card_x + x * 4 + 2];
        }
    }
    blur(&small, 4);
    enlarge(&card_bg, &small);
    free(small.pixels);
    for (int y = 0; y < CARD_H; y++) {
        for (int x = 0; x < CARD_W; x++) {
            uint32_t *p = &card_bg.pixels[(long)y * card_bg.stride + x];
            *p = vx_mix(*p, vx_theme.window, 224);
        }
    }
    /* The shadow, under the card (a few rounded rectangles, fainter outwards). */
    for (int i = 14; i >= 1; i--) {
        vx_fill_rounded(&backdrop, card_x - i, card_y - i + 8, CARD_W + 2 * i, CARD_H + 2 * i, 22 + i,
                        0x000000, 5);
    }
    theme_key = vx_theme.accent ^ (vx_theme.dark ? 0xa5a5a5a5u : 0);
}

/* The window's size (the screen's), and the card in its middle. */
static void layout(void) {
    win_w = window->surface.width;
    win_h = window->surface.height;
    card_x = win_w > CARD_W ? (win_w - CARD_W) / 2 : 0;
    card_y = win_h > CARD_H ? (win_h - CARD_H) / 2 : 0;
    whole_window = true;
}

/* ---- Drawing ---- */

static const struct vx_font *bold(int size) {
    return vx_font(VX_FACE_BOLD, size);
}

static const struct vx_font *sans(int size) {
    return vx_font(VX_FACE_SANS, size);
}

static int text_at(int x, int y, const struct vx_font *font, const char *text, uint32_t color) {
    return vx_text(S, font, x, y, text, color, VX_TRANSPARENT);
}

static void centered(int cx, int y, const struct vx_font *font, const char *text, uint32_t color) {
    text_at(cx - vx_text_width_font(font, text) / 2, y, font, text, color);
}

/* A paragraph, wrapped, in lines of `line` pixels; returns the y below it. */
static int paragraph(int x, int y, int width, int line, const struct vx_font *font, const char *text,
                     uint32_t color, bool center) {
    char lines[8][160];
    int n = vx_text_wrap(lines, 8, text, width, font);
    for (int i = 0; i < n; i++) {
        if (center) {
            centered(x + width / 2, y, font, lines[i], color);
        } else {
            text_at(x, y, font, lines[i], color);
        }
        y += line;
    }
    return y;
}

static struct hit *add_hit(int x, int y, int w, int h, enum hit_kind kind, int index) {
    static struct hit spare;
    if (hit_count == (int)(sizeof(hits) / sizeof(hits[0]))) {
        return &spare;
    }
    hits[hit_count] = (struct hit){x + hit_dx, y, w, h, kind, index};
    return &hits[hit_count++];
}

static bool hot(const struct hit *h) {
    return hover >= 0 && hover < hit_count && hits[hover].kind == h->kind &&
           hits[hover].index == h->index;
}

/* A button; `primary` ones are the accent. */
static void button(int x, int y, int w, int h, const char *label, bool primary, enum hit_kind kind,
                   int index) {
    struct hit *hit = add_hit(x, y, w, h, kind, index);
    unsigned flags = primary || hot(hit) ? VX_BUTTON_HOT : 0;
    vx_draw_button_flags(S, x, y, w, h, label, flags);
}

static uint32_t card_color(void) {
    return vx_theme.dark ? vx_mix(vx_theme.window, 0xffffff, 14) : 0xffffff;
}

/* A rounded panel inside a page. */
static void panel(int x, int y, int w, int h) {
    uint32_t edge = vx_theme.dark ? vx_mix(vx_theme.window, 0xffffff, 30) : 0xd5d9e1;
    vx_fill_rounded(S, x, y, w, h, 10, edge, 255);
    vx_fill_rounded(S, x + 1, y + 1, w - 2, h - 2, 9, card_color(), 255);
}

/* A tick: two strokes. */
static void tick(int x, int y, int size, uint32_t color) {
    int t = size / 6 > 1 ? size / 6 : 2;
    for (int i = 0; i <= size * 3 / 8; i++) {
        vx_fill(S, x + i, y + size / 2 + i - t / 2, t, t, color);
    }
    for (int i = 0; i <= size * 5 / 8; i++) {
        vx_fill(S, x + size * 3 / 8 + i, y + size / 2 + size * 3 / 8 - i - t / 2, t, t, color);
    }
}

/* An icon from Vexa's set (/share/icons), kept once loaded. */
static void icon(const char *name, int x, int y, int size) {
    static struct {
        char name[24];
        struct vx_image *image;
    } cache[24];
    for (int i = 0; i < 24; i++) {
        if (!cache[i].name[0]) {
            snprintf(cache[i].name, sizeof(cache[i].name), "%s", name);
            char path[64];
            snprintf(path, sizeof(path), "/share/icons/%s.png", name);
            cache[i].image = vx_image_load(path, VX_IMAGE_ALPHA);
        }
        if (!strcmp(cache[i].name, name)) {
            if (cache[i].image) {
                vx_blit_alpha(S, x, y, size, size, &cache[i].image->surface);
            }
            return;
        }
    }
}

/* A key on a keyboard; returns its width. */
static int keycap(int x, int y, const char *label) {
    int w = vx_text_width_font(sans(12), label) + 18;
    w = w < 30 ? 30 : w;
    uint32_t face = vx_theme.dark ? 0x4a4a4e : 0xffffff;
    uint32_t edge = vx_theme.dark ? 0x232325 : 0xaab0bb;
    vx_fill_rounded(S, x, y + 2, w, 24, 6, edge, 255);
    vx_fill_rounded(S, x, y, w, 24, 6, vx_mix(edge, face, 120), 255);
    vx_fill_rounded(S, x + 1, y + 1, w - 2, 21, 5, face, 255);
    centered(x + w / 2, y + 3, sans(12), label, VX_COLOR_TEXT);
    return w;
}

/* A row of keys: "Ctrl+Space" as two (a plain "+" between). */
static int keys(int x, int y, const char *combo) {
    char part[24];
    int start = x;
    for (const char *p = combo; *p;) {
        size_t n = strcspn(p, "+");
        snprintf(part, sizeof(part), "%.*s", (int)n, p);
        x += keycap(x, y, part) + 6;
        p += n;
        if (*p == '+') {
            p++;
            if (*p) {
                text_at(x - 1, y + 4, sans(13), "+", VX_COLOR_DIM);
                x += 14;
            }
        }
    }
    return x - start;
}

static void header(const char *title, const char *about) {
    text_at(MARGIN, 36 + rise(0), bold(27), title, VX_COLOR_TEXT);
    vx_draw_text_fit(S, MARGIN, 76 + rise(1), PAGE_W - 2 * MARGIN, about, VX_COLOR_DIM, VX_TRANSPARENT);
}

static void footer(bool back, const char *next) {
    vx_fill(S, MARGIN, FOOTER_Y - 8, PAGE_W - 2 * MARGIN, 1, VX_COLOR_LINE);
    if (back) {
        button(MARGIN, FOOTER_Y + 10, 96, 34, "Back", false, H_BACK, 0);
    }
    button(PAGE_W - MARGIN - 150, FOOTER_Y + 10, 150, 34, next, true, H_NEXT, 0);
}

/* ---- The settings choices make ---- */

static struct vx_settings desk; /* Read again after each change (and not for every look at it). */

static void reload_settings(void) {
    vx_settings_load(&desk, "desktop.conf");
}

static void apply(const char *key, const char *value) {
    reload_settings();
    vx_settings_set(&desk, key, value);
    vx_settings_save(&desk);
    vx_desktop_reload();
    printf("welcome: %s=%s\n", key, value);
    fflush(stdout);
}

static const char *setting(const char *key, const char *fallback) {
    return vx_settings_get(&desk, key, fallback);
}

/* ---- The pages ---- */

/* A little desktop in a theme: a sky of the accent, a panel, a window. */
static void mini_desktop(int x, int y, int w, int h, bool dark, uint32_t accent) {
    struct vx_theme t;
    const char *accent_name = setting("accent", VX_ACCENT_DEFAULT);
    vx_theme_make(&t, dark ? "dark" : "light", accent_name);
    uint32_t sky = vx_mix(accent, 0, dark ? 150 : 70), deep = vx_mix(accent, 0, dark ? 230 : 200);
    for (int row = 0; row < h; row++) {
        vx_fill(S, x, y + row, w, 1, vx_mix(sky, deep, row * 255 / (h - 1)));
    }
    vx_fill(S, x, y, w, 8, vx_mix(t.panel, sky, 60));
    vx_fill_rounded(S, x + 3, y + 2, 14, 5, 2, t.accent, 255);
    int wx = x + 20, wy = y + 20, ww = w - 40, wh = h - 30;
    vx_fill_rounded(S, wx - 1, wy + 1, ww + 2, wh + 3, 6, 0x000000, 70);
    vx_fill_rounded(S, wx, wy, ww, wh, 5, t.window, 255);
    vx_fill_rounded(S, wx, wy, ww, 12, 5, t.title_focused, 255);
    vx_fill(S, wx, wy + 7, ww, 5, t.title_focused);
    static const uint32_t balls[3] = {0xfebc2e, 0x2ac845, 0xff5f57};
    for (int i = 0; i < 3; i++) {
        vx_draw_gel(S, wx + ww - 27 + i * 9, wy + 3, 7, 7, 3, balls[i]);
    }
    vx_fill(S, wx, wy + 12, 28, wh - 12, t.sidebar);
    vx_fill_rounded(S, wx + 3, wy + 16, 22, 6, 3, t.accent, 255);
    vx_fill(S, wx + 36, wy + 18, 50, 5, t.text);
    vx_fill(S, wx + 36, wy + 28, ww - 48, 3, t.dim);
    vx_fill(S, wx + 36, wy + 35, ww - 60, 3, t.dim);
    vx_draw_gel(S, wx + ww - 36, wy + wh - 14, 28, 9, 4, t.accent);
}

static void page_welcome(void) {
    int cx = PAGE_W / 2;
    /* The mark, floating a little. */
    int bob = (int)((now_ms() / 40) % 64);
    bob = (bob < 32 ? bob : 64 - bob) / 8 - 2;
    int top = 44 + bob + rise(0);
    vx_fill_rounded(S, cx - 46, top + 8, 96, 96, 26, 0x000000, 40);
    vx_fill_rounded(S, cx - 48, top + 4, 96, 96, 26, 0x000000, 30);
    vx_draw_gel(S, cx - 48, top, 96, 96, 26, VX_COLOR_ACCENT);
    centered(cx, top + 14, bold(62), "V", 0xffffff);
    if (shown(1)) {
        centered(cx, 168 + rise(1), bold(36), "Welcome to Vexa", VX_COLOR_TEXT);
    }
    if (shown(2)) {
        char hi[120];
        snprintf(hi, sizeof(hi), "Hello, %s.", me.full_name[0] ? me.full_name : me.name);
        centered(cx, 222 + rise(2), bold(19), hi, VX_COLOR_ACCENT);
    }
    if (shown(3)) {
        paragraph(PAGE_W / 2 - 250, 258 + rise(3), 500, 22, sans(15),
                  "Let's make this computer yours. It takes about a minute, and you can change "
                  "everything later in Settings.",
                  VX_COLOR_DIM, true);
    }
    if (shown(4)) {
        static const struct {
            const char *icon, *label;
        } ahead[] = {{"palette", "Your look"}, {"settings", "Keyboard and time"},
                     {"network", "The network"}, {"help", "A quick tour"}};
        for (int i = 0; i < 4; i++) {
            int x = cx - 2 * 130 + i * 130 + 65;
            icon(ahead[i].icon, x - 20, 332 + rise(4), 40);
            centered(x, 376 + rise(4), sans(12), ahead[i].label, VX_COLOR_DIM);
        }
    }
    button(cx - 90, 404 + rise(5), 180, 40, "Get Started", true, H_NEXT, 0);
    struct hit *skip = add_hit(cx - 60, 452, 120, 20, H_SKIP, 0);
    centered(cx, 452, sans(12), "Skip for now", hot(skip) ? VX_COLOR_TEXT : VX_COLOR_DIM);
}

static void page_look(void) {
    header("Make it yours", "Pick a look. Everything follows at once, this window too.");
    const char *theme = setting("theme", "light"), *accent = setting("accent", VX_ACCENT_DEFAULT);
    static const char *const values[] = {"light", "dark", "auto"};
    static const char *const titles[] = {"Light", "Dark", "Automatic"};
    uint32_t accent_color = vx_theme.accent;
    int pw = 188, ph = 118, gap = (PAGE_W - 2 * MARGIN - 3 * pw) / 2;
    for (int i = 0; i < 3; i++) {
        int x = MARGIN + i * (pw + gap), y = 122 + rise(2);
        bool on = !strcmp(theme, values[i]);
        struct hit *hit = add_hit(x - 4, y - 4, pw + 8, ph + 44, H_THEME, i);
        if (on) {
            vx_fill_rounded(S, x - 4, y - 4, pw + 8, ph + 8, 12, VX_COLOR_ACCENT, 255);
            vx_fill_rounded(S, x - 2, y - 2, pw + 4, ph + 4, 10, card_color(), 255);
        } else if (hot(hit)) {
            vx_fill_rounded(S, x - 3, y - 3, pw + 6, ph + 6, 11, vx_mix(card_color(), VX_COLOR_ACCENT, 120), 255);
            vx_fill_rounded(S, x - 2, y - 2, pw + 4, ph + 4, 10, card_color(), 255);
        }
        if (i == 2) { /* Light on the left, dark on the right, cut aslant. */
            mini_desktop(x, y, pw, ph, false, accent_color);
            struct vx_surface right = make_surface(pw, ph);
            struct vx_surface *keep = S;
            S = &right;
            mini_desktop(0, 0, pw, ph, true, accent_color);
            S = keep;
            for (int row = 0; row < ph; row++) {
                int cut = pw / 2 + (ph / 2 - row) * 2 / 5;
                memcpy(S->pixels + (long)(y + row) * S->stride + x + cut,
                       right.pixels + (long)row * right.stride + cut, (size_t)(pw - cut) * 4);
            }
            free(right.pixels);
        } else {
            mini_desktop(x, y, pw, ph, i == 1, accent_color);
        }
        round_corners(S, &card_bg, x, y, pw, ph, 8); /* (Close enough at its edge.) */
        if (on) {
            vx_fill_rounded(S, x + pw - 26, y + 6, 22, 22, 11, 0xffffff, 255);
            vx_draw_gel(S, x + pw - 24, y + 8, 18, 18, 9, VX_COLOR_ACCENT);
            tick(x + pw - 20, y + 11, 11, 0xffffff);
        }
        centered(x + pw / 2, y + ph + 10, on ? bold(14) : sans(14), titles[i], VX_COLOR_TEXT);
    }
    /* The accent colors. */
    text_at(MARGIN, 298 + rise(3), bold(15), "Accent color", VX_COLOR_TEXT);
    int step = (PAGE_W - 2 * MARGIN) / vx_accent_count;
    for (int i = 0; i < vx_accent_count; i++) {
        int cx = MARGIN + i * step + step / 2, cy = 350 + rise(4);
        bool on = !strcmp(accent, vx_accents[i].name);
        struct hit *hit = add_hit(cx - step / 2, cy - 26, step, 70, H_ACCENT, i);
        if (on) {
            vx_fill_rounded(S, cx - 24, cy - 24, 48, 48, 24, vx_accents[i].color, 255);
            vx_fill_rounded(S, cx - 22, cy - 22, 44, 44, 22, card_color(), 255);
        }
        int r = on || hot(hit) ? 18 : 16;
        vx_draw_gel(S, cx - r, cy - r, 2 * r, 2 * r, r, vx_accents[i].color);
        if (on) {
            tick(cx - 8, cy - 9, 16, 0xffffff);
        }
        centered(cx, cy + 28, sans(12), vx_accents[i].label, on ? VX_COLOR_TEXT : VX_COLOR_DIM);
    }
    footer(true, "Next");
}

/* The time zones that match what's typed. */
static int matching_zones(int *out, int max) {
    int n = 0;
    for (int i = 0; i < vx_zone_count && n < max; i++) {
        char hay[80];
        snprintf(hay, sizeof(hay), "%s %s", vx_zones[i].city, vx_zones[i].region);
        bool ok = true;
        for (const char *p = zone_search; *p && ok;) {
            /* A word of what's typed has to be in it (any case). */
            size_t len = strcspn(p, " ");
            bool found = len == 0;
            for (const char *q = hay; len && *q && !found; q++) {
                size_t k = 0;
                while (k < len && q[k] && (q[k] | 0x20) == (p[k] | 0x20)) {
                    k++;
                }
                found = k == len;
            }
            ok = found;
            p += len + (p[len] == ' ');
        }
        if (ok) {
            out[n++] = i;
        }
    }
    return n;
}

static void page_region(void) {
    header("Keyboard and time", "So the keys type what they say, and the clock is right.");
    /* The layouts. */
    int lx = MARGIN, ly = 120 + rise(2);
    text_at(lx, ly, bold(15), "Keyboard layout", VX_COLOR_TEXT);
    const char *layout = setting("keyboard_layout", "us");
    for (int i = 0; i < LAYOUT_COUNT; i++) {
        int y = ly + 28 + i * 31 + rise(3);
        bool on = !strcmp(layout, layouts[i][0]);
        struct hit *hit = add_hit(lx - 4, y - 3, 270, 30, H_LAYOUT, i);
        if (on) {
            vx_draw_gel(S, lx - 4, y - 3, 270, 28, 8, VX_COLOR_ACCENT);
        } else if (hot(hit)) {
            vx_fill_rounded(S, lx - 4, y - 3, 270, 28, 8, vx_mix(card_color(), VX_COLOR_ACCENT, 50), 255);
        }
        text_at(lx + 8, y + 2, sans(14), layouts[i][1], on ? 0xffffff : VX_COLOR_TEXT);
    }
    /* Try the keys. */
    int ty = ly + 28 + 6 * 31 + 8;
    text_at(lx, ty, sans(12), "Try it:", VX_COLOR_DIM);
    vx_draw_field(S, lx + 44, ty - 4, 222, try_field, focus == H_TRY_FIELD);
    add_hit(lx + 44, ty - 4, 222, 24, H_TRY_FIELD, 0);
    /* The time zones. */
    int zx = 340, zw = PAGE_W - MARGIN - zx;
    text_at(zx, ly, bold(15), "Time zone", VX_COLOR_TEXT);
    vx_draw_field(S, zx, ly + 24, zw, zone_search[0] || focus == H_ZONE_FIELD ? zone_search : "", focus == H_ZONE_FIELD);
    if (!zone_search[0] && focus != H_ZONE_FIELD) {
        text_at(zx + 8, ly + 28, sans(13), "Search for a city", VX_COLOR_DIM);
    }
    add_hit(zx, ly + 24, zw, 24, H_ZONE_FIELD, 0);
    int found[400];
    int n = matching_zones(found, 400);
    const char *zone = setting("time_zone", "");
    int rows = 6, list_y = ly + 56 + rise(3), row_h = 28;
    panel(zx, list_y, zw, rows * row_h + 8);
    if (zone_top > n - rows) {
        zone_top = n - rows;
    }
    zone_top = zone_top < 0 ? 0 : zone_top;
    for (int r = 0; r < rows && zone_top + r < n; r++) {
        int i = found[zone_top + r];
        int y = list_y + 4 + r * row_h;
        bool on = !strcmp(zone, vx_zones[i].city);
        struct hit *hit = add_hit(zx + 4, y, zw - 8 - 10, row_h, H_ZONE, i);
        if (on) {
            vx_draw_gel(S, zx + 4, y, zw - 18, row_h - 2, 7, VX_COLOR_ACCENT);
        } else if (hot(hit)) {
            vx_fill_rounded(S, zx + 4, y, zw - 18, row_h - 2, 7, vx_mix(card_color(), VX_COLOR_ACCENT, 50), 255);
        }
        char line[80], offset[24];
        int minutes = vx_zones[i].offset;
        snprintf(offset, sizeof(offset), "UTC%+d:%02d", minutes / 60, (minutes < 0 ? -minutes : minutes) % 60);
        snprintf(line, sizeof(line), "%s", vx_zones[i].city);
        text_at(zx + 12, y + 4, sans(14), line, on ? 0xffffff : VX_COLOR_TEXT);
        int w = vx_text_width_font(sans(12), offset);
        text_at(zx + zw - 22 - w, y + 5, sans(12), offset, on ? 0xe8f4f2 : VX_COLOR_DIM);
    }
    if (!n) {
        text_at(zx + 12, list_y + 12, sans(13), "No city matches", VX_COLOR_DIM);
    }
    if (n > rows) { /* A scroll bar. */
        int bar = (rows * row_h) * rows / n;
        bar = bar < 24 ? 24 : bar;
        int at = list_y + 4 + (rows * row_h - bar) * zone_top / (n - rows);
        vx_fill_rounded(S, zx + zw - 8, at, 4, bar, 2, VX_COLOR_DIM, 255);
    }
    /* What time it is there. */
    struct vx_date date;
    vx_local_now(&date);
    char clock[96];
    snprintf(clock, sizeof(clock), "It's %02d:%02d on %s %d%s.", date.hour, date.minute,
             vx_month_names[date.month - 1], date.day, zone[0] ? "" : " (set a city)");
    text_at(zx, list_y + rows * row_h + 18, sans(13), clock, VX_COLOR_DIM);
    footer(true, "Next");
}

/* ---- The network page ---- */

static void *check_internet(void *arg) {
    (void)arg;
    uint32_t address;
    int result = CHECK_OFFLINE;
    if (vx_resolve("example.com", &address) == 0) {
        int handle = vx_connect_to("example.com", 80);
        if (handle >= 0) {
            vx_close(handle);
            result = CHECK_ONLINE;
        }
    }
    __atomic_store_n(&check_state, result, __ATOMIC_SEQ_CST);
    return NULL;
}

static void start_check(void) {
    if (check_state == CHECK_RUNNING) {
        return;
    }
    if (check_thread_handle) {
        vx_thread_join(check_thread_handle);
        check_thread_handle = NULL;
    }
    check_state = CHECK_RUNNING;
    check_thread_handle = vx_thread_create(check_internet, NULL);
    if (!check_thread_handle) {
        check_state = CHECK_OFFLINE;
    }
}

/* The wired card that has (or is getting) an address: its line, or NULL. */
static bool wired_card(struct vx_net_interface *out, bool *any) {
    struct vx_net_interface list[8];
    long count = vx_net_info(list, 8);
    *any = false;
    bool found = false;
    for (long i = 0; i < count && i < 8; i++) {
        if (list[i].flags & VX_NET_LOOPBACK) {
            continue;
        }
        *any = true;
        if (!found || list[i].address) {
            *out = list[i];
            found = true;
        }
    }
    return found;
}

static void page_network(void) {
    header("Connect to the network", "Wired networks connect by themselves. Vexa tells you how it went.");
    struct vx_net_interface card;
    bool any;
    wired_card(&card, &any);
    bool connected = any && card.address != 0;
    icon(connected ? "network-cable" : "network", MARGIN, 124 + rise(2), 96);
    int x = MARGIN + 124, y = 126 + rise(2);
    char line[160], ip[16], router[16];
    if (!any) {
        text_at(x, y, bold(18), "No network card found", VX_COLOR_TEXT);
        paragraph(x, y + 32, PAGE_W - x - MARGIN, 21, sans(14),
                  "That's fine: Vexa works without one. If this computer has a card Vexa doesn't "
                  "know yet, Settings, Network and Device Manager will say what it found.",
                  VX_COLOR_DIM, false);
    } else if (!connected) {
        text_at(x, y, bold(18), "Waiting for an address", VX_COLOR_TEXT);
        int dots = (int)(now_ms() / 300 % 4);
        snprintf(line, sizeof(line), "%s is asking the router for one%.*s", card.name, dots, "...");
        text_at(x, y + 32, sans(14), line, VX_COLOR_DIM);
    } else {
        text_at(x, y, bold(18), "Connected", VX_COLOR_TEXT);
        vx_fill_rounded(S, x + 112, y + 3, 18, 18, 9, 0x3fbf6f, 255);
        tick(x + 116, y + 6, 11, 0xffffff);
        snprintf(line, sizeof(line), "Wired (%s). Address %s.", card.name, vx_format_ipv4(card.address, ip));
        text_at(x, y + 32, sans(14), line, VX_COLOR_DIM);
        if (card.gateway) {
            snprintf(line, sizeof(line), "Router %s.", vx_format_ipv4(card.gateway, router));
            text_at(x, y + 54, sans(14), line, VX_COLOR_DIM);
        }
        /* Can it reach the internet? */
        int cy = y + 92;
        int state = check_state;
        if (state == CHECK_IDLE) {
            start_check();
            state = check_state;
        }
        if (state == CHECK_RUNNING) {
            int phase = (int)(now_ms() / 120 % 8);
            for (int i = 0; i < 8; i++) {
                int d = (phase + 8 - i) % 8;
                vx_fill_rounded(S, x + 2 + i * 6, cy + 6, 4, 10, 2, vx_mix(VX_COLOR_ACCENT, card_color(), d * 30), 255);
            }
            text_at(x + 62, cy + 3, sans(14), "Checking the internet", VX_COLOR_DIM);
        } else if (state == CHECK_ONLINE) {
            vx_fill_rounded(S, x, cy, 22, 22, 11, 0x3fbf6f, 255);
            tick(x + 5, cy + 5, 12, 0xffffff);
            text_at(x + 32, cy + 3, bold(14), "Online: Vexa can reach the internet", VX_COLOR_TEXT);
        } else {
            vx_fill_rounded(S, x, cy, 22, 22, 11, 0xf0ad4e, 255);
            centered(x + 11, cy + 2, bold(15), "!", 0xffffff);
            text_at(x + 32, cy + 3, bold(14), "Can't reach the internet", VX_COLOR_TEXT);
            paragraph(x + 32, cy + 28, PAGE_W - x - MARGIN - 32, 20, sans(13),
                      "The router may not be online. Check its cable, or carry on: you can "
                      "connect later.",
                      VX_COLOR_DIM, false);
        }
        if (state != CHECK_RUNNING) {
            button(PAGE_W - MARGIN - 130, y + 88, 130, 30, "Check Again", false, H_CHECK, 0);
        }
    }
    /* The computer's name. */
    int ny = 288 + rise(4);
    panel(MARGIN, ny, PAGE_W - 2 * MARGIN, 82);
    text_at(MARGIN + 18, ny + 14, bold(14), "Name this computer", VX_COLOR_TEXT);
    text_at(MARGIN + 18, ny + 38, sans(12), "Other computers on the network will see it.", VX_COLOR_DIM);
    vx_draw_field(S, PAGE_W - MARGIN - 250, ny + 28, 232, name_field, focus == H_NAME_FIELD);
    add_hit(PAGE_W - MARGIN - 250, ny + 28, 232, 24, H_NAME_FIELD, 0);
    text_at(MARGIN, ny + 96, sans(12), "Wi-Fi isn't supported yet. A wired card connects as soon as the cable is in.",
            VX_COLOR_DIM);
    footer(true, "Next");
}

/* ---- The tour ---- */

static void tip(int col, int row, const char *icon_name, const char *title, const char *combo,
                const char *about, enum hit_kind action, const char *action_label) {
    int tw = (PAGE_W - 2 * MARGIN - 16) / 2, th = 92;
    int x = MARGIN + col * (tw + 16), y = 108 + row * (th + 12) + rise(2 + row * 2 + col);
    panel(x, y, tw, th);
    icon(icon_name, x + 12, y + 12, 44);
    text_at(x + 68, y + 10, bold(14), title, VX_COLOR_TEXT);
    if (combo[0]) {
        keys(x + 68, y + 31, combo);
    }
    paragraph(x + 68, y + (combo[0] ? 60 : 34), tw - 80, 15, sans(12), about, VX_COLOR_DIM, false);
    if (action_label) {
        button(x + tw - 94, y + 10, 82, 26, action_label, false, action, 0);
    }
}

static void page_tour(void) {
    header("Find your way around", "A few things worth knowing. None of it is hidden for long.");
    tip(0, 0, "search", "Search anything", "Ctrl+Space", "Apps, settings and files, even sums.", H_NEXT, NULL);
    tip(1, 0, "computer", "The Vexa menu", "", "Top left: your apps by type, Lock, Log Out and Shut Down.", H_NEXT, NULL);
    tip(0, 1, "terminal", "Windows", "Alt+Tab", "Super and the arrows snap a window. Alt+F4 closes it.", H_NEXT, NULL);
    tip(1, 1, "control-panel", "Settings", "", "Look, wallpaper, sound, network, accounts: it's all there.", H_SETTINGS, "Open");
    tip(0, 2, "pictures", "Screenshots", "PrtSc", "They land in Pictures. Alt: a window. Shift: an area.", H_NEXT, NULL);
    tip(1, 2, "shield", "Lock and layouts", "Super+L", "Alt+Shift switches the keyboard layout.", H_NEXT, NULL);
    footer(true, "Next");
}

/* ---- All set ---- */

static void page_done(void) {
    int cx = PAGE_W / 2;
    /* A ring that fills in, with a tick. */
    int t = ease(page_age, 700);
    int top = 40 + rise(0);
    vx_fill_rounded(S, cx - 46, top + 6, 92, 92, 46, vx_mix(card_color(), 0x3fbf6f, 60), 255);
    if (t > 120) {
        vx_draw_gel(S, cx - 40, top + 12, 80, 80, 40, 0x3fbf6f);
    }
    if (t > 500) {
        tick(cx - 22, top + 30, 44, 0xffffff);
    }
    centered(cx, 160 + rise(1), bold(32), "You're all set", VX_COLOR_TEXT);
    /* What was chosen. */
    char line[160];
    struct {
        const char *icon, *label;
        char value[96];
    } rows[4];
    const char *theme = setting("theme", "light");
    snprintf(rows[0].value, sizeof(rows[0].value), "%s, %s",
             !strcmp(theme, "dark") ? "Dark" : !strcmp(theme, "auto") ? "Automatic" : "Light",
             setting("accent", VX_ACCENT_DEFAULT));
    const char *layout = setting("keyboard_layout", "us"), *layout_name = layout;
    for (int i = 0; i < LAYOUT_COUNT; i++) {
        layout_name = !strcmp(layout, layouts[i][0]) ? layouts[i][1] : layout_name;
    }
    snprintf(rows[1].value, sizeof(rows[1].value), "%s", layout_name);
    snprintf(rows[2].value, sizeof(rows[2].value), "%s", setting("time_zone", "")[0] ? setting("time_zone", "") : "UTC");
    struct vx_net_interface card;
    bool any;
    wired_card(&card, &any);
    char ip[16];
    snprintf(rows[3].value, sizeof(rows[3].value), "%s", any && card.address ? vx_format_ipv4(card.address, ip)
                                                          : any ? "Waiting for an address" : "No network card");
    rows[0].icon = "palette", rows[0].label = "Look";
    rows[1].icon = "settings", rows[1].label = "Keyboard";
    rows[2].icon = "calendar", rows[2].label = "Time zone";
    rows[3].icon = "network", rows[3].label = "Network";
    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        int x = cx - 210 + col * 250, y = 224 + row * 52 + rise(2 + i);
        icon(rows[i].icon, x, y, 36);
        text_at(x + 46, y + 1, sans(12), rows[i].label, VX_COLOR_DIM);
        snprintf(line, sizeof(line), "%s", rows[i].value);
        vx_draw_text_fit(S, x + 46, y + 17, 190, line, VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    paragraph(cx - 270, 336 + rise(6), 540, 20, sans(13),
              "Find this tour again in the Vexa menu, under System, Welcome. Enjoy.",
              VX_COLOR_DIM, true);
    button(cx - 250, FOOTER_Y + 10, 150, 34, "Get Apps", false, H_SOFTWARE, 0);
    button(cx - 90, FOOTER_Y + 10, 150, 34, "Settings", false, H_SETTINGS, 0);
    button(cx + 80, FOOTER_Y + 10, 170, 34, "Start Using Vexa", true, H_FINISH, 0);
}

static void draw_page(struct vx_surface *target, int p, long age) {
    S = target;
    page_age = age;
    /* The card's glass, beside the steps. */
    for (int y = 0; y < CARD_H; y++) {
        memcpy(S->pixels + (long)y * S->stride, card_bg.pixels + (long)y * card_bg.stride + SIDEBAR,
               (size_t)PAGE_W * 4);
    }
    hit_count = 0;
    hit_dx = SIDEBAR;
    switch (p) {
    case P_WELCOME: page_welcome(); break;
    case P_LOOK: page_look(); break;
    case P_REGION: page_region(); break;
    case P_NETWORK: page_network(); break;
    case P_TOUR: page_tour(); break;
    case P_DONE: page_done(); break;
    }
    hit_dx = 0;
}

/* ---- The steps, down the left (as in the Installer's sidebar) ---- */

static void disc(int cx, int cy, int r, uint32_t color) {
    vx_fill_rounded(S, cx - r, cy - r, 2 * r, 2 * r, r, color, 255);
}

/* Drawn over the card's left side, in the card's own coordinates (the hits
 * too), so a click on a step done goes back to it. */
static void rail(struct vx_surface *win) {
    static const char *const names[PAGE_COUNT] = {"Welcome", "Your look", "Keyboard and time",
                                                  "Network", "Tour", "Finish"};
    static struct vx_surface view;
    view = (struct vx_surface){win->pixels + (long)card_y * win->stride + card_x, SIDEBAR, CARD_H, win->stride};
    struct vx_surface *keep = S;
    S = &view;
    hit_dx = 0;
    uint32_t top = vx_mix(0x1c2033, VX_COLOR_ACCENT, 70), bottom = vx_mix(0x0d0f18, VX_COLOR_ACCENT, 25);
    for (int row = 0; row < CARD_H; row++) {
        vx_fill(S, 0, row, SIDEBAR, 1, vx_mix(top, bottom, row * 255 / (CARD_H - 1)));
    }
    for (int row = 0; row < 120; row++) { /* A soft sheen across the top. */
        vx_fill_rounded(S, 0, row, SIDEBAR, 1, 0, 0xffffff, (120 - row) * 18 / 120);
    }
    vx_fill(S, SIDEBAR - 1, 0, 1, CARD_H, vx_mix(bottom, 0x000000, 80));

    vx_draw_gel(S, 20, 24, 44, 44, 14, VX_COLOR_ACCENT);
    centered(42, 30, bold(26), "V", 0xffffff);
    text_at(78, 28, bold(16), "Vexa Setup", 0xffffff);
    char step[32];
    snprintf(step, sizeof(step), "Step %d of %d", page + 1, PAGE_COUNT);
    text_at(78, 50, sans(12), step, 0xc8cce0);

    int y0 = 124, gap = 50;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int cy = y0 + i * gap;
        if (i + 1 < PAGE_COUNT) { /* The line to the next: lit where it's done. */
            vx_fill(S, 41, cy + 13, 2, gap - 26, i < page ? VX_COLOR_ACCENT : 0x4a4f66);
        }
        struct hit *hit = NULL;
        if (i < page) {
            hit = add_hit(10, cy - 20, SIDEBAR - 20, 40, H_STEP, i);
        }
        if (i == page) {
            vx_fill_rounded(S, 12, cy - 20, SIDEBAR - 24, 40, 10, 0xffffff, 40);
            disc(42, cy, 15, vx_mix(VX_COLOR_ACCENT, top, 140)); /* Glow. */
            disc(42, cy, 12, 0xffffff);
            char number[4];
            snprintf(number, sizeof(number), "%d", i + 1);
            centered(42, cy - 8, bold(13), number, VX_COLOR_ACCENT);
        } else if (i < page) {
            if (hit && hot(hit)) {
                vx_fill_rounded(S, 12, cy - 20, SIDEBAR - 24, 40, 10, 0xffffff, 18);
            }
            disc(42, cy, 12, VX_COLOR_ACCENT);
            tick(42 - 10, cy - 10, 20, 0xffffff);
        } else {
            disc(42, cy, 12, 0x4a4f66);
            disc(42, cy, 10, vx_mix(top, bottom, i * 255 / PAGE_COUNT));
            char number[4];
            snprintf(number, sizeof(number), "%d", i + 1);
            centered(42, cy - 8, sans(12), number, 0x8a8fa8);
        }
        const struct vx_font *f = i == page ? bold(13) : sans(14);
        uint32_t color = i == page ? 0xffffff : i < page ? 0xd8dbe8 : 0x8a8fa8;
        text_at(66, cy - vx_font_height(f) / 2, f, names[i], color);
    }
    text_at(28, CARD_H - 30, sans(11), "Vexa: a small system of its own", 0x7d82a0);
    S = keep;
}

/* ---- The window ---- */

static bool sliding(void) {
    return leaving >= 0;
}

/* A page on the card, `at` pixels to the right of where it belongs (negative:
 * to the left); what's outside the card isn't drawn. */
static void blit_card(struct vx_surface *win, const struct vx_surface *p, int at) {
    int w = PAGE_W - (at < 0 ? -at : at);
    if (w > 0) {
        vx_blit(win, card_x + SIDEBAR + (at > 0 ? at : 0), card_y, p, at < 0 ? -at : 0, 0, w, CARD_H);
    }
}

static void draw_frame(void) {
    reload_settings();
    struct vx_surface *win = &window->surface;
    long now = now_ms();
    /* The backdrop doesn't change, so after the first frame only the card is
     * drawn again and shown (the desktop has less to copy, and what it reads
     * is the card as it was finished). */
    bool all = whole_window;
    whole_window = false;
    if (all) {
        memcpy(win->pixels, backdrop.pixels, (size_t)win_w * win_h * 4);
    }
    for (int y = card_y; y < card_y + CARD_H; y++) {
        memcpy(win->pixels + (long)y * win->stride + card_x, backdrop.pixels + (long)y * backdrop.stride + card_x,
               (size_t)CARD_W * 4);
    }
    /* The page (or, sliding, the one leaving and the one coming), beside the steps. */
    if (sliding() && now - slide_since >= SLIDE_MS) {
        leaving = -1;
    }
    if (sliding()) {
        int offset = PAGE_W * ease(now - slide_since, SLIDE_MS) / 1000;
        int old_at = -slide_dir * offset, new_at = old_at + slide_dir * PAGE_W;
        draw_page(&pages[0], leaving, 100000);
        draw_page(&pages[1], page, now - page_since);
        blit_card(win, &pages[0], old_at);
        blit_card(win, &pages[1], new_at);
    } else {
        draw_page(&pages[0], page, now - page_since);
        blit_card(win, &pages[0], 0);
    }
    rail(win);
    round_corners(win, &backdrop, card_x, card_y, CARD_W, CARD_H, 20);
    if (all) {
        vx_window_present(window, 0, 0, win_w, win_h);
    } else {
        vx_window_present(window, card_x, card_y, CARD_W, CARD_H);
    }
}

static void go(int target) {
    if (target < 0 || target >= PAGE_COUNT || target == page) {
        return;
    }
    leaving = page;
    slide_dir = target > page ? 1 : -1;
    page = target;
    slide_since = page_since = now_ms();
    focus = -1;
    zone_top = 0;
    if (page == P_REGION) { /* The chosen zone in view. */
        int found[400];
        int n = matching_zones(found, 400);
        const char *zone = setting("time_zone", "");
        for (int i = 0; i < n; i++) {
            if (!strcmp(vx_zones[found[i]].city, zone)) {
                zone_top = i > 2 ? i - 2 : 0;
            }
        }
    }
    printf("welcome: page %d\n", page);
    fflush(stdout);
}

/* Done with the welcome (whichever way): not shown again for this account. */
static void finish(void) {
    if (name_changed && name_field[0]) {
        const char *argv[] = {"hostname", name_field};
        struct vx_spawn spawn = {.argv = argv, .argc = 2};
        int child = vx_spawn("/bin/hostname", &spawn);
        if (child >= 0) {
            vx_wait(child, 0);
            vx_close(child);
        }
    }
    reload_settings();
    vx_settings_set_bool(&desk, "welcome_done", true);
    vx_settings_save(&desk);
    printf("welcome: finished\n");
    fflush(stdout);
}

static void open_app(const char *name, const char *argument) {
    struct vx_app app;
    if (vx_app_find(name, &app) == 0) {
        int process = vx_app_open(&app, argument);
        if (process >= 0) {
            vx_close(process);
        }
    }
}

static void click(const struct hit *h) {
    focus = -1;
    switch (h->kind) {
    case H_NEXT: go(page + 1); break;
    case H_BACK: go(page - 1); break;
    case H_SKIP: finish(); vx_window_destroy(window); exit(0);
    case H_THEME: {
        static const char *const values[] = {"light", "dark", "auto"};
        apply("theme", values[h->index]);
        break;
    }
    case H_ACCENT: apply("accent", vx_accents[h->index].name); break;
    case H_LAYOUT: apply("keyboard_layout", layouts[h->index][0]); focus = H_TRY_FIELD; break;
    case H_ZONE: apply("time_zone", vx_zones[h->index].city); break;
    case H_ZONE_FIELD: focus = H_ZONE_FIELD; break;
    case H_TRY_FIELD: focus = H_TRY_FIELD; break;
    case H_NAME_FIELD: focus = H_NAME_FIELD; break;
    case H_CHECK: check_state = CHECK_IDLE; break;
    case H_STEP: go(h->index); break;
    case H_SETTINGS: open_app("Settings", NULL); break;
    case H_SOFTWARE: open_app("Software", NULL); break;
    case H_FINISH: finish(); vx_window_destroy(window); exit(0);
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool pressed = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    pointer_x = e->x - card_x;
    pointer_y = e->y - card_y;
    int before = hover;
    hover = -1;
    for (int i = hit_count - 1; i >= 0 && !sliding(); i--) {
        if (vx_inside(pointer_x, pointer_y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
            hover = i;
            break;
        }
    }
    vx_window_set_cursor(window, hover >= 0 ? (hits[hover].kind == H_ZONE_FIELD || hits[hover].kind == H_TRY_FIELD ||
                                               hits[hover].kind == H_NAME_FIELD ? VX_CURSOR_TEXT : VX_CURSOR_HAND)
                                            : VX_CURSOR_ARROW);
    if (e->wheel && page == P_REGION && pointer_x > SIDEBAR + 330) {
        zone_top += e->wheel * 2;
        zone_top = zone_top < 0 ? 0 : zone_top;
    }
    if (pressed && hover >= 0) {
        click(&hits[hover]);
    } else if (pressed) {
        focus = -1;
    }
    (void)before;
}

static void key(const struct vx_gui_event *e) {
    if (e->value == 0) {
        return;
    }
    if (focus >= 0) {
        char *field = focus == H_TRY_FIELD ? try_field : focus == H_ZONE_FIELD ? zone_search : name_field;
        size_t size = focus == H_TRY_FIELD ? sizeof(try_field) : focus == H_ZONE_FIELD ? sizeof(zone_search)
                                                                                         : sizeof(name_field);
        if (e->key == VX_KEY_ENTER || e->key == VX_KEY_ESC) {
            focus = -1;
        } else if (vx_field_key(field, size, e)) {
            if (focus == H_NAME_FIELD) {
                name_changed = true;
            } else if (focus == H_ZONE_FIELD) {
                zone_top = 0;
            }
        }
        return;
    }
    if (e->key == VX_KEY_ENTER || e->key == VX_KEY_RIGHT) {
        if (page == P_DONE) {
            finish();
            vx_window_destroy(window);
            exit(0);
        }
        go(page + 1);
    } else if (e->key == VX_KEY_LEFT) {
        go(page - 1);
    }
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    window = vx_window_create_flags("Welcome to Vexa", 0, 0, VX_WINDOW_FULLSCREEN);
    if (!window) {
        fprintf(stderr, "welcome: no desktop to open a window on\n");
        return 1;
    }
    layout();
    if (vx_current_user(&me) != 0) {
        snprintf(me.name, sizeof(me.name), "there");
        me.full_name[0] = '\0';
    }
    vx_get_hostname(name_field, sizeof(name_field));
    make_scenery();
    page = P_WELCOME;
    page_since = now_ms();
    int held = 0;
    for (;;) {
        draw_frame();
        /* Animations (and the network's live status) need a frame now and then. */
        long wait = sliding() || now_ms() - page_since < 900 || page == P_WELCOME ? 33
                    : page == P_NETWORK                                          ? 200
                                                                                 : -1;
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, wait);
        if (got < 0) {
            break;
        }
        if (got == 0) {
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            finish();
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_THEME:
            vx_theme_load();
            make_scenery();
            break;
        case VX_GUI_RESIZE: /* The screen changed size: the window follows it. */
            if (vx_window_resize(window, e.width, e.height) == 0) {
                layout();
                make_scenery();
            }
            break;
        }
    }
    return 0;
}
