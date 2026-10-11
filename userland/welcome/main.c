/* welcome: the first-run experience (see docs/USER-GUIDE.md, "Welcome").
 *
 * Over the whole screen, like Windows XP's: first a greeting (the logo, the
 * user's name and Get Started, on the wallpaper, dimmed), then a setup in steps
 * on a frosted card in the screen's middle: the steps down its left (the ones
 * done ticked and clickable to go back to, the one you're on lit up, and how far
 * along it is), and the step's page beside them, which slides in. The steps:
 * your name and the computer's, the look (theme, accent and wallpaper), the
 * keyboard and time zone, the network, a short tour and a summary. The desktop
 * starts it the first time an account logs in on an installed Vexa (and the Vexa
 * menu has it, for another go). Choices apply as they're made, the same way
 * Settings does it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/app.h>
#include <vexa/desktop.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include <vexa/time.h>
#include <vexa/users.h>

#define CARD_W 1000
#define CARD_H 580
#define SIDEBAR 250                 /* The steps, down the left (as in the Installer). */
#define PAGE_W (CARD_W - SIDEBAR)   /* A page's width: what's beside them. */
#define MARGIN 48
#define FOOTER_Y (CARD_H - 64)      /* Back and Next, under a line. */
#define SLIDE_MS 340
#define FADE_MS 450                 /* The greeting and the card, one into the other. */
#define FIELD_H 34

/* The window is the whole screen; the card is in its middle (see layout). */
static int win_w, win_h, card_x, card_y;
/* The scale: the page is laid out in design units (for a CARD_W x CARD_H card)
 * and drawn at ui percent of that; the real sizes below are in pixels. */
static int ui = 100;
static int real_card_w, real_card_h, real_side, real_page_w;

/* A design unit in pixels. */
static int u(int v) {
    return v * ui / 100;
}
static bool whole_window;   /* Everything needs drawing and showing (at first, and after a change). */

enum page { P_WELCOME, P_WHO, P_LOOK, P_REGION, P_NETWORK, P_TOUR, P_DONE, PAGE_COUNT };
#define STEP_COUNT (PAGE_COUNT - 1) /* The greeting isn't a step. */

enum hit_kind {
    H_NEXT, H_BACK, H_SKIP, H_THEME, H_ACCENT, H_LAYOUT, H_ZONE, H_ZONE_FIELD, H_TRY_FIELD,
    H_NAME_FIELD, H_CHECK, H_SETTINGS, H_SOFTWARE, H_FINISH, H_STEP, H_WHO_FIELD, H_FIND_ZONE,
    H_WALL,
};

struct hit {
    int x, y, w, h;
    enum hit_kind kind;
    int index;
};

static struct vx_window *window;
static struct vx_surface backdrop;      /* The wallpaper, the card's glow and shadow. */
static struct vx_surface hero_bg;       /* The wallpaper, dimmed: behind the greeting. */
static struct vx_surface fade_from;     /* The frame being faded out of. */
static struct vx_surface card_bg;       /* What's behind a page: the frosted card. */
static struct vx_surface rail_bg;       /* The sidebar, as it looks (made with the scenery). */
static struct vx_surface pages[2];      /* A page being drawn (two while one slides). */
static uint32_t theme_key;              /* Which theme backdrop and card_bg are for. */

static struct hit hits[96];
static int hit_count;
static int hit_dx;                      /* Added to a hit's place: a page's is beside the steps. */
static int hit_px, hit_py;              /* And in pixels: the greeting's is the whole window's. */
static int hover = -1;                  /* The hit under the pointer. */
static struct vx_surface *S;            /* What's being drawn on. */
static long page_age;                   /* How long the page being drawn has been there. */

static int page, leaving = -1, slide_dir;
static long page_since, slide_since, fade_since = -1;

static struct vx_image *logo;           /* The Vexa logo (/share/logo.png). */
static struct vx_surface logo_mirror;  /* Its reflection: the bottom of it, flipped and fading. */
static struct vx_user me;
static char name_field[64], try_field[96], zone_search[48];
static char who_field[64];              /* The name Vexa greets the user by (their own setting). */
static bool who_changed;
static int focus = -1;                  /* The text field with the keyboard, or -1. */
static int zone_top;
static int pointer_x, pointer_y;
static bool name_changed;

/* The time zone from the network (a thread asks, the page shows what it found). */
enum { ZONE_IDLE, ZONE_RUNNING, ZONE_FOUND, ZONE_FAILED };
static volatile int zone_state = ZONE_IDLE;
static char zone_city[32];
static char zone_note[96] = "Asks ip-api.com for it: your address is sent.";
static struct vx_thread *zone_thread_handle;

/* The network page: the internet check runs in a thread of its own. */
enum { CHECK_IDLE, CHECK_RUNNING, CHECK_ONLINE, CHECK_OFFLINE };
static volatile int check_state = CHECK_IDLE;
static struct vx_thread *check_thread_handle;

static const char *const layouts[][2] = {
    {"us", "English (US)"}, {"gb", "English (UK)"}, {"de", "German"},
    {"fr", "French"},       {"es", "Spanish"},      {"dvorak", "Dvorak"},
};
#define LAYOUT_COUNT 6

/* The wallpapers to pick from: pictures (the default one is in the theme's
 * colors), and two of the desktop's gradients. */
static const struct {
    const char *label, *path, *gradient;
} walls[] = {
    {"Glass", DESKTOP_DEFAULT_WALLPAPER, NULL}, {"Aurora", "/share/pictures/aurora.png", NULL},
    {"Meadow", "/share/pictures/meadow.png", NULL}, {"Ocean", NULL, "ocean"}, {"Dusk", NULL, "dusk"},
};
#define WALL_COUNT 5
#define THUMB_W 118
#define THUMB_H 66
static struct vx_surface thumbs[WALL_COUNT];
static int thumbs_ui;                   /* The scale they were made at (0: not yet). */
static uint32_t glass_key;              /* The theme the glass one is in. */

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

/* A surface `s` of the window's (or card's) size, made again if the size changed. */
static void sized(struct vx_surface *s, int w, int h) {
    if (!s->pixels || s->width != w || s->height != h) {
        free(s->pixels);
        *s = make_surface(w, h);
    }
}

/* `from` over `s` (as much of it as `alpha`, 0 to 255): two channels at a time. */
static void blend(struct vx_surface *s, const struct vx_surface *from, int alpha) {
    uint32_t a = (uint32_t)alpha, b = 256 - a;
    for (int y = 0; y < s->height && y < from->height; y++) {
        uint32_t *d = s->pixels + (long)y * s->stride;
        const uint32_t *f = from->pixels + (long)y * from->stride;
        for (int x = 0; x < s->width && x < from->width; x++) {
            uint32_t p = d[x], q = f[x];
            uint32_t rb = ((q & 0xff00ff) * a + (p & 0xff00ff) * b) >> 8 & 0xff00ff;
            uint32_t g = ((q & 0x00ff00) * a + (p & 0x00ff00) * b) >> 8 & 0x00ff00;
            d[x] = rb | g;
        }
    }
}

/* A copy of a rectangle of `from` into `to` (the same place, the same size of surface). */
static void copy_rect(struct vx_surface *to, const struct vx_surface *from, int x, int y, int w, int h) {
    for (int row = y; row < y + h; row++) {
        memcpy(to->pixels + (long)row * to->stride + x, from->pixels + (long)row * from->stride + x,
               (size_t)w * 4);
    }
}

/* A glass bubble in the wallpaper (in pixels): a lit crescent at the top-left. */
static void bubble(int cx, int cy, int r) {
    vx_fill_rounded(&backdrop, cx - r, cy - r, 2 * r, 2 * r, r, 0xffffff, 20);
    vx_fill_rounded(&backdrop, cx - r + r / 6, cy - r + r / 6, 2 * r - r / 3, 2 * r - r / 3, r, 0xffffff, 12);
    vx_fill_rounded(&backdrop, cx - r * 3 / 5, cy - r * 3 / 5, r * 2 / 5, r / 4, r / 8, 0xffffff, 140);
}

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

/* The backdrop (the wallpaper chosen, with bubbles, a glow and the card's shadow),
 * the greeting's (the same, dimmed), the card's frosted glass (that part of the
 * backdrop, blurred, in the window's color, with a light from the top), and the
 * sidebar. */
static void make_scenery(void) {
    reload_settings();
    sized(&backdrop, win_w, win_h);
    sized(&hero_bg, win_w, win_h);
    sized(&fade_from, win_w, win_h);
    sized(&card_bg, real_card_w, real_card_h);
    sized(&rail_bg, real_side, real_card_h);
    sized(&pages[0], real_page_w, real_card_h);
    sized(&pages[1], real_page_w, real_card_h);
    whole_window = true;
    /* The wallpaper the desktop shows: a picture, or a gradient. */
    char path[96];
    struct vx_image *image = NULL;
    uint32_t top = vx_mix(vx_theme.accent, 0, 70), bottom = vx_mix(vx_theme.accent, 0, 200);
    const char *wall = setting("wallpaper", "image");
    if (!strcmp(wall, "image")) {
        const char *chosen = setting("wallpaper_image", DESKTOP_DEFAULT_WALLPAPER);
        if (strcmp(chosen, DESKTOP_DEFAULT_WALLPAPER)) {
            image = vx_image_load(chosen, 0);
        }
        if (!image) {
            vx_theme_wallpaper(&vx_theme, path, sizeof(path));
            image = vx_image_load(path, 0);
        }
    } else {
        for (int i = 0; i < DESKTOP_WALLPAPER_COUNT; i++) {
            if (!strcmp(wall, desktop_wallpapers[i].name)) {
                top = desktop_wallpapers[i].top, bottom = desktop_wallpapers[i].bottom;
            }
        }
    }
    if (image) {
        cover(&backdrop, &image->surface);
        vx_image_free(image);
    } else {
        for (int y = 0; y < win_h; y++) {
            vx_fill(&backdrop, 0, y, win_w, 1, vx_mix(top, bottom, y * 255 / (win_h - 1)));
        }
    }
    /* A few glass bubbles, where the card isn't. */
    static const struct {
        int x, y, r; /* Percent of the window, and the radius in design units. */
    } bubbles[] = {{9, 16, 40}, {19, 62, 24}, {5, 86, 50}, {88, 10, 22}, {93, 44, 34}, {84, 88, 60}, {34, 93, 14}};
    for (unsigned i = 0; i < sizeof(bubbles) / sizeof(bubbles[0]); i++) {
        int cx = win_w * bubbles[i].x / 100, cy = win_h * bubbles[i].y / 100, r = u(bubbles[i].r);
        bool over = cx + r > card_x - 16 && cx - r < card_x + real_card_w + 16 && cy + r > card_y - 16 &&
                    cy - r < card_y + real_card_h + 16;
        if (!over && cx - r >= 0 && cy - r >= 0 && cx + r < win_w && cy + r < win_h) {
            bubble(cx, cy, r);
        }
    }
    /* The greeting's: dimmed, and a little more towards the bottom. */
    for (int y = 0; y < win_h; y++) {
        int amount = 80 + 50 * y / win_h;
        const uint32_t *from = backdrop.pixels + (long)y * backdrop.stride;
        uint32_t *to = hero_bg.pixels + (long)y * hero_bg.stride;
        for (int x = 0; x < win_w; x++) {
            to[x] = vx_mix(from[x], 0x041312, amount);
        }
    }
    /* A glow of light round the card, under it. */
    uint32_t light = vx_mix(VX_COLOR_ACCENT, 0xffffff, 90);
    for (int i = 16; i >= 1; i--) {
        vx_fill_rounded(&backdrop, card_x - 2 * i, card_y - 2 * i + 10, real_card_w + 4 * i, real_card_h + 4 * i,
                        u(22) + 2 * i, light, 3);
    }
    /* The frosted glass: the part behind the card, smaller, blurred, enlarged,
     * and mostly the window's color, with a veil of white from the top. */
    struct vx_surface small = make_surface(real_card_w / 4, real_card_h / 4);
    for (int y = 0; y < small.height; y++) {
        for (int x = 0; x < small.width; x++) {
            small.pixels[y * small.stride + x] =
                backdrop.pixels[(long)(card_y + y * 4 + 2) * backdrop.stride + card_x + x * 4 + 2];
        }
    }
    blur(&small, 4);
    enlarge(&card_bg, &small);
    free(small.pixels);
    int veil = real_card_h / 3;
    for (int y = 0; y < real_card_h; y++) {
        int amount = y < veil ? (veil - y) * (vx_theme.dark ? 40 : 90) / veil : 0;
        for (int x = 0; x < real_card_w; x++) {
            uint32_t *p = &card_bg.pixels[(long)y * card_bg.stride + x];
            *p = vx_mix(vx_mix(*p, vx_theme.window, 224), 0xffffff, amount);
        }
    }
    /* The shadow, and a thin light rim round the card. */
    for (int i = 14; i >= 1; i--) {
        vx_fill_rounded(&backdrop, card_x - i, card_y - i + 8, real_card_w + 2 * i, real_card_h + 2 * i, u(22) + i,
                        0x000000, 5);
    }
    vx_fill_rounded(&backdrop, card_x - 1, card_y - 1, real_card_w + 2, real_card_h + 2, u(22) + 1, 0xffffff, 110);
    /* The sidebar: the accent, deepening downwards, with a sheen at the top. */
    top = vx_mix(VX_COLOR_ACCENT, 0x000000, 120), bottom = vx_mix(0x0c1420, VX_COLOR_ACCENT, 14);
    int sheen = real_card_h / 3;
    for (int row = 0; row < real_card_h; row++) {
        uint32_t c = vx_mix(top, bottom, row * 255 / (real_card_h - 1));
        if (row < sheen) {
            c = vx_mix(c, 0xffffff, (sheen - row) * 36 / sheen);
        }
        vx_fill(&rail_bg, 0, row, real_side, 1, c);
    }
    vx_fill(&rail_bg, real_side - 1, 0, 1, real_card_h, vx_mix(bottom, 0x000000, 80));
    theme_key = vx_theme.accent ^ (vx_theme.dark ? 0xa5a5a5a5u : 0);
}

/* The window's size (the screen's), the scale that fits, and the card in its
 * middle: the design's size on 1280 x 800, a quarter bigger on 1920 x 1080 (up
 * to half again on a bigger screen), and smaller on a small one, to fit. */
static void layout(void) {
    win_w = window->surface.width;
    win_h = window->surface.height;
    int by_width = win_w * 100 / 1280, by_height = win_h * 100 / 800;
    ui = (by_width < by_height ? by_width : by_height) / 25 * 25;
    ui = ui < 100 ? 100 : ui > 150 ? 150 : ui;
    int fit_w = (win_w - 32) * 100 / CARD_W, fit_h = (win_h - 32) * 100 / CARD_H;
    int fit = fit_w < fit_h ? fit_w : fit_h;
    ui = ui > fit ? fit : ui;
    ui = ui < 50 ? 50 : ui;
    real_card_w = u(CARD_W);
    real_card_h = u(CARD_H);
    real_side = u(SIDEBAR);
    real_page_w = real_card_w - real_side;
    card_x = win_w > real_card_w ? (win_w - real_card_w) / 2 : 0;
    card_y = win_h > real_card_h ? (win_h - real_card_h) / 2 : 0;
    whole_window = true;
}

/* ---- Drawing ---- */

/* Drawing on the page, in design units. Edges are scaled (not sizes), so a
 * line one unit thick stays whole, and gradients have no gaps between rows. */
/* Drawing on the page, in design units. Edges are scaled (not sizes), so a
 * line one unit thick stays whole, and gradients have no gaps between rows. */
static void fill(int x, int y, int w, int h, uint32_t color) {
    int x0 = u(x), y0 = u(y);
    vx_fill(S, x0, y0, u(x + w) - x0, u(y + h) - y0, color);
}

static void fill_rounded(int x, int y, int w, int h, int r, uint32_t color, int alpha) {
    int x0 = u(x), y0 = u(y);
    vx_fill_rounded(S, x0, y0, u(x + w) - x0, u(y + h) - y0, u(r), color, alpha);
}

static void gel(int x, int y, int w, int h, int r, uint32_t color) {
    int x0 = u(x), y0 = u(y);
    vx_draw_gel(S, x0, y0, u(x + w) - x0, u(y + h) - y0, u(r), color);
}

/* A picture (an image) in a box, in design units. */
static void picture(const struct vx_image *image, int x, int y, int w, int h) {
    int x0 = u(x), y0 = u(y);
    vx_blit_alpha(S, x0, y0, u(x + w) - x0, u(y + h) - y0, &image->surface);
}

/* ---- Type: DejaVu Sans, bold for headings and sans for the body (scaled with the card) ---- */

static const struct vx_font *bold(int size) {
    return vx_font(VX_FACE_BOLD, u(size));
}

static const struct vx_font *sans(int size) {
    return vx_font(VX_FACE_SANS, u(size));
}

static int text_at(int x, int y, const struct vx_font *font, const char *text, uint32_t color) {
    return vx_text(S, font, u(x), u(y), text, color, VX_TRANSPARENT);
}

static void centered(int cx, int y, const struct vx_font *font, const char *text, uint32_t color) {
    vx_text(S, font, u(cx) - vx_text_width_font(font, text) / 2, u(y), text, color, VX_TRANSPARENT);
}

/* A paragraph, wrapped, in lines of `line` pixels; returns the y below it. */
static int paragraph(int x, int y, int width, int line, const struct vx_font *font, const char *text,
                     uint32_t color, bool center) {
    char lines[8][160];
    int n = vx_text_wrap(lines, 8, text, u(width), font);
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

/* Text cut to `w` design units, with an ellipsis if it's longer. */
static void fit_text(int x, int y, int w, const struct vx_font *font, const char *text, uint32_t color) {
    static const char dots[] = "\xe2\x80\xa6"; /* U+2026 */
    int width = u(w);
    if (vx_text_width_font(font, text) <= width) {
        text_at(x, y, font, text, color);
        return;
    }
    char line[160];
    size_t keep = vx_text_fit_bytes(font, text, width - vx_text_width_font(font, dots));
    keep = keep > sizeof(line) - sizeof(dots) ? sizeof(line) - sizeof(dots) : keep;
    memcpy(line, text, keep);
    strcpy(line + keep, dots);
    text_at(x, y, font, line, color);
}

/* The ink of headings, the bevel under text (white on light, black on dark), and
 * a panel's glass (the card's color with a touch of the accent). */
static uint32_t ink(void) {
    return vx_theme.dark ? VX_COLOR_TEXT : vx_mix(VX_COLOR_ACCENT, 0x000000, 110);
}

static uint32_t shade(void) {
    return vx_theme.dark ? 0x000000 : 0xffffff;
}

/* Text with a bevel: the `under` color one pixel below it (in pixels). */
static void embossed(int px, int py, const struct vx_font *font, const char *text, uint32_t color,
                     uint32_t under) {
    vx_text(S, font, px, py + 1, text, under, VX_TRANSPARENT);
    vx_text(S, font, px, py, text, color, VX_TRANSPARENT);
}

/* A heading centered on cx, its top at y (design units). */
static void heading(int cx, int y, const struct vx_font *font, const char *text, uint32_t color) {
    embossed(u(cx) - vx_text_width_font(font, text) / 2, u(y), font, text, color, shade());
}

/* A glossy bar or button (design units): `base` at the bottom, `top` over the upper
 * half, a sheen across the top if `sheen`, and a darker edge all round. */
static void gloss(int x, int y, int w, int h, int r, uint32_t base, uint32_t top, bool sheen) {
    int x0 = u(x), y0 = u(y), w0 = u(x + w) - x0, h0 = u(y + h) - y0, rr = u(r);
    vx_fill_rounded(S, x0, y0, w0, h0, rr, vx_mix(base, 0x000000, 60), 255);
    vx_fill_rounded(S, x0 + 1, y0 + 1, w0 - 2, h0 - 2, rr, base, 255);
    vx_fill_rounded(S, x0 + 1, y0 + 1, w0 - 2, (h0 - 2) * 56 / 100, rr, top, 255);
    if (sheen) {
        vx_fill_rounded(S, x0 + 3, y0 + 2, w0 - 6, (h0 - 4) * 42 / 100, rr > 2 ? rr - 2 : rr, 0xffffff, 64);
    }
}

/* The accent, glossy: the next step's buttons, the chips, the bars. */
static void accent_gloss(int x, int y, int w, int h, int r) {
    gloss(x, y, w, h, r, vx_mix(VX_COLOR_ACCENT, 0x000000, 90), vx_mix(VX_COLOR_ACCENT, 0x000000, 30), true);
}

/* The logo's reflection: its lower 30 percent, flipped, and fading to nothing. */
static void make_mirror(void) {
    free(logo_mirror.pixels);
    logo_mirror = (struct vx_surface){NULL, 0, 0, 0};
    if (!logo) {
        return;
    }
    const struct vx_surface *src = &logo->surface;
    int rows = src->height * 30 / 100;
    logo_mirror = make_surface(src->width, rows);
    for (int y = 0; y < rows; y++) {
        const uint32_t *from = src->pixels + (long)(src->height - 1 - y) * src->stride;
        uint32_t *to = logo_mirror.pixels + (long)y * logo_mirror.stride;
        uint32_t fade = 120 * (uint32_t)(rows - y) / (uint32_t)rows;
        for (int x = 0; x < src->width; x++) {
            to[x] = ((from[x] >> 24) * fade / 255) << 24 | (from[x] & 0xffffff);
        }
    }
}

static struct hit *add_hit(int x, int y, int w, int h, enum hit_kind kind, int index) {
    static struct hit spare;
    if (hit_count == (int)(sizeof(hits) / sizeof(hits[0]))) {
        return &spare;
    }
    int x0 = u(x + hit_dx) + hit_px, y0 = u(y) + hit_py;
    hits[hit_count] = (struct hit){x0, y0, u(x + hit_dx + w) + hit_px - x0, u(y + h) + hit_py - y0, kind, index};
    return &hits[hit_count++];
}

static bool hot(const struct hit *h) {
    return hover >= 0 && hover < hit_count && hits[hover].kind == h->kind &&
           hits[hover].index == h->index;
}

/* A button: glossy. The next step's is the accent (and always so); the others are
 * grey glass, tinted by the accent when the pointer is on them. */
static void button(int x, int y, int w, int h, const char *label, bool primary, enum hit_kind kind,
                   int index) {
    struct hit *hit = add_hit(x, y, w, h, kind, index);
    bool hovered = hot(hit);
    if (hovered) {
        fill_rounded(x - 3, y - 3, w + 6, h + 6, 11, VX_COLOR_ACCENT, 90);
    }
    int size = h >= 44 ? 16 : 14;
    const struct vx_font *f = primary ? bold(size) : sans(size);
    int px = u(x) + (u(w) - vx_text_width_font(f, label)) / 2;
    int py = u(y) + (u(h) - vx_font_height(f)) / 2;
    if (primary) {
        accent_gloss(x, y, w, h, 8);
        embossed(px, py, f, label, 0xffffff, vx_mix(VX_COLOR_ACCENT, 0x000000, 150));
    } else {
        uint32_t base = vx_theme.dark ? 0x3a3d46 : 0xc4cad6, top = vx_theme.dark ? 0x555a66 : 0xf6f8fb;
        if (hovered) {
            base = vx_mix(base, VX_COLOR_ACCENT, 60);
            top = vx_mix(top, VX_COLOR_ACCENT, 50);
        }
        gloss(x, y, w, h, 8, base, top, true);
        embossed(px, py, f, label, VX_COLOR_TEXT, shade());
    }
}

static uint32_t card_color(void) {
    return vx_theme.dark ? vx_mix(vx_theme.window, 0xffffff, 14) : 0xffffff;
}

/* The panel glass: the card's color, with a touch of the accent. */
static uint32_t panel_color(void) {
    return vx_mix(card_color(), VX_COLOR_ACCENT, 22);
}

/* A glass panel inside a page: a lit rim, a soft shadow beneath, and a sheen on the top half. */
static void panel(int x, int y, int w, int h) {
    uint32_t rim = vx_theme.dark ? vx_mix(card_color(), 0xffffff, 50) : vx_mix(VX_COLOR_ACCENT, 0xffffff, 150);
    fill_rounded(x, y + 3, w, h, 12, 0x000000, vx_theme.dark ? 70 : 22);
    fill_rounded(x, y, w, h, 12, rim, 255);
    fill_rounded(x + 1, y + 1, w - 2, h - 2, 11, panel_color(), 255);
    fill_rounded(x + 2, y + 2, w - 4, (h - 4) / 2, 10, 0xffffff, vx_theme.dark ? 16 : 150);
}

/* A text box (design units, FIELD_H high): the accent round it when it has the
 * keyboard (and a caret), and the end of the text shown when it's long. */
static void entry(int x, int y, int w, const char *text, bool focused, const char *placeholder,
                  enum hit_kind kind) {
    add_hit(x, y, w, FIELD_H, kind, 0);
    if (focused) {
        fill_rounded(x - 3, y - 3, w + 6, FIELD_H + 6, 10, VX_COLOR_ACCENT, 70);
    }
    uint32_t edge = focused ? VX_COLOR_ACCENT : vx_theme.dark ? 0x55575f : 0xb4bcc8;
    fill_rounded(x, y, w, FIELD_H, 7, edge, 255);
    fill_rounded(x + 1, y + 1, w - 2, FIELD_H - 2, 6, VX_COLOR_VIEW, 255);
    fill(x + 4, y + 1, w - 8, 1, vx_theme.dark ? 0x101012 : 0xdde2ea); /* Set in: a shade at the top. */
    const struct vx_font *f = sans(15);
    const char *tail = text;
    while (*tail && vx_text_width_font(f, tail) > u(w - 26)) {
        tail++;
        while ((*tail & 0xc0) == 0x80) {
            tail++;
        }
    }
    int px = u(x + 12), py = u(y) + (u(FIELD_H) - vx_font_height(f)) / 2;
    if (!text[0] && !focused && placeholder) {
        vx_text(S, f, px, py, placeholder, VX_COLOR_DIM, VX_TRANSPARENT);
    }
    int end = vx_text(S, f, px, py, tail, VX_COLOR_TEXT, VX_TRANSPARENT);
    if (focused) {
        vx_fill(S, end + 1, py + 1, ui >= 150 ? 2 : 1, vx_font_height(f) - 2, VX_COLOR_ACCENT);
    }
}

/* A tick: two strokes. */
static void tick(int x, int y, int size, uint32_t color) {
    int t = size / 6 > 1 ? size / 6 : 2;
    for (int i = 0; i <= size * 3 / 8; i++) {
        fill(x + i, y + size / 2 + i - t / 2, t, t, color);
    }
    for (int i = 0; i <= size * 5 / 8; i++) {
        fill(x + size * 3 / 8 + i, y + size / 2 + size * 3 / 8 - i - t / 2, t, t, color);
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
                picture(cache[i].image, x, y, size, size);
            }
            return;
        }
    }
}

/* A key on a keyboard; returns its width. */
static int keycap(int x, int y, const char *label) {
    int w = vx_text_width_font(sans(12), label) * 100 / ui + 18;
    w = w < 30 ? 30 : w;
    uint32_t face = vx_theme.dark ? 0x4a4a4e : 0xffffff;
    uint32_t edge = vx_theme.dark ? 0x232325 : 0xaab0bb;
    fill_rounded(x, y + 2, w, 24, 6, edge, 255);
    fill_rounded(x, y, w, 24, 6, vx_mix(edge, face, 120), 255);
    fill_rounded(x + 1, y + 1, w - 2, 21, 5, face, 255);
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

/* A page's title (bold, with a bevel) and what it's about, under it. */
static void header(const char *title, const char *about) {
    embossed(u(MARGIN), u(40 + rise(0)), bold(28), title, VX_COLOR_TEXT, shade());
    fit_text(MARGIN, 84 + rise(1), PAGE_W - 2 * MARGIN, sans(14), about, VX_COLOR_DIM);
}

/* A heading inside a page. */
static void label(int x, int y, const char *text) {
    text_at(x, y, bold(15), text, VX_COLOR_TEXT);
}

static void footer(bool back, const char *next) {
    fill(MARGIN, FOOTER_Y - 10, PAGE_W - 2 * MARGIN, 1, VX_COLOR_LINE);
    if (back) {
        button(MARGIN, FOOTER_Y + 12, 110, 38, "Back", false, H_BACK, 0);
    }
    button(PAGE_W - MARGIN - 170, FOOTER_Y + 12, 170, 38, next, true, H_NEXT, 0);
}

/* ---- The pages ---- */

/* A little desktop in a theme: a sky of the accent, a panel, a window. */
static void mini_desktop(int x, int y, int w, int h, bool dark, uint32_t accent) {
    struct vx_theme t;
    const char *accent_name = setting("accent", VX_ACCENT_DEFAULT);
    vx_theme_make(&t, dark ? "dark" : "light", accent_name);
    uint32_t sky = vx_mix(accent, 0, dark ? 150 : 70), deep = vx_mix(accent, 0, dark ? 230 : 200);
    for (int row = 0; row < h; row++) {
        fill(x, y + row, w, 1, vx_mix(sky, deep, row * 255 / (h - 1)));
    }
    fill(x, y, w, 8, vx_mix(t.panel, sky, 60));
    fill_rounded(x + 3, y + 2, 14, 5, 2, t.accent, 255);
    int wx = x + 20, wy = y + 20, ww = w - 40, wh = h - 30;
    fill_rounded(wx - 1, wy + 1, ww + 2, wh + 3, 6, 0x000000, 70);
    fill_rounded(wx, wy, ww, wh, 5, t.window, 255);
    fill_rounded(wx, wy, ww, 12, 5, t.title_focused, 255);
    fill(wx, wy + 7, ww, 5, t.title_focused);
    static const uint32_t balls[3] = {0xfebc2e, 0x2ac845, 0xff5f57};
    for (int i = 0; i < 3; i++) {
        gel(wx + ww - 27 + i * 9, wy + 3, 7, 7, 3, balls[i]);
    }
    fill(wx, wy + 12, 28, wh - 12, t.sidebar);
    fill_rounded(wx + 3, wy + 16, 22, 6, 3, t.accent, 255);
    fill(wx + 36, wy + 18, 50, 5, t.text);
    fill(wx + 36, wy + 28, ww - 48, 3, t.dim);
    fill(wx + 36, wy + 35, ww - 60, 3, t.dim);
    gel(wx + ww - 36, wy + wh - 14, 28, 9, 4, t.accent);
}

/* Up to two letters of a name ("Ada Lovelace": "AL"), for the user's tile. */
static void initials(const char *name, char out[3]) {
    int n = 0;
    bool start = true;
    for (const char *p = name; *p && n < 2; p++) {
        if (*p == ' ') {
            start = true;
        } else if (start) {
            start = false;
            if ((unsigned char)*p < 0x80) {
                out[n++] = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
            }
        }
    }
    if (!n) {
        out[n++] = '?';
    }
    out[n] = '\0';
}

/* The name of the user (as Vexa greets them) and the computer's name. */
static void page_who(void) {
    header("Who will use this computer?", "Vexa greets you by your name. The computer's name is how other computers see it.");
    /* The user's tile: their initials, on the accent. */
    int ty = 132 + rise(2);
    fill_rounded(MARGIN, ty + 4, 132, 132, 24, 0x000000, vx_theme.dark ? 70 : 26);
    accent_gloss(MARGIN, ty, 132, 132, 24);
    char letters[3];
    initials(who_field[0] ? who_field : me.name, letters);
    const struct vx_font *big = bold(46);
    embossed(u(MARGIN + 66) - vx_text_width_font(big, letters) / 2, u(ty + 66) - vx_font_height(big) / 2, big,
             letters, 0xffffff, vx_mix(VX_COLOR_ACCENT, 0x000000, 150));
    char account[80];
    snprintf(account, sizeof(account), "Account: %s", me.name);
    centered(MARGIN + 66, ty + 146, sans(13), account, VX_COLOR_DIM);
    /* The two names. */
    int x = MARGIN + 164, w = PAGE_W - MARGIN - x;
    int y = 132 + rise(3);
    panel(x, y, w, 112);
    label(x + 20, y + 16, "Your name");
    text_at(x + 20, y + 38, sans(13), "Leave it as it is to use your account's name.", VX_COLOR_DIM);
    entry(x + 20, y + 62, w - 40, who_field, focus == H_WHO_FIELD, "Your name", H_WHO_FIELD);
    y = 262 + rise(4);
    panel(x, y, w, 112);
    label(x + 20, y + 16, "Name this computer");
    text_at(x + 20, y + 38, sans(13), "Other computers on the network will see it.", VX_COLOR_DIM);
    entry(x + 20, y + 62, w - 40, name_field, focus == H_NAME_FIELD, "vexa", H_NAME_FIELD);
    int ny = 404 + rise(5);
    panel(MARGIN, ny, PAGE_W - 2 * MARGIN, 64);
    icon("help", MARGIN + 16, ny + 14, 36);
    text_at(MARGIN + 66, ny + 13, bold(14), "Tip", VX_COLOR_TEXT);
    text_at(MARGIN + 66, ny + 33, sans(13), "Tab moves between the boxes, and Enter goes on to the next step.", VX_COLOR_DIM);
    footer(true, "Next");
}

/* The wallpaper chosen: one of walls[], or -1. */
static int chosen_wall(void) {
    const char *wall = setting("wallpaper", "image");
    const char *path = setting("wallpaper_image", DESKTOP_DEFAULT_WALLPAPER);
    for (int i = 0; i < WALL_COUNT; i++) {
        if (walls[i].gradient ? !strcmp(wall, walls[i].gradient)
                              : !strcmp(wall, "image") && !strcmp(path, walls[i].path)) {
            return i;
        }
    }
    return -1;
}

static void use_wall(int i) {
    reload_settings();
    if (walls[i].gradient) {
        vx_settings_set(&desk, "wallpaper", walls[i].gradient);
    } else {
        vx_settings_set(&desk, "wallpaper", "image");
        vx_settings_set(&desk, "wallpaper_image", walls[i].path);
    }
    vx_settings_save(&desk);
    vx_desktop_reload();
    printf("welcome: wallpaper=%s\n", walls[i].label);
    fflush(stdout);
    make_scenery(); /* This window shows it too. */
}

/* The wallpapers' pictures, small: made when first wanted (and the glass one
 * again when the theme changes, as it's in the theme's colors). */
static void make_thumbs(void) {
    bool all = thumbs_ui != ui;
    for (int i = 0; i < WALL_COUNT; i++) {
        bool glass = walls[i].path && !strcmp(walls[i].path, DESKTOP_DEFAULT_WALLPAPER);
        if (!all && !(glass && glass_key != theme_key)) {
            continue;
        }
        sized(&thumbs[i], u(THUMB_W), u(THUMB_H));
        struct vx_surface *t = &thumbs[i];
        struct vx_image *image = NULL;
        if (walls[i].path) {
            char path[96];
            snprintf(path, sizeof(path), "%s", walls[i].path);
            if (glass) {
                vx_theme_wallpaper(&vx_theme, path, sizeof(path));
            }
            image = vx_image_load(path, 0);
        }
        if (image) {
            cover(t, &image->surface);
            vx_image_free(image);
        } else {
            uint32_t top = 0x303030, bottom = 0x101010;
            for (int k = 0; k < DESKTOP_WALLPAPER_COUNT; k++) {
                if (walls[i].gradient && !strcmp(walls[i].gradient, desktop_wallpapers[k].name)) {
                    top = desktop_wallpapers[k].top, bottom = desktop_wallpapers[k].bottom;
                }
            }
            for (int y = 0; y < t->height; y++) {
                vx_fill(t, 0, y, t->width, 1, vx_mix(top, bottom, y * 255 / (t->height - 1)));
            }
        }
    }
    thumbs_ui = ui;
    glass_key = theme_key;
}

static void page_look(void) {
    header("Make it yours", "Pick a look. Everything follows at once, this window too.");
    const char *theme = setting("theme", "light"), *accent = setting("accent", VX_ACCENT_DEFAULT);
    static const char *const values[] = {"light", "dark", "auto"};
    static const char *const titles[] = {"Light", "Dark", "Automatic"};
    uint32_t accent_color = vx_theme.accent;
    /* The theme: three little desktops. */
    label(MARGIN, 124 + rise(2), "Theme");
    int pw = 196, ph = 104, gap = (PAGE_W - 2 * MARGIN - 3 * pw) / 2;
    for (int i = 0; i < 3; i++) {
        int x = MARGIN + i * (pw + gap), y = 150 + rise(2);
        bool on = !strcmp(theme, values[i]);
        struct hit *hit = add_hit(x - 4, y - 4, pw + 8, ph + 34, H_THEME, i);
        if (on) {
            fill_rounded(x - 5, y - 5, pw + 10, ph + 10, 12, VX_COLOR_ACCENT, 255);
            fill_rounded(x - 3, y - 3, pw + 6, ph + 6, 10, card_color(), 255);
        } else if (hot(hit)) {
            fill_rounded(x - 3, y - 3, pw + 6, ph + 6, 11, vx_mix(card_color(), VX_COLOR_ACCENT, 50), 255);
            fill_rounded(x - 2, y - 2, pw + 4, ph + 4, 10, card_color(), 255);
        }
        if (i == 2) { /* Light on the left, dark on the right, cut aslant with a line of the accent. */
            mini_desktop(x, y, pw, ph, false, accent_color);
            struct vx_surface right = make_surface(u(pw), u(ph));
            struct vx_surface *keep = S;
            S = &right;
            mini_desktop(0, 0, pw, ph, true, accent_color);
            S = keep;
            int rx = u(x), ry = u(y), rw = u(pw), rh = u(ph);
            for (int row = 0; row < rh; row++) {
                int cut = rw / 2 + (rh / 2 - row) * 2 / 5;
                memcpy(S->pixels + (long)(ry + row) * S->stride + rx + cut,
                       right.pixels + (long)row * right.stride + cut, (size_t)(rw - cut) * 4);
                for (int k = 0; k < 2; k++) {
                    uint32_t *edge = S->pixels + (long)(ry + row) * S->stride + rx + cut + k;
                    *edge = vx_mix(*edge, accent_color, 200);
                }
            }
            free(right.pixels);
        } else {
            mini_desktop(x, y, pw, ph, i == 1, accent_color);
        }
        round_corners(S, &card_bg, u(x), u(y), u(x + pw) - u(x), u(y + ph) - u(y), u(8)); /* (Close enough at its edge.) */
        if (on) {
            fill_rounded(x + pw - 26, y + 6, 22, 22, 11, 0xffffff, 255);
            gel(x + pw - 24, y + 8, 18, 18, 9, VX_COLOR_ACCENT);
            tick(x + pw - 20, y + 11, 11, 0xffffff);
        }
        centered(x + pw / 2, y + ph + 10, on ? bold(14) : sans(14), titles[i], VX_COLOR_TEXT);
    }
    /* The accent colors: glossy orbs, the chosen one ringed. */
    label(MARGIN, 298 + rise(3), "Accent color");
    int step = (PAGE_W - 2 * MARGIN) / vx_accent_count;
    for (int i = 0; i < vx_accent_count; i++) {
        int cx = MARGIN + i * step + step / 2, cy = 340 + rise(3);
        bool on = !strcmp(accent, vx_accents[i].name);
        struct hit *hit = add_hit(cx - step / 2, cy - 22, step, 64, H_ACCENT, i);
        if (on) {
            fill_rounded(cx - 22, cy - 22, 44, 44, 22, vx_accents[i].color, 255);
            fill_rounded(cx - 20, cy - 20, 40, 40, 20, card_color(), 255);
        }
        int r = on || hot(hit) ? 17 : 15;
        gel(cx - r, cy - r, 2 * r, 2 * r, r, vx_accents[i].color);
        if (on) {
            tick(cx - 8, cy - 9, 16, 0xffffff);
        }
        centered(cx, cy + 24, sans(12), vx_accents[i].label, on ? VX_COLOR_TEXT : VX_COLOR_DIM);
    }
    /* The wallpaper: little pictures, with their names on them. */
    label(MARGIN, 398 + rise(4), "Wallpaper");
    make_thumbs();
    int chosen = chosen_wall(), tgap = (PAGE_W - 2 * MARGIN - WALL_COUNT * THUMB_W) / (WALL_COUNT - 1);
    for (int i = 0; i < WALL_COUNT; i++) {
        int x = MARGIN + i * (THUMB_W + tgap), y = 424 + rise(4);
        struct hit *hit = add_hit(x - 3, y - 3, THUMB_W + 6, THUMB_H + 6, H_WALL, i);
        if (i == chosen) {
            fill_rounded(x - 4, y - 4, THUMB_W + 8, THUMB_H + 8, 10, VX_COLOR_ACCENT, 255);
        } else if (hot(hit)) {
            fill_rounded(x - 3, y - 3, THUMB_W + 6, THUMB_H + 6, 9, vx_mix(card_color(), VX_COLOR_ACCENT, 120), 255);
        }
        vx_blit(S, u(x), u(y), &thumbs[i], 0, 0, thumbs[i].width, thumbs[i].height);
        for (int row = 0; row < 20; row++) { /* A shade at the bottom, for the name. */
            fill_rounded(x, y + THUMB_H - 20 + row, THUMB_W, 1, 0, 0x000000, row * 7);
        }
        round_corners(S, i == chosen ? S : &card_bg, u(x), u(y), u(x + THUMB_W) - u(x), u(y + THUMB_H) - u(y), u(6));
        text_at(x + 8, y + THUMB_H - 18, bold(12), walls[i].label, 0xffffff);
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

/* ---- The time zone from the network ---- */

/* Asks for the time zone of this computer's internet address (ip-api.com, over
 * plain HTTP: the address is sent, and only that). It answers the zone's name
 * ("America/New_York") and the offset in seconds, on two lines. */
static void *find_zone(void *arg) {
    (void)arg;
    int result = ZONE_FAILED;
    int handle = vx_connect_to("ip-api.com", 80);
    if (handle >= 0) {
        static const char request[] = "GET /line/?fields=timezone,offset HTTP/1.0\r\nHost: ip-api.com\r\n\r\n";
        static char reply[512];
        size_t got = 0;
        if (vx_write(handle, request, sizeof(request) - 1) > 0) {
            long n;
            while (got < sizeof(reply) - 1 && (n = vx_read(handle, reply + got, sizeof(reply) - 1 - got)) > 0) {
                got += (size_t)n;
            }
        }
        vx_close(handle);
        reply[got] = '\0';
        char *body = strstr(reply, "\r\n\r\n");
        if (body) {
            body += 4;
            char name[64] = "";
            size_t length = strcspn(body, "\r\n");
            if (length && length < sizeof(name)) {
                memcpy(name, body, length);
                name[length] = '\0';
            }
            char city[32];
            const char *slash = strrchr(name, '/');
            snprintf(city, sizeof(city), "%s", slash ? slash + 1 : name);
            for (char *c = city; *c; c++) {
                *c = *c == '_' ? ' ' : *c;
            }
            const struct vx_zone *zone = vx_find_zone(city);
            if (!zone) { /* Not a city of ours: the zone with that offset (now, or standard). */
                char *after = body + length;
                long minutes = strtol(after + strspn(after, "\r\n"), NULL, 10) / 60;
                for (int i = 0; i < vx_zone_count && !zone; i++) {
                    if (vx_zones[i].offset == minutes || vx_zones[i].offset == minutes - 60) {
                        zone = &vx_zones[i];
                    }
                }
            }
            if (zone) {
                snprintf(zone_city, sizeof(zone_city), "%s", zone->city);
                result = ZONE_FOUND;
            }
        }
    }
    __atomic_store_n(&zone_state, result, __ATOMIC_SEQ_CST);
    return NULL;
}

static void start_zone_lookup(void) {
    if (zone_state == ZONE_RUNNING) {
        return;
    }
    if (zone_thread_handle) {
        vx_thread_join(zone_thread_handle);
        zone_thread_handle = NULL;
    }
    zone_state = ZONE_RUNNING;
    zone_thread_handle = vx_thread_create(find_zone, NULL);
    if (!zone_thread_handle) {
        zone_state = ZONE_FAILED;
    }
}

/* When the lookup has an answer: the time zone is set to it (on the main
 * thread, where the settings are). */
static void settle_zone(void) {
    if (zone_state != ZONE_FOUND && zone_state != ZONE_FAILED) {
        return;
    }
    if (zone_state == ZONE_FOUND) {
        apply("time_zone", zone_city);
        snprintf(zone_note, sizeof(zone_note), "Time zone set to %s.", zone_city);
    } else {
        snprintf(zone_note, sizeof(zone_note), "Couldn't find it. Is the network connected?");
    }
    zone_state = ZONE_IDLE;
}

static void page_region(void) {
    header("Keyboard and time", "So the keys type what they say, and the clock is right.");
    /* The layouts. */
    int lx = MARGIN, ly = 128 + rise(2), lw = 272;
    label(lx, ly, "Keyboard layout");
    const char *layout = setting("keyboard_layout", "us");
    for (int i = 0; i < LAYOUT_COUNT; i++) {
        int y = ly + 30 + i * 32 + rise(3);
        bool on = !strcmp(layout, layouts[i][0]);
        struct hit *hit = add_hit(lx - 4, y, lw + 8, 30, H_LAYOUT, i);
        if (on) {
            accent_gloss(lx - 4, y, lw + 8, 30, 8);
        } else if (hot(hit)) {
            fill_rounded(lx - 4, y, lw + 8, 30, 8, vx_mix(card_color(), VX_COLOR_ACCENT, 50), 255);
        }
        text_at(lx + 10, y + 6, sans(15), layouts[i][1], on ? 0xffffff : VX_COLOR_TEXT);
    }
    /* Try the keys. */
    int ty = ly + 30 + LAYOUT_COUNT * 32 + 14;
    text_at(lx, ty, sans(13), "Try it here:", VX_COLOR_DIM);
    entry(lx, ty + 20, lw, try_field, focus == H_TRY_FIELD, NULL, H_TRY_FIELD);
    /* The time zones. */
    int zx = 352, zw = PAGE_W - MARGIN - zx;
    label(zx, ly, "Time zone");
    button(zx + zw - 150, ly - 6, 150, 28, "From my network", false, H_FIND_ZONE, 0);
    entry(zx, ly + 30, zw, zone_search, focus == H_ZONE_FIELD, "Search for a city", H_ZONE_FIELD);
    int found[400];
    int n = matching_zones(found, 400);
    const char *zone = setting("time_zone", "");
    int rows = 7, list_y = ly + 76 + rise(3), row_h = 30;
    panel(zx, list_y, zw, rows * row_h + 10);
    if (zone_top > n - rows) {
        zone_top = n - rows;
    }
    zone_top = zone_top < 0 ? 0 : zone_top;
    for (int r = 0; r < rows && zone_top + r < n; r++) {
        int i = found[zone_top + r];
        int y = list_y + 5 + r * row_h;
        bool on = !strcmp(zone, vx_zones[i].city);
        struct hit *hit = add_hit(zx + 5, y, zw - 20, row_h - 2, H_ZONE, i);
        if (on) {
            accent_gloss(zx + 5, y, zw - 20, row_h - 3, 7);
        } else if (hot(hit)) {
            fill_rounded(zx + 5, y, zw - 20, row_h - 3, 7, vx_mix(card_color(), VX_COLOR_ACCENT, 50), 255);
        }
        char offset[24];
        int minutes = vx_zones[i].offset;
        snprintf(offset, sizeof(offset), "UTC%+d:%02d", minutes / 60, (minutes < 0 ? -minutes : minutes) % 60);
        int w = vx_text_width_font(sans(12), offset) * 100 / ui;
        fit_text(zx + 16, y + 6, zw - 60 - w, sans(15), vx_zones[i].city, on ? 0xffffff : VX_COLOR_TEXT);
        text_at(zx + zw - 26 - w, y + 8, sans(12), offset, on ? 0xe8f4f2 : VX_COLOR_DIM);
    }
    if (!n) {
        text_at(zx + 16, list_y + 14, sans(14), "No city matches", VX_COLOR_DIM);
    }
    if (n > rows) { /* A scroll bar. */
        int bar = (rows * row_h) * rows / n;
        bar = bar < 24 ? 24 : bar;
        int at = list_y + 5 + (rows * row_h - bar) * zone_top / (n - rows);
        fill_rounded(zx + zw - 10, at, 4, bar, 2, VX_COLOR_DIM, 255);
    }
    /* What time it is there, and what's happening with the lookup. */
    struct vx_date date;
    vx_local_now(&date);
    char clock[96];
    snprintf(clock, sizeof(clock), "It's %02d:%02d on %s %d%s.", date.hour, date.minute,
             vx_month_names[date.month - 1], date.day, zone[0] ? "" : " (set a city)");
    text_at(zx, list_y + rows * row_h + 20, sans(13), clock, VX_COLOR_TEXT);
    fit_text(zx, list_y + rows * row_h + 40, zw, sans(13),
             zone_state == ZONE_RUNNING ? "Asking for your time zone..." : zone_note, VX_COLOR_DIM);
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
    int top = 136 + rise(2);
    fill_rounded(MARGIN, top + 4, 124, 124, 62, 0x000000, vx_theme.dark ? 60 : 20);
    fill_rounded(MARGIN, top, 124, 124, 62, vx_mix(VX_COLOR_ACCENT, 0xffffff, 150), 255);
    fill_rounded(MARGIN + 1, top + 1, 122, 122, 61, panel_color(), 255);
    fill_rounded(MARGIN + 8, top + 6, 108, 52, 46, 0xffffff, vx_theme.dark ? 14 : 130);
    icon(connected ? "network-cable" : "network", MARGIN + 16, top + 16, 92);
    int x = MARGIN + 156, y = top + 8;
    char line[160], ip[16], router[16];
    if (!any) {
        text_at(x, y, bold(20), "No network card found", VX_COLOR_TEXT);
        paragraph(x, y + 36, PAGE_W - x - MARGIN, 22, sans(15),
                  "That's fine: Vexa works without one. If this computer has a card Vexa doesn't "
                  "know yet, Settings, Network and Device Manager will say what it found.",
                  VX_COLOR_DIM, false);
    } else if (!connected) {
        text_at(x, y, bold(20), "Waiting for an address", VX_COLOR_TEXT);
        int dots = (int)(now_ms() / 300 % 4);
        snprintf(line, sizeof(line), "%s is asking the router for one%.*s", card.name, dots, "...");
        text_at(x, y + 36, sans(15), line, VX_COLOR_DIM);
    } else {
        int after = vx_text_width_font(bold(20), "Connected") * 100 / ui;
        text_at(x, y, bold(20), "Connected", VX_COLOR_TEXT);
        fill_rounded(x + after + 10, y + 3, 20, 20, 10, 0x3fbf6f, 255);
        tick(x + after + 14, y + 6, 12, 0xffffff);
        snprintf(line, sizeof(line), "Wired (%s). Address %s.", card.name, vx_format_ipv4(card.address, ip));
        text_at(x, y + 36, sans(15), line, VX_COLOR_DIM);
        if (card.gateway) {
            snprintf(line, sizeof(line), "Router %s.", vx_format_ipv4(card.gateway, router));
            text_at(x, y + 60, sans(15), line, VX_COLOR_DIM);
        }
        /* Can it reach the internet? */
        int cy = y + 104;
        int state = check_state;
        if (state == CHECK_IDLE) {
            start_check();
            state = check_state;
        }
        if (state == CHECK_RUNNING) {
            int phase = (int)(now_ms() / 120 % 8);
            for (int i = 0; i < 8; i++) {
                int d = (phase + 8 - i) % 8;
                fill_rounded(x + 2 + i * 6, cy + 6, 4, 12, 2, vx_mix(VX_COLOR_ACCENT, card_color(), d * 30), 255);
            }
            text_at(x + 62, cy + 3, sans(15), "Checking the internet", VX_COLOR_DIM);
        } else if (state == CHECK_ONLINE) {
            fill_rounded(x, cy, 24, 24, 12, 0x3fbf6f, 255);
            tick(x + 6, cy + 6, 12, 0xffffff);
            text_at(x + 34, cy + 3, bold(15), "Online: Vexa can reach the internet", VX_COLOR_TEXT);
        } else {
            fill_rounded(x, cy, 24, 24, 12, 0xf0ad4e, 255);
            centered(x + 12, cy + 3, bold(15), "!", 0xffffff);
            text_at(x + 34, cy + 3, bold(15), "Can't reach the internet", VX_COLOR_TEXT);
            paragraph(x + 34, cy + 30, PAGE_W - x - MARGIN - 34, 20, sans(13),
                      "The router may not be online. Check its cable, or carry on: you can "
                      "connect later.",
                      VX_COLOR_DIM, false);
        }
        if (state != CHECK_RUNNING) {
            button(PAGE_W - MARGIN - 140, cy - 4, 140, 32, "Check Again", false, H_CHECK, 0);
        }
    }
    int ny = 404 + rise(4);
    panel(MARGIN, ny, PAGE_W - 2 * MARGIN, 64);
    icon("help", MARGIN + 16, ny + 14, 36);
    text_at(MARGIN + 66, ny + 13, bold(14), "No Wi-Fi yet", VX_COLOR_TEXT);
    text_at(MARGIN + 66, ny + 33, sans(13), "A wired card connects as soon as the cable is in.", VX_COLOR_DIM);
    footer(true, "Next");
}

/* ---- The tour ---- */

static void tip(int col, int row, const char *icon_name, const char *title, const char *combo,
                const char *about, enum hit_kind action, const char *action_label) {
    int tw = (PAGE_W - 2 * MARGIN - 16) / 2, th = 106;
    int x = MARGIN + col * (tw + 16), y = 124 + row * (th + 10) + rise(2 + row * 2 + col);
    panel(x, y, tw, th);
    fill_rounded(x + 16, y + 25, 56, 56, 28, vx_mix(VX_COLOR_ACCENT, 0xffffff, 110), 255);
    icon(icon_name, x + 21, y + 30, 46);
    text_at(x + 88, y + 14, bold(15), title, VX_COLOR_TEXT);
    if (combo[0]) {
        keys(x + 88, y + 38, combo);
    }
    paragraph(x + 88, y + (combo[0] ? 68 : 40), tw - 104, 17, sans(13), about, VX_COLOR_DIM, false);
    if (action_label) {
        button(x + tw - 96, y + 12, 84, 28, action_label, false, action, 0);
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
    /* A badge that fills in, with a tick, and a halo behind it. */
    int t = ease(page_age, 700);
    int top = 34 + rise(0);
    fill_rounded(cx - 74, top - 22, 148, 148, 74, 0x3fbf6f, 34);
    fill_rounded(cx - 52, top + 4, 104, 104, 52, vx_mix(card_color(), 0x3fbf6f, 70), 255);
    if (t > 120) {
        gel(cx - 44, top + 12, 88, 88, 44, 0x3fbf6f);
    }
    if (t > 500) {
        tick(cx - 24, top + 32, 48, 0xffffff);
    }
    heading(cx, 168 + rise(1), bold(32), "You're all set", VX_COLOR_TEXT);
    /* What was chosen, in glass tiles. */
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
        int x = cx - 316 + col * 324, y = 236 + row * 74 + rise(2 + i);
        panel(x, y, 308, 62);
        icon(rows[i].icon, x + 14, y + 11, 40);
        text_at(x + 66, y + 10, sans(13), rows[i].label, VX_COLOR_DIM);
        fit_text(x + 66, y + 30, 228, sans(15), rows[i].value, VX_COLOR_TEXT);
    }
    paragraph(cx - 280, 402 + rise(6), 560, 20, sans(14),
              "Find this tour again in the Vexa menu, under System, Welcome. Enjoy.",
              VX_COLOR_DIM, true);
    fill(MARGIN, FOOTER_Y - 10, PAGE_W - 2 * MARGIN, 1, VX_COLOR_LINE);
    button(cx - 280, FOOTER_Y + 12, 168, 38, "Get Apps", false, H_SOFTWARE, 0);
    button(cx - 100, FOOTER_Y + 12, 168, 38, "Settings", false, H_SETTINGS, 0);
    button(cx + 80, FOOTER_Y + 12, 200, 38, "Start Using Vexa", true, H_FINISH, 0);
}

static void draw_page(struct vx_surface *target, int p, long age) {
    S = target;
    page_age = age;
    /* The card's glass, beside the steps. */
    for (int y = 0; y < real_card_h; y++) {
        memcpy(S->pixels + (long)y * S->stride, card_bg.pixels + (long)y * card_bg.stride + real_side,
               (size_t)real_page_w * 4);
    }
    hit_count = 0;
    hit_dx = SIDEBAR;
    switch (p) {
    case P_WHO: page_who(); break;
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
    fill_rounded(cx - r, cy - r, 2 * r, 2 * r, r, color, 255);
}

/* Drawn over the card's left side, in the card's own coordinates (the hits too,
 * so a step done is clickable to go back to it), with how far along it is at the
 * bottom. */
static void rail(struct vx_surface *win) {
    static const char *const names[PAGE_COUNT] = {"Welcome", "Your name", "Your look",
                                                  "Keyboard and time", "Network", "Tour", "Finish"};
    static struct vx_surface view;
    view = (struct vx_surface){win->pixels + (long)card_y * win->stride + card_x, real_side, real_card_h,
                               win->stride};
    copy_rect(&view, &rail_bg, 0, 0, real_side, real_card_h);
    struct vx_surface *keep = S;
    S = &view;
    hit_dx = 0;
    if (logo) {
        picture(logo, 20, 22, 210, 84);
    }
    const uint32_t lit = vx_mix(VX_COLOR_ACCENT, 0xffffff, 120), idle = 0x3a4158;
    int y0 = 156, gap = 48;
    for (int i = P_WHO; i < PAGE_COUNT; i++) {
        int cy = y0 + (i - P_WHO) * gap;
        if (i + 1 < PAGE_COUNT) { /* The line to the next: lit where it's done. */
            fill(45, cy + 15, 2, gap - 30, i < page ? lit : idle);
        }
        struct hit *hit = i < page ? add_hit(10, cy - 20, SIDEBAR - 20, 40, H_STEP, i) : NULL;
        char number[4];
        snprintf(number, sizeof(number), "%d", i);
        if (i == page) {
            fill_rounded(12, cy - 20, SIDEBAR - 24, 40, 11, 0xffffff, 34); /* The glass under it. */
            fill_rounded(14, cy - 19, SIDEBAR - 28, 17, 10, 0xffffff, 28);
            fill_rounded(29, cy - 17, 34, 34, 17, lit, 110);              /* The glow. */
            disc(46, cy, 13, 0xffffff);
            centered(46, cy - 8, bold(13), number, ink());
        } else if (i < page) {
            if (hot(hit)) {
                fill_rounded(12, cy - 20, SIDEBAR - 24, 40, 11, 0xffffff, 18);
            }
            gloss(33, cy - 13, 26, 26, 13, vx_mix(VX_COLOR_ACCENT, 0x000000, 60), VX_COLOR_ACCENT, true);
            tick(46 - 10, cy - 10, 20, 0xffffff);
        } else {
            disc(46, cy, 13, idle);
            disc(46, cy, 12, 0x1b2131);
            centered(46, cy - 8, sans(13), number, 0x8a8fa8);
        }
        const struct vx_font *f = i == page ? bold(15) : sans(15);
        uint32_t color = i == page ? 0xffffff : i < page ? 0xd8dbe8 : 0x8a8fa8;
        text_at(72, cy - vx_font_height(f) * 100 / ui / 2, f, names[i], color);
    }
    /* How far along: a glossy bar. */
    char along[32];
    snprintf(along, sizeof(along), "Step %d of %d", page, STEP_COUNT);
    text_at(24, CARD_H - 98, sans(13), along, 0xd8dbe8);
    int bar = SIDEBAR - 48;
    fill_rounded(24, CARD_H - 74, bar, 10, 5, 0x000000, 110);
    gloss(24, CARD_H - 74, bar * page / STEP_COUNT, 10, 5, vx_mix(VX_COLOR_ACCENT, 0x000000, 60), lit, false);
    text_at(24, CARD_H - 46, sans(11), "Vexa: a small system of its own", 0x9aa3c0);
    S = keep;
}

/* ---- The greeting: the whole screen, before the steps ---- */

static int hero_x, hero_y, hero_w, hero_h; /* The part of it that changes (pixels). */

/* Text with a soft shadow under it (pixels), for the greeting's light text on the wallpaper. */
static void lifted(int px, int py, const struct vx_font *font, const char *text, uint32_t color) {
    vx_text(S, font, px + 1, py + u(2), text, 0x031110, VX_TRANSPARENT);
    vx_text(S, font, px, py, text, color, VX_TRANSPARENT);
}

static void lifted_center(int cx, int y, const struct vx_font *font, const char *text, uint32_t color) {
    lifted(u(cx) - vx_text_width_font(font, text) / 2, u(y), font, text, color);
}

static void hero(struct vx_surface *win, bool all, long age) {
    int cx = win_w * 50 / ui, cy = win_h * 50 / ui; /* The middle, in design units. */
    hero_x = u(cx - 340), hero_y = u(cy - 290), hero_w = u(680), hero_h = u(530);
    hero_x = hero_x < 0 ? 0 : hero_x, hero_y = hero_y < 0 ? 0 : hero_y;
    hero_w = hero_x + hero_w > win_w ? win_w - hero_x : hero_w;
    hero_h = hero_y + hero_h > win_h ? win_h - hero_y : hero_h;
    if (all) {
        copy_rect(win, &hero_bg, 0, 0, win_w, win_h);
    } else {
        copy_rect(win, &hero_bg, hero_x, hero_y, hero_w, hero_h);
    }
    S = win;
    page_age = age;
    hit_count = 0;
    hit_dx = 0;
    hit_px = -card_x, hit_py = -card_y; /* Hits are in the card's coordinates. */
    if (logo) {
        picture(logo, cx - 220, cy - 270 + rise(0) / 2, 440, 176);
        if (logo_mirror.pixels) {
            vx_blit_alpha(S, u(cx - 220), u(cy - 94 + rise(0) / 2), u(440), u(26), &logo_mirror);
        }
    }
    if (shown(1)) {
        lifted_center(cx, cy - 62 + rise(1), bold(44), "Welcome to Vexa", 0xffffff);
    }
    if (shown(2)) {
        char hi[120];
        snprintf(hi, sizeof(hi), "Hello, %s.", who_field[0] ? who_field : me.full_name[0] ? me.full_name : me.name);
        lifted_center(cx, cy + 2 + rise(2), sans(21), hi, vx_mix(VX_COLOR_ACCENT, 0xffffff, 170));
    }
    if (shown(3)) {
        static const char *const lines[] = {"Let's make this computer yours. It takes about a minute,",
                                            "and you can change everything later in Settings."};
        for (int i = 0; i < 2; i++) {
            lifted_center(cx, cy + 46 + i * 24 + rise(3), sans(15), lines[i], 0xcfe4e1);
        }
    }
    if (shown(4)) {
        button(cx - 130, cy + 120 + rise(4), 260, 48, "Get Started", true, H_NEXT, 0);
        struct hit *skip = add_hit(cx - 70, cy + 186, 140, 22, H_SKIP, 0);
        lifted_center(cx, cy + 188, sans(13), "Skip for now", hot(skip) ? 0xffffff : 0xa9c9c4);
    }
    hit_px = hit_py = 0;
}

/* ---- The window ---- */

static bool sliding(void) {
    return leaving >= 0;
}

/* A page on the card, `at` pixels to the right of where it belongs (negative:
 * to the left); what's outside the card isn't drawn. */
static void blit_card(struct vx_surface *win, const struct vx_surface *p, int at) {
    int w = real_page_w - (at < 0 ? -at : at);
    if (w > 0) {
        vx_blit(win, card_x + real_side + (at > 0 ? at : 0), card_y, p, at < 0 ? -at : 0, 0, w, real_card_h);
    }
}

/* The card: the backdrop round it, the page (or two, sliding), and the steps. */
static void card(struct vx_surface *win, bool all, long now) {
    if (all) {
        copy_rect(win, &backdrop, 0, 0, win_w, win_h);
    } else {
        copy_rect(win, &backdrop, card_x, card_y, real_card_w, real_card_h);
    }
    if (sliding() && now - slide_since >= SLIDE_MS) {
        leaving = -1;
    }
    if (sliding()) {
        int offset = real_page_w * ease(now - slide_since, SLIDE_MS) / 1000;
        int old_at = -slide_dir * offset, new_at = old_at + slide_dir * real_page_w;
        draw_page(&pages[0], leaving, 100000);
        draw_page(&pages[1], page, now - page_since);
        blit_card(win, &pages[0], old_at);
        blit_card(win, &pages[1], new_at);
    } else {
        draw_page(&pages[0], page, now - page_since);
        blit_card(win, &pages[0], 0);
    }
    rail(win);
    round_corners(win, &backdrop, card_x, card_y, real_card_w, real_card_h, u(20));
}

static bool fading(void) {
    return fade_since >= 0;
}

static void draw_frame(void) {
    settle_zone();
    reload_settings();
    struct vx_surface *win = &window->surface;
    long now = now_ms();
    if (fading() && now - fade_since >= FADE_MS) {
        fade_since = -1;
        whole_window = true;
    }
    /* What doesn't change (the backdrop) is drawn once: after that, only the part
     * that does is drawn again and shown (but all of it while fading). */
    bool all = whole_window || fading();
    whole_window = false;
    int x = card_x, y = card_y, w = real_card_w, h = real_card_h;
    if (page == P_WELCOME) {
        hero(win, all, now - page_since);
        x = hero_x, y = hero_y, w = hero_w, h = hero_h;
    } else {
        card(win, all, now);
    }
    if (fading()) {
        blend(win, &fade_from, 255 - 255 * ease(now - fade_since, FADE_MS) / 1000);
    }
    if (all) {
        vx_window_present(window, 0, 0, win_w, win_h);
    } else {
        vx_window_present(window, x, y, w, h);
    }
}

static void remember_name(void) {
    if (who_changed) {
        vx_settings_set(&desk, "display_name", who_field);
        who_changed = false;
    }
}

/* The greeting and the card fade one into the other (from what's shown now). */
static void start_fade(void) {
    copy_rect(&fade_from, &window->surface, 0, 0, win_w, win_h);
    fade_since = now_ms();
}

static void go(int target) {
    if (target < 0 || target >= PAGE_COUNT || target == page) {
        return;
    }
    if (page == P_WHO && who_changed) {
        reload_settings();
        remember_name();
        vx_settings_save(&desk);
    }
    if (page == P_WELCOME || target == P_WELCOME) {
        start_fade();
        leaving = -1;
    } else {
        leaving = page;
        slide_dir = target > page ? 1 : -1;
        slide_since = now_ms();
    }
    page = target;
    page_since = now_ms();
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
    remember_name();
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

/* The welcome melody, once as the window opens. The player runs on its own: nothing waits for it. */
static void play_welcome_sound(void) {
    const char *argv[] = {"play", "--welcome"};
    struct vx_spawn spawn = {.argv = argv, .argc = 2, .handles = {0, 1, 2}};
    long child = vx_spawn("/bin/play", &spawn);
    if (child >= 0) {
        vx_close((int)child);
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
    case H_WALL: use_wall(h->index); break;
    case H_LAYOUT: apply("keyboard_layout", layouts[h->index][0]); focus = H_TRY_FIELD; break;
    case H_ZONE: apply("time_zone", vx_zones[h->index].city); break;
    case H_ZONE_FIELD: focus = H_ZONE_FIELD; break;
    case H_TRY_FIELD: focus = H_TRY_FIELD; break;
    case H_NAME_FIELD: focus = H_NAME_FIELD; break;
    case H_WHO_FIELD: focus = H_WHO_FIELD; break;
    case H_FIND_ZONE: start_zone_lookup(); break;
    case H_CHECK: check_state = CHECK_IDLE; break;
    case H_STEP: go(h->index); break;
    case H_SETTINGS: open_app("Settings", NULL); break;
    case H_SOFTWARE: open_app("Software", NULL); break;
    case H_FINISH: finish(); vx_window_destroy(window); exit(0);
    }
}

static bool is_field(enum hit_kind kind) {
    return kind == H_ZONE_FIELD || kind == H_TRY_FIELD || kind == H_NAME_FIELD || kind == H_WHO_FIELD;
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool pressed = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    pointer_x = e->x - card_x;
    pointer_y = e->y - card_y;
    hover = -1;
    for (int i = hit_count - 1; i >= 0 && !sliding() && !fading(); i--) {
        if (vx_inside(pointer_x, pointer_y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
            hover = i;
            break;
        }
    }
    vx_window_set_cursor(window, hover < 0 ? VX_CURSOR_ARROW : is_field(hits[hover].kind) ? VX_CURSOR_TEXT
                                                                                         : VX_CURSOR_HAND);
    if (e->wheel && page == P_REGION && pointer_x > real_side + u(352)) {
        zone_top += e->wheel * 2;
        zone_top = zone_top < 0 ? 0 : zone_top;
    }
    if (pressed && hover >= 0) {
        click(&hits[hover]);
    } else if (pressed) {
        focus = -1;
    }
}

static void key(const struct vx_gui_event *e) {
    if (e->value == 0) {
        return;
    }
    /* Tab: the next text box on the page. */
    if (e->key == VX_KEY_TAB && (page == P_WHO || page == P_REGION)) {
        int a = page == P_WHO ? H_WHO_FIELD : H_TRY_FIELD, b = page == P_WHO ? H_NAME_FIELD : H_ZONE_FIELD;
        focus = focus == a ? b : a;
        return;
    }
    if (focus >= 0) {
        char *field = focus == H_TRY_FIELD ? try_field : focus == H_ZONE_FIELD ? zone_search
                      : focus == H_WHO_FIELD ? who_field : name_field;
        size_t size = focus == H_TRY_FIELD ? sizeof(try_field) : focus == H_ZONE_FIELD ? sizeof(zone_search)
                      : focus == H_WHO_FIELD ? sizeof(who_field) : sizeof(name_field);
        if (e->key == VX_KEY_ESC) {
            focus = -1;
        } else if (e->key == VX_KEY_ENTER) {
            /* The names are done with Enter: on to the next step. */
            bool names = focus == H_WHO_FIELD || focus == H_NAME_FIELD;
            focus = -1;
            if (names) {
                go(page + 1);
            }
        } else if (vx_field_key(field, size, e)) {
            if (focus == H_NAME_FIELD) {
                name_changed = true;
            } else if (focus == H_WHO_FIELD) {
                who_changed = true;
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
    reload_settings();
    snprintf(who_field, sizeof(who_field), "%s",
             setting("display_name", me.full_name[0] ? me.full_name : me.name));
    logo = vx_image_load("/share/logo.png", VX_IMAGE_ALPHA);
    make_mirror();
    make_scenery();
    play_welcome_sound();
    page = P_WELCOME;
    page_since = now_ms();
    int held = 0;
    for (;;) {
        draw_frame();
        /* Animation only while a page settles in, slides or fades (and the
         * network's live status): once it has, nothing moves until something is done. */
        long wait = fading() || sliding() || now_ms() - page_since < 1200 ? 33
                    : zone_state == ZONE_RUNNING                         ? 100
                    : page == P_NETWORK                                 ? 200
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
                fade_since = -1;
                layout();
                make_scenery();
            }
            break;
        }
    }
    return 0;
}
