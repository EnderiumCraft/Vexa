/* Videos: plays MPEG-1 videos (.mpg, with MP2 sound), like QuickTime: the
 * picture fills the window (its shape kept), and a dark glass bar with
 * Play/Pause, the time and a scrubber shows while the pointer moves.
 *
 *   videos [file]         the window (without a file: the Open dialog)
 *   videos --play file    plays it without a window, and says what it is
 *
 * Keys: Space plays or pauses, Left/Right go 5 seconds back or on, Home
 * goes back to the start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../third_party/media/pl_mpeg.h"
#pragma GCC diagnostic pop

#define BAR_W 420
#define BAR_H 46
#define BAR_SHOWN_MS 2500

static struct vx_window *window;
static plm_t *plm;
static char name[200];
static int video_w, video_h, rate;
static double duration;
static int audio = -1;

static struct vx_mutex lock = VX_MUTEX_INIT;   /* Around plm (seeking, decoding). */
static struct vx_mutex frame_lock = VX_MUTEX_INIT;
static uint32_t *frame;           /* The newest picture (video_w x video_h). */
static volatile bool new_frame, playing, ended;
static volatile long frames;
static long bar_until;            /* When the bar goes (ms since start). */
static bool hot_play;

/* ---- Decoding (a thread: pictures into `frame`, sound to /dev/audio0) ---- */

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
    for (unsigned i = 0; i < s->count * 2; i++) {
        float v = s->interleaved[i] * 32767.0f;
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
        if (!playing) {
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

static bool open_video(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return false;
    }
    fclose(f);
    plm = plm_create_with_memory(data, (size_t)size, 1);
    if (!plm || !plm_probe(plm, 5000 * 1024) || !plm_get_width(plm)) {
        return false;
    }
    video_w = plm_get_width(plm);
    video_h = plm_get_height(plm);
    rate = plm_get_samplerate(plm);
    duration = plm_get_duration(plm);
    frame = calloc((size_t)video_w * video_h, 4);
    if (!frame) {
        return false;
    }
    plm_set_video_decode_callback(plm, on_video, NULL);
    plm_set_audio_decode_callback(plm, on_audio, NULL);
    plm_set_audio_lead_time(plm, 0.15);
    if (rate && audio >= 0) {
        struct vx_audio_format format = {(unsigned)rate, 2};
        vx_control(audio, VX_AUDIO_SET_FORMAT, &format, sizeof(format));
    } else {
        plm_set_audio_enabled(plm, 0);
    }
    const char *base = strrchr(path, '/');
    snprintf(name, sizeof(name), "%s", base ? base + 1 : path);
    printf("videos: %s (%dx%d, %.0f frames a second, %d Hz sound, %d:%02d)\n", name, video_w,
           video_h, plm_get_framerate(plm), rate, (int)duration / 60, (int)duration % 60);
    fflush(stdout);
    return true;
}

/* ---- Drawing ---- */

static void picture_rect(int *x, int *y, int *w, int *h) {
    int ww = window->surface.width, wh = window->surface.height;
    *w = ww;
    *h = video_w ? video_h * ww / video_w : wh;
    if (*h > wh) {
        *h = wh;
        *w = video_h ? video_w * wh / video_h : ww;
    }
    *x = (ww - *w) / 2;
    *y = (wh - *h) / 2;
}

static void bar_rect(int *x, int *y, int *w) {
    int ww = window->surface.width, wh = window->surface.height;
    *w = ww - 32 < BAR_W ? ww - 32 : BAR_W;
    *x = (ww - *w) / 2;
    *y = wh - BAR_H - 16;
}

static void time_text(char *out, size_t size, double t) {
    int s = t < 0 ? 0 : (int)t;
    snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int ww = s->width, wh = s->height;
    int px, py, pw, ph;
    picture_rect(&px, &py, &pw, &ph);
    /* Black around the picture, the picture scaled (nearest pixel: quick). */
    vx_fill(s, 0, 0, ww, py, 0);
    vx_fill(s, 0, py + ph, ww, wh - py - ph, 0);
    vx_fill(s, 0, py, px, ph, 0);
    vx_fill(s, px + pw, py, ww - px - pw, ph, 0);
    vx_mutex_lock(&frame_lock);
    for (int y = 0; y < ph; y++) {
        const uint32_t *from = frame + (long)(y * video_h / ph) * video_w;
        uint32_t *to = s->pixels + (long)(py + y) * s->stride + px;
        long step = ((long)video_w << 16) / pw, at = 0;
        for (int x = 0; x < pw; x++, at += step) {
            to[x] = from[at >> 16] & 0xffffff;
        }
    }
    new_frame = false;
    vx_mutex_unlock(&frame_lock);
    /* The bar: dark glass, rounded; Play/Pause, the time, a scrubber. */
    if (!playing || vx_uptime() < bar_until) {
        int bx, by, bw;
        bar_rect(&bx, &by, &bw);
        vx_fill_rounded(s, bx, by, bw, BAR_H, 12, 0x101018, 175);
        vx_fill_rounded(s, bx + 1, by + 1, bw - 2, BAR_H / 2, 11, 0xffffff, 26);
        int cx = bx + 26, cy = by + BAR_H / 2;
        vx_fill_rounded(s, cx - 14, cy - 14, 28, 28, 14, 0xffffff, hot_play ? 60 : 30);
        if (playing) {
            vx_fill(s, cx - 6, cy - 7, 4, 14, 0xffffff);
            vx_fill(s, cx + 2, cy - 7, 4, 14, 0xffffff);
        } else {
            for (int i = 0; i < 8; i++) {
                vx_fill(s, cx - 4 + i, cy - 8 + i, 1, 16 - 2 * i, 0xffffff);
            }
        }
        double t = now_time();
        char elapsed[16], left[20], rest[16];
        time_text(elapsed, sizeof(elapsed), t);
        time_text(rest, sizeof(rest), duration - t);
        snprintf(left, sizeof(left), "-%s", rest);
        int sx = bx + 104, sw = bw - 104 - 64;
        vx_draw_text(s, bx + 50, cy - 8, elapsed, 0xffffff, VX_TRANSPARENT);
        vx_draw_text(s, sx + sw + 10, cy - 8, left, 0xffffff, VX_TRANSPARENT);
        vx_fill_rounded(s, sx, cy - 3, sw, 6, 3, 0xffffff, 60);
        int at = duration > 0 ? (int)(sw * (t / duration)) : 0;
        at = at > sw ? sw : at;
        vx_fill_rounded(s, sx, cy - 3, at, 6, 3, 0xffffff, 230);
        vx_fill_rounded(s, sx + at - 6, cy - 6, 12, 12, 6, 0xffffff, 255);
    }
    vx_window_present(window, 0, 0, ww, wh);
}

/* ---- Input ---- */

static void pointer(const struct vx_gui_event *e, int *held) {
    bar_until = vx_uptime() + BAR_SHOWN_MS;
    int bx, by, bw;
    bar_rect(&bx, &by, &bw);
    hot_play = vx_inside(e->x, e->y, bx + 8, by + 4, 40, BAR_H - 8);
    bool down = (e->buttons & 1) && !(*held & 1);
    bool dragging = (e->buttons & 1) && (*held & 1);
    *held = e->buttons;
    int sx = bx + 104, sw = bw - 104 - 64;
    if ((down || dragging) && vx_inside(e->x, e->y, sx - 8, by, sw + 16, BAR_H)) {
        seek((double)(e->x - sx) / sw * duration);
    } else if (down && hot_play) {
        toggle();
    } else if (down && !vx_inside(e->x, e->y, bx, by, bw, BAR_H)) {
        toggle(); /* (A click on the picture.) */
    }
}

static void key(const struct vx_gui_event *e) {
    if (!e->value) {
        return;
    }
    bar_until = vx_uptime() + BAR_SHOWN_MS;
    if (e->key == VX_KEY_LEFT) {
        seek(now_time() - 5);
    } else if (e->key == VX_KEY_RIGHT) {
        seek(now_time() + 5);
    } else if (e->key == VX_KEY_HOME) {
        seek(0);
    } else if (e->character == ' ') {
        toggle();
    }
}

/* ---- Starting ---- */

int main(int argc, char **argv) {
    audio = vx_open("/dev/audio0", VX_OPEN_WRITE);
    bool headless = argc == 3 && !strcmp(argv[1], "--play");
    char chosen[256] = "";
    if (argc >= 2) {
        snprintf(chosen, sizeof(chosen), "%s", argv[headless ? 2 : 1]);
    } else if (!vx_open_dialog("Open a Video", "/share/videos", chosen, sizeof(chosen))) {
        return 0;
    }
    if (!open_video(chosen)) {
        fprintf(stderr, "videos: %s isn't an MPEG-1 video this can play\n", chosen);
        return 1;
    }
    vx_thread_create(decoder, NULL);
    playing = true;
    if (headless) {
        while (!ended) {
            vx_sleep(50);
        }
        if (audio >= 0) {
            vx_control(audio, VX_AUDIO_DRAIN, NULL, 0);
        }
        printf("videos: %ld frames, done\n", frames);
        return 0;
    }
    int w = video_w < 480 ? 480 : video_w > 1100 ? 1100 : video_w;
    int h = video_w ? video_h * w / video_w : 360;
    char title[240];
    snprintf(title, sizeof(title), "%s - Videos", name);
    window = vx_window_create_flags(title, w, h, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "videos: no desktop to open a window on\n");
        return 1;
    }
    bar_until = vx_uptime() + BAR_SHOWN_MS;
    int held = 0;
    bool bar_was_shown = true;
    for (;;) {
        bool bar_shown = !playing || vx_uptime() < bar_until;
        if (new_frame || bar_shown || bar_shown != bar_was_shown) {
            draw();
        }
        bar_was_shown = bar_shown;
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, playing ? 15 : 250);
        if (got < 0) {
            return 0;
        }
        if (got == 0) {
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            playing = false;
            if (audio >= 0) {
                vx_control(audio, VX_AUDIO_DROP, NULL, 0);
            }
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_RESIZE:
            if (e.width >= 240 && e.height >= 160) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        default:
            break;
        }
    }
}
