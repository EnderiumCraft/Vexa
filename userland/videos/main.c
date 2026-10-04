/* Videos: plays MPEG-1 videos (.mpg, with MP2 sound).
 *
 * Without a file it opens on the library: the videos in ~/Videos and
 * /share/videos as cards, each with its first picture, its name and how
 * long it is; a click plays one. Playing, the picture fills the window
 * (its shape kept, or filling it all: F), scaled smoothly; the controls
 * float over it while the pointer moves: back to the library, the name,
 * and a glass bar with play, ten seconds back or on, the scrubber (it
 * shows the time under the pointer), the time and this app's volume. A
 * big play button sits in the middle while it's paused.
 *
 *   videos [file]         the window, playing the file (or the library)
 *   videos --play file    plays it without a window, and says what it is
 *
 * Keys: Space plays or pauses, Left/Right go 5 seconds back or on, Up/Down
 * the volume, F fits or fills, Home goes back to the start, Escape back
 * to the library. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vexa/gui.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include <vexa/users.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../third_party/media/pl_mpeg.h"
#pragma GCC diagnostic pop
#include "../music/look.h"

#define CONTROLS_MS 2500 /* How long the controls stay after the pointer stops. */
#define BAR_H 64
#define TOP_H 56
#define MAX_VIDEOS 64
#define THUMB_W 256
#define THUMB_H 144

static struct vx_window *window;
static int audio = -1;
static volatile int volume = 100;

/* ---- The video playing ---- */

static plm_t *plm;
static uint8_t *file_data;
static char name[200];
static int video_w, video_h, rate;
static double duration, framerate;

static struct vx_mutex lock = VX_MUTEX_INIT;   /* Around plm (seeking, decoding). */
static struct vx_mutex frame_lock = VX_MUTEX_INIT;
static uint32_t *frame;           /* The newest picture (video_w x video_h). */
static volatile bool new_frame, playing, ended, have_video;
static volatile long frames;

static void on_video(plm_t *p, plm_frame_t *f, void *user) {
    (void)p, (void)user;
    vx_mutex_lock(&frame_lock);
    plm_frame_to_bgra(f, (uint8_t *)frame, video_w * 4); /* (B, G, R, A: 0xAARRGGBB.) */
    new_frame = true;
    frames++;
    vx_mutex_unlock(&frame_lock);
}

static void on_audio(plm_t *p, plm_samples_t *s, void *user) {
    (void)p, (void)user;
    static int16_t out[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
    float gain = (float)(volume * volume) / 10000.0f * 32767.0f;
    for (unsigned i = 0; i < s->count * 2; i++) {
        float v = s->interleaved[i] * gain;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    const char *data = (const char *)out;
    long left = (long)s->count * 4;
    while (audio >= 0 && left > 0) {
        long w = vx_write(audio, data, (size_t)left);
        if (w <= 0) {
            break;
        }
        data += w, left -= w;
    }
}

static void *decoder(void *arg) {
    (void)arg;
    long last = vx_uptime();
    for (;;) {
        long now = vx_uptime();
        if (!playing || !have_video) {
            last = now;
            vx_sleep(10);
            continue;
        }
        double elapsed = (now - last) / 1000.0;
        last = now;
        vx_mutex_lock(&lock);
        plm_decode(plm, elapsed > 0.1 ? 0.1 : elapsed);
        if (plm_has_ended(plm)) {
            playing = false;
            ended = true;
        }
        vx_mutex_unlock(&lock);
        vx_sleep(4);
    }
    return NULL;
}

static double now_time(void) {
    if (!have_video) {
        return 0;
    }
    vx_mutex_lock(&lock);
    double t = plm_get_time(plm);
    vx_mutex_unlock(&lock);
    return t;
}

static void seek(double t) {
    t = t < 0 ? 0 : duration && t > duration ? duration : t;
    vx_mutex_lock(&lock);
    if (audio >= 0) {
        vx_control(audio, VX_AUDIO_DROP, NULL, 0);
    }
    plm_seek(plm, t, 0);
    ended = false;
    vx_mutex_unlock(&lock);
}

static void toggle(void) {
    if (ended) {
        seek(0);
        playing = true;
    } else {
        playing = !playing;
        if (!playing && audio >= 0) {
            vx_control(audio, VX_AUDIO_DROP, NULL, 0);
        }
    }
    printf("videos: %s\n", playing ? "playing" : "paused");
    fflush(stdout);
}

/* Reads a whole file into memory (pl_mpeg decodes from there). */
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n > 0 ? malloc((size_t)n) : NULL;
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)(n > 0 ? n : 0);
    return data;
}

static void close_video(void) {
    vx_mutex_lock(&lock);
    playing = false;
    have_video = false;
    if (audio >= 0) {
        vx_control(audio, VX_AUDIO_DROP, NULL, 0);
    }
    if (plm) {
        plm_destroy(plm); /* (It frees the file's data.) */
        plm = NULL;
    }
    vx_mutex_unlock(&lock);
    vx_mutex_lock(&frame_lock);
    free(frame);
    frame = NULL;
    vx_mutex_unlock(&frame_lock);
}

static bool open_video(const char *path) {
    close_video();
    size_t size;
    file_data = read_file(path, &size);
    if (!file_data) {
        return false;
    }
    plm_t *p = plm_create_with_memory(file_data, size, 1);
    if (!p || !plm_probe(p, 5000 * 1024) || !plm_get_width(p)) {
        if (p) {
            plm_destroy(p);
        }
        return false;
    }
    vx_mutex_lock(&lock);
    plm = p;
    video_w = plm_get_width(plm);
    video_h = plm_get_height(plm);
    rate = plm_get_samplerate(plm);
    duration = plm_get_duration(plm);
    framerate = plm_get_framerate(plm);
    frame = calloc((size_t)video_w * video_h, 4);
    plm_set_video_decode_callback(plm, on_video, NULL);
    plm_set_audio_decode_callback(plm, on_audio, NULL);
    plm_set_audio_lead_time(plm, 0.15);
    if (rate && audio >= 0) {
        struct vx_audio_format format = {(unsigned)rate, 2};
        vx_control(audio, VX_AUDIO_SET_FORMAT, &format, sizeof(format));
    } else {
        plm_set_audio_enabled(plm, 0);
    }
    ended = false;
    frames = 0;
    have_video = frame != NULL;
    vx_mutex_unlock(&lock);
    const char *base = strrchr(path, '/');
    snprintf(name, sizeof(name), "%s", base ? base + 1 : path);
    printf("videos: %s (%dx%d, %.0f frames a second, %d Hz sound, %d:%02d)\n", name, video_w,
           video_h, framerate, rate, (int)duration / 60, (int)duration % 60);
    fflush(stdout);
    return have_video;
}

/* ---- The library ---- */

struct video {
    char path[256], title[128];
    double seconds;
    int width, height;
    uint32_t *thumb; /* THUMB_W x THUMB_H, or NULL. */
};

static struct video videos[MAX_VIDEOS];
static int video_count, library_top, hot_card = -1;
static bool in_library;

static bool is_video(const char *file) {
    size_t n = strlen(file);
    return (n > 4 && !strcasecmp(file + n - 4, ".mpg")) ||
           (n > 5 && !strcasecmp(file + n - 5, ".mpeg"));
}

static void thumb_frame(plm_t *p, plm_frame_t *f, void *user) {
    (void)p;
    *(plm_frame_t **)user = f;
}

/* A video's first picture (well: one a second in), its size and length. */
static void read_video(struct video *v) {
    size_t size;
    uint8_t *data = read_file(v->path, &size);
    if (!data) {
        return;
    }
    plm_t *p = plm_create_with_memory(data, size, 1);
    if (!p || !plm_probe(p, 5000 * 1024) || !plm_get_width(p)) {
        if (p) {
            plm_destroy(p);
        }
        return;
    }
    v->seconds = plm_get_duration(p);
    v->width = plm_get_width(p);
    v->height = plm_get_height(p);
    plm_set_audio_enabled(p, 0);
    plm_frame_t *f = NULL;
    plm_set_video_decode_callback(p, thumb_frame, &f);
    if (v->seconds > 2) {
        plm_seek(p, 1.0, 0);
    }
    for (int i = 0; i < 40 && !f; i++) {
        plm_decode(p, 0.05);
    }
    if (f) {
        uint32_t *rgb = malloc((size_t)v->width * v->height * 4);
        v->thumb = malloc(THUMB_W * THUMB_H * 4);
        if (rgb && v->thumb) {
            plm_frame_to_bgra(f, (uint8_t *)rgb, v->width * 4);
            /* Filling the card (cropped to its shape), averaged a little. */
            double scale = (double)THUMB_W / v->width > (double)THUMB_H / v->height
                               ? (double)THUMB_W / v->width
                               : (double)THUMB_H / v->height;
            int ox = (int)((v->width * scale - THUMB_W) / 2), oy = (int)((v->height * scale - THUMB_H) / 2);
            for (int y = 0; y < THUMB_H; y++) {
                for (int x = 0; x < THUMB_W; x++) {
                    int sx = (int)((x + ox) / scale), sy = (int)((y + oy) / scale);
                    sx = sx >= v->width ? v->width - 1 : sx;
                    sy = sy >= v->height ? v->height - 1 : sy;
                    v->thumb[y * THUMB_W + x] = rgb[sy * v->width + sx] & 0xffffff;
                }
            }
        } else {
            free(v->thumb);
            v->thumb = NULL;
        }
        free(rgb);
    }
    plm_destroy(p);
}

static void add_folder(const char *folder) {
    DIR *dir = opendir(folder);
    if (!dir) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && video_count < MAX_VIDEOS) {
        if (e->d_name[0] == '.' || !is_video(e->d_name)) {
            continue;
        }
        struct video *v = &videos[video_count++];
        memset(v, 0, sizeof(*v));
        snprintf(v->path, sizeof(v->path), "%s/%s", folder, e->d_name);
        snprintf(v->title, sizeof(v->title), "%s", e->d_name);
        char *dot = strrchr(v->title, '.');
        if (dot) {
            *dot = '\0';
        }
        read_video(v);
    }
    closedir(dir);
}

/* ---- Where things are ---- */

struct rect {
    int x, y, w, h;
};

static int W(void) {
    return window->surface.width;
}

static int H(void) {
    return window->surface.height;
}

static bool in(struct rect r, int x, int y) {
    return vx_inside(x, y, r.x, r.y, r.w, r.h);
}

enum part {
    P_NONE, P_PLAY, P_BACK10, P_FORWARD10, P_SCRUB, P_MUTE, P_VOLUME, P_FILL, P_LIBRARY,
    P_OPEN, P_BIG_PLAY,
};

static struct rect bar_rect(void) {
    int w = W() - 40 < 760 ? W() - 40 : 760;
    return (struct rect){(W() - w) / 2, H() - BAR_H - 18, w, BAR_H};
}

static struct rect part_rect(enum part p) {
    struct rect b = bar_rect();
    int cy = b.y + b.h / 2;
    switch (p) {
    case P_BACK10: return (struct rect){b.x + 14, cy - 15, 30, 30};
    case P_PLAY: return (struct rect){b.x + 50, cy - 21, 42, 42};
    case P_FORWARD10: return (struct rect){b.x + 98, cy - 15, 30, 30};
    case P_SCRUB: return (struct rect){b.x + 190, cy - 9, b.w - 190 - 260, 18};
    case P_MUTE: return (struct rect){b.x + b.w - 178, cy - 15, 26, 30};
    case P_VOLUME: return (struct rect){b.x + b.w - 146, cy - 9, 90, 18};
    case P_FILL: return (struct rect){b.x + b.w - 44, cy - 15, 30, 30};
    case P_LIBRARY: return (struct rect){16, 14, 104, 30};
    case P_OPEN: return (struct rect){W() - 24 - 110, 26, 110, 32};
    case P_BIG_PLAY: return (struct rect){W() / 2 - 44, H() / 2 - 44, 88, 88};
    default: return (struct rect){0, 0, 0, 0};
    }
}

static enum part hot = P_NONE;
static int hot_x = -1;
static long controls_until;
static bool fill_window, came_from_library;

static bool controls_shown(void) {
    return !playing || vx_uptime() < controls_until || hot != P_NONE;
}

/* ---- Drawing ---- */

static void text(struct vx_surface *s, const struct vx_font *f, int x, int y, const char *t,
                 uint32_t color) {
    vx_text(s, f, x, y, t, color, VX_TRANSPARENT);
}

static void text_fit(struct vx_surface *s, const struct vx_font *f, int x, int y, int w,
                     const char *t, uint32_t color) {
    if (vx_text_width_font(f, t) <= w) {
        text(s, f, x, y, t, color);
        return;
    }
    char cut[200];
    size_t n = vx_text_fit_bytes(f, t, w - vx_text_width_font(f, "..."));
    snprintf(cut, sizeof(cut), "%.*s...", (int)n, t);
    text(s, f, x, y, cut, color);
}

static const struct vx_font *sans(int size) {
    return vx_font(VX_FACE_SANS, size);
}

static const struct vx_font *bold(int size) {
    return vx_font(VX_FACE_BOLD, size);
}

static void picture_rect(int *x, int *y, int *w, int *h) {
    int ww = W(), wh = H();
    double sx = (double)ww / video_w, sy = (double)wh / video_h;
    double scale = fill_window ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    *w = (int)(video_w * scale);
    *h = (int)(video_h * scale);
    *x = (ww - *w) / 2;
    *y = (wh - *h) / 2;
}

/* The picture, scaled with bilinear filtering (16.16 fixed point). */
static void draw_picture(struct vx_surface *s) {
    int px, py, pw, ph;
    picture_rect(&px, &py, &pw, &ph);
    int x0 = px < 0 ? 0 : px, y0 = py < 0 ? 0 : py;
    int x1 = px + pw > W() ? W() : px + pw, y1 = py + ph > H() ? H() : py + ph;
    vx_fill(s, 0, 0, W(), y0, 0);
    vx_fill(s, 0, y1, W(), H() - y1, 0);
    vx_fill(s, 0, y0, x0, y1 - y0, 0);
    vx_fill(s, x1, y0, W() - x1, y1 - y0, 0);
    vx_mutex_lock(&frame_lock);
    if (!frame) {
        vx_mutex_unlock(&frame_lock);
        return;
    }
    long step_x = ((long)video_w << 16) / pw, step_y = ((long)video_h << 16) / ph;
    for (int y = y0; y < y1; y++) {
        long fy = (long)(y - py) * step_y + step_y / 2 - 32768;
        fy = fy < 0 ? 0 : fy;
        int sy = (int)(fy >> 16), sy2 = sy + 1 < video_h ? sy + 1 : sy;
        unsigned wy = (unsigned)(fy >> 8) & 255;
        const uint32_t *r1 = frame + (long)sy * video_w, *r2 = frame + (long)sy2 * video_w;
        uint32_t *to = s->pixels + (long)y * s->stride;
        long fx = (long)(x0 - px) * step_x + step_x / 2 - 32768;
        for (int x = x0; x < x1; x++, fx += step_x) {
            long cx = fx < 0 ? 0 : fx;
            int sx = (int)(cx >> 16), sx2 = sx + 1 < video_w ? sx + 1 : sx;
            unsigned wx = (unsigned)(cx >> 8) & 255;
            uint32_t a = r1[sx], b = r1[sx2], c = r2[sx], d = r2[sx2];
            /* Red and blue together, then green. */
            uint32_t rb_top = ((a & 0xff00ff) * (256 - wx) + (b & 0xff00ff) * wx) >> 8 & 0xff00ff;
            uint32_t rb_bot = ((c & 0xff00ff) * (256 - wx) + (d & 0xff00ff) * wx) >> 8 & 0xff00ff;
            uint32_t g_top = ((a & 0xff00) * (256 - wx) + (b & 0xff00) * wx) >> 8 & 0xff00;
            uint32_t g_bot = ((c & 0xff00) * (256 - wx) + (d & 0xff00) * wx) >> 8 & 0xff00;
            uint32_t rb = (rb_top * (256 - wy) + rb_bot * wy) >> 8 & 0xff00ff;
            uint32_t g = (g_top * (256 - wy) + g_bot * wy) >> 8 & 0xff00;
            to[x] = rb | g;
        }
    }
    new_frame = false;
    vx_mutex_unlock(&frame_lock);
}

/* Glass over the picture: dark, tinted with the accent, a sheen on top. */
static void glass(struct vx_surface *s, struct rect r, int radius) {
    vx_fill_rounded(s, r.x + 2, r.y + 4, r.w, r.h, radius, 0x000000, 70);
    vx_fill_rounded(s, r.x, r.y, r.w, r.h, radius, vx_mix(0x10121c, VX_COLOR_ACCENT, 40), 200);
    vx_fill_rounded(s, r.x + 1, r.y + 1, r.w - 2, r.h / 2, radius - 1, 0xffffff, 22);
    vx_fill_rounded(s, r.x + radius, r.y, r.w - 2 * radius, 1, 0, 0xffffff, 70);
}

/* A round "10" with an arrow: ten seconds back or on. */
static void skip10(struct vx_surface *s, struct rect r, bool back) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (hot == (back ? P_BACK10 : P_FORWARD10)) {
        lk_circle(s, cx, cy, 15, 0xffffff, 40);
    }
    lk_circle(s, cx, cy, 11, 0xffffff, 255);
    lk_circle(s, cx, cy, 9, vx_mix(0x10121c, VX_COLOR_ACCENT, 40), 255);
    /* A gap at the top, with an arrowhead. */
    vx_fill(s, back ? cx - 6 : cx, cy - 12, 7, 5, vx_mix(0x10121c, VX_COLOR_ACCENT, 40));
    lk_triangle(s, back ? cx - 1 : cx + 1, cy - 10, 6, back, 0xffffff);
    lk_centered(s, bold(10), cx, cy - 6, "10", 0xffffff);
}

static void draw_controls(struct vx_surface *s) {
    /* The top: a shade, the way back, the name. */
    for (int row = 0; row < TOP_H + 20; row++) {
        vx_fill_rounded(s, 0, row, W(), 1, 0, 0x000000, (TOP_H + 20 - row) * 150 / (TOP_H + 20));
    }
    if (came_from_library) {
        struct rect r = part_rect(P_LIBRARY);
        glass(s, r, 15);
        if (hot == P_LIBRARY) {
            vx_fill_rounded(s, r.x, r.y, r.w, r.h, 15, 0xffffff, 30);
        }
        lk_triangle(s, r.x + 18, r.y + 15, 10, true, 0xffffff);
        text(s, bold(13), r.x + 28, r.y + 7, "Library", 0xffffff);
    }
    int nx = came_from_library ? 136 : 20;
    text_fit(s, bold(15), nx, 18, W() - nx - 20, name, 0xffffff);
    char info[64];
    snprintf(info, sizeof(info), "%dx%d  -  %.0f fps", video_w, video_h, framerate);
    text(s, sans(12), nx, 38, info, 0xc8cce0);

    struct rect b = bar_rect();
    glass(s, b, 20);
    skip10(s, part_rect(P_BACK10), true);
    skip10(s, part_rect(P_FORWARD10), false);
    struct rect p = part_rect(P_PLAY);
    vx_draw_gel(s, p.x, p.y, p.w, p.h, p.w / 2,
                hot == P_PLAY ? vx_mix(VX_COLOR_ACCENT, 0xffffff, 50) : VX_COLOR_ACCENT);
    lk_play_sign(s, p.x + p.w / 2 + (playing ? 0 : 2), p.y + p.h / 2, 16, playing, 0xffffff);
    /* The times and the scrubber. */
    struct rect sc = part_rect(P_SCRUB);
    double t = now_time();
    char elapsed[16], left[20], rest[16];
    lk_time(elapsed, sizeof(elapsed), t);
    lk_time(rest, sizeof(rest), duration - t);
    snprintf(left, sizeof(left), "-%s", rest);
    const struct vx_font *f = sans(12);
    text(s, f, sc.x - 14 - vx_text_width_font(f, elapsed), sc.y + 1, elapsed, 0xffffff);
    text(s, f, sc.x + sc.w + 12, sc.y + 1, left, 0xffffff);
    lk_track(s, sc.x, sc.y + sc.h / 2, sc.w, duration > 0 ? t / duration : 0, hot == P_SCRUB, true);
    if (hot == P_SCRUB && hot_x >= 0 && duration > 0) {
        char there[16];
        lk_time(there, sizeof(there), (double)(hot_x - sc.x) / sc.w * duration);
        int tw = vx_text_width_font(f, there) + 14;
        struct rect tip = {hot_x - tw / 2, b.y - 30, tw, 22};
        glass(s, tip, 8);
        text(s, f, tip.x + 7, tip.y + 3, there, 0xffffff);
    }
    /* The volume, and fit or fill. */
    struct rect m = part_rect(P_MUTE), v = part_rect(P_VOLUME), fl = part_rect(P_FILL);
    lk_speaker_sign(s, m.x + 2, m.y + 15, volume, 0xffffff);
    lk_track(s, v.x, v.y + v.h / 2, v.w, volume / 100.0, hot == P_VOLUME, true);
    if (hot == P_FILL) {
        lk_circle(s, fl.x + 15, fl.y + 15, 15, 0xffffff, 40);
    }
    /* Four corners: out (to fill) or in (to fit). */
    int c = fl.x + 15, cy = fl.y + 15, o = fill_window ? 3 : 7, in_ = fill_window ? 7 : 3;
    (void)in_;
    for (int dx = -1; dx <= 1; dx += 2) {
        for (int dy = -1; dy <= 1; dy += 2) {
            int x = c + dx * o, y = cy + dy * o;
            vx_fill(s, dx < 0 ? x - 4 : x, y - 1, 5, 2, 0xffffff);
            vx_fill(s, x - 1, dy < 0 ? y - 4 : y, 2, 5, 0xffffff);
        }
    }
}

static void draw_player(struct vx_surface *s) {
    draw_picture(s);
    if (!playing) { /* A big play button in the middle. */
        struct rect r = part_rect(P_BIG_PLAY);
        lk_circle(s, r.x + 44 + 2, r.y + 48, 44, 0x000000, 70);
        lk_circle(s, r.x + 44, r.y + 44, 44, 0xffffff, hot == P_BIG_PLAY ? 90 : 55);
        vx_draw_gel(s, r.x + 8, r.y + 8, 72, 72, 36, VX_COLOR_ACCENT);
        lk_play_sign(s, r.x + 47, r.y + 44, 28, false, 0xffffff);
        if (ended) {
            lk_centered(s, bold(14), W() / 2, r.y + r.h + 12, "Play again", 0xffffff);
        }
    }
    if (controls_shown()) {
        draw_controls(s);
    }
}

#define CARD_GAP 22
#define CARD_TEXT 52

static int cards_per_row(void) {
    int n = (W() - 48 + CARD_GAP) / (THUMB_W + CARD_GAP);
    return n < 1 ? 1 : n;
}

static struct rect card_rect(int i) {
    int per_row = cards_per_row(), k = i - library_top;
    int total = per_row * THUMB_W + (per_row - 1) * CARD_GAP;
    int x0 = (W() - total) / 2;
    return (struct rect){x0 + (k % per_row) * (THUMB_W + CARD_GAP),
                         100 + (k / per_row) * (THUMB_H + CARD_TEXT + CARD_GAP), THUMB_W,
                         THUMB_H + CARD_TEXT};
}

static void draw_library(struct vx_surface *s) {
    uint32_t top, bottom;
    lk_sidebar_colors(&top, &bottom);
    lk_gradient(s, 0, 0, W(), H(), top, bottom);
    for (int row = 0; row < 120; row++) {
        vx_fill_rounded(s, 0, row, W(), 1, 0, 0xffffff, (120 - row) * 14 / 120);
    }
    lk_logo(s, 24, 20, 46, true);
    text(s, bold(22), 84, 22, "Videos", 0xffffff);
    char count[48];
    snprintf(count, sizeof(count), "%d video%s", video_count, video_count == 1 ? "" : "s");
    text(s, sans(12), 84, 50, count, 0xc8cce0);
    struct rect o = part_rect(P_OPEN);
    vx_draw_gel(s, o.x, o.y, o.w, o.h, o.h / 2,
                hot == P_OPEN ? vx_mix(VX_COLOR_ACCENT, 0xffffff, 50) : VX_COLOR_ACCENT);
    lk_centered(s, bold(13), o.x + o.w / 2, o.y + 8, "Open...", 0xffffff);
    if (!video_count) {
        lk_film(s, W() / 2 - 40, H() / 2 - 90, 80, 0x8a8fa8);
        lk_centered(s, bold(16), W() / 2, H() / 2, "No videos yet", 0xffffff);
        lk_centered(s, sans(13), W() / 2, H() / 2 + 26,
                    "Put MPEG-1 videos (.mpg) in your Videos folder, or Open... one.", 0xb8bcd0);
        return;
    }
    for (int i = library_top; i < video_count; i++) {
        struct rect r = card_rect(i);
        if (r.y > H()) {
            break;
        }
        const struct video *v = &videos[i];
        bool lit = i == hot_card;
        if (lit) {
            vx_fill_rounded(s, r.x - 6, r.y - 6, r.w + 12, r.h + 8, 16, VX_COLOR_ACCENT, 70);
        }
        vx_fill_rounded(s, r.x + 2, r.y + 5, THUMB_W, THUMB_H, 12, 0x000000, 80);
        if (v->thumb) {
            /* The picture, with round corners. */
            for (int y = 0; y < THUMB_H; y++) {
                int inset = 0, coverage;
                if (y < 12) {
                    inset = vx_corner_inset(12, y, &coverage);
                } else if (y >= THUMB_H - 12) {
                    inset = vx_corner_inset(12, THUMB_H - 1 - y, &coverage);
                }
                if (r.y + y < 0 || r.y + y >= H()) {
                    continue;
                }
                for (int x = inset; x < THUMB_W - inset; x++) {
                    int px = r.x + x;
                    if (px >= 0 && px < W()) {
                        s->pixels[(long)(r.y + y) * s->stride + px] = v->thumb[y * THUMB_W + x];
                    }
                }
            }
            vx_fill_rounded(s, r.x, r.y, THUMB_W, THUMB_H / 2, 12, 0xffffff, 18);
        } else {
            lk_art(s, r.x + (THUMB_W - THUMB_H) / 2, r.y, THUMB_H, 12, v->title);
        }
        char length[16];
        lk_time(length, sizeof(length), v->seconds);
        const struct vx_font *f = bold(11);
        int lw = vx_text_width_font(f, length) + 12;
        vx_fill_rounded(s, r.x + THUMB_W - lw - 8, r.y + THUMB_H - 26, lw, 18, 9, 0x000000, 170);
        text(s, f, r.x + THUMB_W - lw - 2, r.y + THUMB_H - 24, length, 0xffffff);
        if (lit) {
            lk_circle(s, r.x + THUMB_W / 2, r.y + THUMB_H / 2, 28, 0xffffff, 70);
            vx_draw_gel(s, r.x + THUMB_W / 2 - 22, r.y + THUMB_H / 2 - 22, 44, 44, 22, VX_COLOR_ACCENT);
            lk_play_sign(s, r.x + THUMB_W / 2 + 2, r.y + THUMB_H / 2, 16, false, 0xffffff);
        }
        text_fit(s, bold(14), r.x + 2, r.y + THUMB_H + 10, THUMB_W - 4, v->title, 0xffffff);
        char details[64];
        snprintf(details, sizeof(details), "%dx%d", v->width, v->height);
        text(s, sans(12), r.x + 2, r.y + THUMB_H + 30, details, 0xa8acc0);
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    if (in_library) {
        draw_library(s);
    } else {
        draw_player(s);
    }
    vx_window_present(window, 0, 0, W(), H());
}

/* ---- Input ---- */

static void set_title(void) {
    char title[240];
    if (in_library) {
        snprintf(title, sizeof(title), "Videos");
    } else {
        snprintf(title, sizeof(title), "%s - Videos", name);
    }
    vx_window_set_title(window, title);
}

static void save_volume(void) {
    struct vx_settings st;
    vx_settings_load(&st, "videos.conf");
    vx_settings_set_int(&st, "volume", volume);
    vx_settings_save(&st);
}

static void set_volume(int v) {
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
}

static void play_file(const char *path, bool from_library) {
    if (!open_video(path)) {
        fprintf(stderr, "videos: %s isn't an MPEG-1 video this can play\n", path);
        return;
    }
    in_library = false;
    came_from_library = from_library;
    playing = true;
    controls_until = vx_uptime() + CONTROLS_MS;
    set_title();
}

static void back_to_library(void) {
    close_video();
    in_library = true;
    set_title();
}

static void open_dialog(void) {
    char chosen[256];
    char folder[300];
    vx_home_path(folder, sizeof(folder), "Videos");
    struct vx_stat st;
    if (vx_stat(folder, &st) != 0) {
        snprintf(folder, sizeof(folder), "/share/videos");
    }
    if (vx_open_dialog("Open a Video", folder, chosen, sizeof(chosen))) {
        play_file(chosen, true);
    }
}

static enum part part_at(int x, int y) {
    if (in_library) {
        return in(part_rect(P_OPEN), x, y) ? P_OPEN : P_NONE;
    }
    if (!playing && in(part_rect(P_BIG_PLAY), x, y)) {
        return P_BIG_PLAY;
    }
    if (!controls_shown() && !in(bar_rect(), x, y)) {
        return P_NONE;
    }
    static const enum part parts[] = {P_PLAY, P_BACK10, P_FORWARD10, P_SCRUB, P_MUTE,
                                      P_VOLUME, P_FILL, P_LIBRARY};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        struct rect r = part_rect(parts[i]);
        if (parts[i] == P_LIBRARY && !came_from_library) {
            continue;
        }
        if (parts[i] == P_SCRUB || parts[i] == P_VOLUME) {
            r.x -= 8, r.w += 16, r.y -= 8, r.h += 16;
        }
        if (in(r, x, y)) {
            return parts[i];
        }
    }
    return P_NONE;
}

static int card_at(int x, int y) {
    if (!in_library) {
        return -1;
    }
    for (int i = library_top; i < video_count; i++) {
        if (in(card_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

static void press(enum part p, int x) {
    switch (p) {
    case P_PLAY:
    case P_BIG_PLAY: toggle(); break;
    case P_BACK10: seek(now_time() - 10); break;
    case P_FORWARD10: seek(now_time() + 10); break;
    case P_SCRUB: {
        struct rect r = part_rect(P_SCRUB);
        seek((double)(x - r.x) / r.w * duration);
        break;
    }
    case P_VOLUME: {
        struct rect r = part_rect(P_VOLUME);
        set_volume((x - r.x) * 100 / r.w);
        break;
    }
    case P_MUTE: {
        static int before = 100;
        if (volume) {
            before = volume;
            set_volume(0);
        } else {
            set_volume(before ? before : 100);
        }
        save_volume();
        break;
    }
    case P_FILL: fill_window = !fill_window; break;
    case P_LIBRARY: back_to_library(); break;
    case P_OPEN: open_dialog(); break;
    default: break;
    }
}

static void pointer(const struct vx_gui_event *e, int *held) {
    static enum part dragging = P_NONE;
    static long last_click;
    if (!in_library) {
        controls_until = vx_uptime() + CONTROLS_MS;
    }
    hot = part_at(e->x, e->y);
    hot_card = card_at(e->x, e->y);
    hot_x = e->x;
    bool down = (e->buttons & 1) && !(*held & 1), up = !(e->buttons & 1) && (*held & 1);
    *held = e->buttons;
    if (in_library && e->wheel) {
        library_top -= e->wheel * cards_per_row();
        int last = video_count - cards_per_row();
        library_top = library_top > last ? last : library_top;
        library_top = library_top < 0 ? 0 : library_top / cards_per_row() * cards_per_row();
    }
    if (dragging != P_NONE) {
        if (e->buttons & 1) {
            press(dragging, e->x);
        } else if (up) {
            if (dragging == P_VOLUME) {
                save_volume();
            }
            dragging = P_NONE;
        }
        return;
    }
    if (!down) {
        return;
    }
    if (hot == P_SCRUB || hot == P_VOLUME) {
        dragging = hot;
        press(hot, e->x);
        return;
    }
    if (hot != P_NONE) {
        press(hot, e->x);
        return;
    }
    if (hot_card >= 0) {
        play_file(videos[hot_card].path, true);
        return;
    }
    if (!in_library && !in(bar_rect(), e->x, e->y)) {
        /* A click on the picture plays or pauses; a double click fits or fills. */
        long t = vx_uptime();
        if (t - last_click < 400) {
            fill_window = !fill_window;
            toggle(); /* (Undoing the first click's.) */
        } else {
            toggle();
        }
        last_click = t;
    }
}

static void key(const struct vx_gui_event *e) {
    if (!e->value) {
        return;
    }
    if (in_library) {
        if (e->key == VX_KEY_ENTER && video_count) {
            play_file(videos[hot_card >= 0 ? hot_card : 0].path, true);
        }
        return;
    }
    controls_until = vx_uptime() + CONTROLS_MS;
    switch (e->key) {
    case VX_KEY_LEFT: seek(now_time() - 5); break;
    case VX_KEY_RIGHT: seek(now_time() + 5); break;
    case VX_KEY_UP: set_volume(volume + 10); save_volume(); break;
    case VX_KEY_DOWN: set_volume(volume - 10); save_volume(); break;
    case VX_KEY_HOME: seek(0); break;
    case VX_KEY_ESC:
        if (came_from_library) {
            back_to_library();
        }
        break;
    default:
        if (e->character == ' ') {
            toggle();
        } else if (e->character == 'f' || e->character == 'F') {
            fill_window = !fill_window;
        }
        break;
    }
}

/* ---- Starting ---- */

int main(int argc, char **argv) {
    audio = vx_open("/dev/audio0", VX_OPEN_WRITE);
    bool headless = argc == 3 && !strcmp(argv[1], "--play");
    vx_thread_create(decoder, NULL);
    if (headless) {
        if (!open_video(argv[2])) {
            fprintf(stderr, "videos: %s isn't an MPEG-1 video this can play\n", argv[2]);
            return 1;
        }
        playing = true;
        while (!ended) {
            vx_sleep(50);
        }
        if (audio >= 0) {
            vx_control(audio, VX_AUDIO_DRAIN, NULL, 0);
        }
        printf("videos: %ld frames, done\n", frames);
        return 0;
    }
    struct vx_settings st;
    vx_settings_load(&st, "videos.conf");
    set_volume(vx_settings_int(&st, "volume", 100));
    int w = 880, h = 560;
    bool start_playing = argc >= 2;
    if (start_playing) {
        if (!open_video(argv[1])) {
            fprintf(stderr, "videos: %s isn't an MPEG-1 video this can play\n", argv[1]);
            return 1;
        }
        w = video_w < 640 ? 640 : video_w > 1100 ? 1100 : video_w;
        h = video_h * w / video_w;
    }
    window = vx_window_create_flags("Videos", w, h, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "videos: no desktop to open a window on\n");
        return 1;
    }
    char folder[300];
    vx_home_path(folder, sizeof(folder), "Videos");
    add_folder(folder);
    add_folder("/share/videos");
    printf("videos: %d in the library\n", video_count);
    fflush(stdout);
    if (start_playing) {
        in_library = false;
        playing = true;
        controls_until = vx_uptime() + CONTROLS_MS;
    } else {
        in_library = true;
    }
    set_title();
    int held = 0;
    bool shown_before = true;
    for (;;) {
        bool shown = !in_library && controls_shown();
        if (in_library || new_frame || shown || shown != shown_before) {
            draw();
        }
        shown_before = shown;
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, !in_library && playing ? 15 : in_library ? 1000 : 250);
        if (got < 0) {
            return 0;
        }
        while (got > 0) {
            switch (e.type) {
            case VX_GUI_CLOSE:
                close_video();
                vx_window_destroy(window);
                return 0;
            case VX_GUI_KEY: key(&e); break;
            case VX_GUI_POINTER: pointer(&e, &held); break;
            case VX_GUI_RESIZE:
                if (e.width >= 480 && e.height >= 300) {
                    vx_window_resize(window, e.width, e.height);
                }
                break;
            case VX_GUI_DROP: {
                char *paths = vx_drop_paths(&e);
                char *first = paths ? strtok(paths, "\n") : NULL;
                if (first && is_video(first)) {
                    play_file(first, true);
                }
                free(paths);
                break;
            }
            default:
                break;
            }
            got = vx_gui_wait(&e, 0);
        }
    }
}
