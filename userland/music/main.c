/* Music: plays MP3, Ogg Vorbis, FLAC and WAV files.
 *
 * On the left, Vexa's dark sidebar: the library (Songs, Artists, Albums),
 * the folders it reads (~/Music and /share/music) and what's playing. Up
 * top, the song that's playing: its cover (made up from its album's name),
 * what it is, and a live spectrum of the sound. Below, the songs (or the
 * artists and albums as cards, which pick the songs shown), with a search
 * field; along the bottom a glass bar with shuffle, back, play, next,
 * repeat, the scrubber and this app's volume (kept in its settings).
 * Files dropped on it join the list.
 *
 *   music [file...]       the window, playing the first file given
 *   music --play file     plays it without a window, and says what it is
 *
 * Keys: Space plays or pauses, Return plays the selected song, Up/Down
 * select, Left/Right go 5 seconds back or on, Ctrl+Left/Right the song
 * before or after, Ctrl+F searches, +/- the volume, Delete takes a song
 * out of the list. */
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
#include "decode.h"
#include "look.h"

#define WIDTH 960
#define HEIGHT 620
#define SIDEBAR 220
#define HERO 168      /* The playing song's banner. */
#define BAR 76        /* The transport along the bottom. */
#define HEADER 34     /* The list's view title and search. */
#define ROW 30
#define MAX_TRACKS 256
#define BANDS 28

struct track {
    char path[256];
    struct decoder_info info;
    bool known; /* (Its info has been read.) */
};

static struct vx_window *window;
static struct track tracks[MAX_TRACKS];
static int track_count, selected = -1, top;
static bool shuffle, repeat;

/* What's shown: the songs (of an artist or album, or matching the search),
 * or the artists or albums as cards. */
enum view { V_SONGS, V_ARTISTS, V_ALBUMS };
static enum view view = V_SONGS;
static char filter_artist[128], filter_album[128], search[64];
static bool searching; /* Typing goes into the search field. */
static int shown[MAX_TRACKS], shown_count;
static char groups[MAX_TRACKS][128];
static int group_first[MAX_TRACKS], group_size[MAX_TRACKS], group_count, group_top;

/* ---- Playing (a thread decodes and writes; this side asks it to) ---- */

enum state { STOPPED, PLAYING, PAUSED };
static struct vx_mutex lock = VX_MUTEX_INIT;
static struct decoder *decoder;
static struct decoder_info playing_info;
static volatile int playing = -1; /* The track, or -1. */
static volatile enum state state = STOPPED;
static volatile bool ended;       /* The song came to its end. */
static double seek_to = -1;       /* (Under the lock.) */
static double start_at;           /* Where the frames written start, in seconds. */
static unsigned long long written; /* Frames written since then. */
static int audio = -1;
static volatile int volume = 100; /* This app's, 0 to 100. */
/* The latest sound, mono, for the spectrum: frame `n` is at ring[n % RING]. */
#define RING 32768
static int16_t ring[RING];
static unsigned long long ring_frames;
static float bands[BANDS], peaks[BANDS];

static void stop_sound(void) {
    if (audio >= 0) {
        vx_control(audio, VX_AUDIO_DROP, NULL, 0);
    }
}

/* What's been heard so far, in frames since start_at, and in seconds. */
static long long heard_frames(void) {
    unsigned delay = 0;
    if (state == PLAYING && audio >= 0) {
        vx_control(audio, VX_AUDIO_DELAY, &delay, sizeof(delay));
    }
    long long heard = (long long)written - (long long)delay;
    return heard > 0 ? heard : 0;
}

static double position(void) {
    vx_mutex_lock(&lock);
    double at = start_at;
    if (decoder && playing_info.rate) {
        at += (double)heard_frames() / playing_info.rate;
    }
    vx_mutex_unlock(&lock);
    return at;
}

static void *player(void *arg) {
    (void)arg;
    static int16_t buffer[2048 * 2];
    for (;;) {
        vx_mutex_lock(&lock);
        if (state != PLAYING || !decoder) {
            vx_mutex_unlock(&lock);
            vx_sleep(20);
            continue;
        }
        if (seek_to >= 0) {
            decoder_seek(decoder, seek_to, playing_info.rate, playing_info.channels);
            start_at = seek_to;
            written = 0;
            ring_frames = 0;
            seek_to = -1;
            stop_sound();
        }
        unsigned channels = playing_info.channels;
        int n = decoder_read(decoder, buffer, 2048, channels);
        vx_mutex_unlock(&lock);
        if (n <= 0) { /* The end: let it finish playing. */
            vx_control(audio, VX_AUDIO_DRAIN, NULL, 0);
            vx_mutex_lock(&lock);
            if (state == PLAYING && seek_to < 0) {
                state = STOPPED;
                ended = true;
            }
            vx_mutex_unlock(&lock);
            continue;
        }
        /* This app's volume (squared: it sounds even), and the spectrum's copy. */
        int gain = volume * volume; /* Of 10000. */
        for (int i = 0; i < n; i++) {
            int sum = 0;
            for (unsigned c = 0; c < channels; c++) {
                int16_t *v = &buffer[i * channels + c];
                sum += *v;
                if (gain != 10000) {
                    *v = (int16_t)(*v * gain / 10000);
                }
            }
            ring[(ring_frames + (unsigned)i) % RING] = (int16_t)(sum / (int)channels);
        }
        const char *p = (const char *)buffer;
        long left = (long)n * channels * 2;
        while (left > 0) {
            long w = vx_write(audio, p, (size_t)left);
            if (w <= 0) {
                break;
            }
            p += w, left -= w;
        }
        vx_mutex_lock(&lock);
        written += (unsigned long long)n;
        ring_frames += (unsigned long long)n;
        vx_mutex_unlock(&lock);
    }
    return NULL;
}

/* The spectrum of what's heard now: BANDS bands from 60 Hz to 12 kHz
 * (Goertzel's algorithm on 512 frames), falling back slowly. */
static void update_bands(void) {
    static float window_table[512];
    if (!window_table[256]) {
        for (int i = 0; i < 512; i++) { /* A Hann window, without cos: a parabola close enough. */
            float t = (float)i / 511.0f;
            window_table[i] = 4.0f * t * (1.0f - t);
        }
    }
    float input[512];
    bool live = false;
    vx_mutex_lock(&lock);
    if (state == PLAYING && playing_info.rate) {
        long long heard = heard_frames();
        long long end = heard < (long long)ring_frames ? heard : (long long)ring_frames;
        if (end >= 512 && (long long)ring_frames - end < RING - 512) {
            for (int i = 0; i < 512; i++) {
                input[i] = ring[(unsigned long long)(end - 512 + i) % RING] * window_table[i];
            }
            live = true;
        }
    }
    unsigned rate = playing_info.rate ? playing_info.rate : 44100;
    vx_mutex_unlock(&lock);
    for (int b = 0; b < BANDS; b++) {
        float level = 0;
        if (live) {
            /* Frequencies spaced evenly in pitch: 60 Hz * 200^(b / BANDS). */
            float f = 60.0f;
            for (int k = 0; k < b; k++) {
                f *= 1.2083f; /* (200^(1/28).) */
            }
            float w = 2.0f * 3.14159265f * f / (float)rate;
            /* cos(w) by its series (w stays under pi). */
            float w2 = w * w, c = 1 - w2 / 2 + w2 * w2 / 24 - w2 * w2 * w2 / 720 +
                                  w2 * w2 * w2 * w2 / 40320;
            float coeff = 2 * c, s1 = 0, s2 = 0;
            for (int i = 0; i < 512; i++) {
                float s0 = input[i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            /* To 0..1, roughly in decibels. */
            float db = 0;
            for (float p = power / 1e8f; p > 1.0f && db < 60; p /= 1.2589f) {
                db += 1;
            }
            level = db / 34.0f;
            level = level > 1 ? 1 : level;
        }
        bands[b] = level > bands[b] ? level : bands[b] * 0.82f;
        peaks[b] = bands[b] > peaks[b] ? bands[b] : peaks[b] - 0.012f;
        peaks[b] = peaks[b] < 0 ? 0 : peaks[b];
    }
}

static bool read_info(struct track *t) {
    if (!t->known) {
        struct decoder *d = decoder_open(t->path, &t->info);
        if (!d) {
            return false;
        }
        decoder_close(d);
        t->known = true;
    }
    return true;
}

static void play(int i) {
    if (i < 0 || i >= track_count) {
        return;
    }
    struct decoder_info info;
    struct decoder *d = decoder_open(tracks[i].path, &info);
    if (!d) {
        printf("music: can't play %s\n", tracks[i].path);
        return;
    }
    vx_mutex_lock(&lock);
    stop_sound();
    if (decoder) {
        decoder_close(decoder);
    }
    decoder = d;
    playing_info = info;
    tracks[i].info = info;
    tracks[i].known = true;
    struct vx_audio_format format = {info.rate, info.channels};
    vx_control(audio, VX_AUDIO_SET_FORMAT, &format, sizeof(format));
    start_at = 0;
    written = 0;
    ring_frames = 0;
    seek_to = -1;
    playing = i;
    selected = i;
    ended = false;
    state = PLAYING;
    vx_mutex_unlock(&lock);
    const char *name = strrchr(tracks[i].path, '/');
    printf("music: playing %s (%s, %u Hz, %s, %d:%02d)\n", name ? name + 1 : tracks[i].path,
           info.format, info.rate, info.channels == 2 ? "stereo" : "mono", (int)info.seconds / 60,
           (int)info.seconds % 60);
    fflush(stdout);
}

static void toggle_pause(void) {
    if (state == PLAYING) {
        double at = position();
        vx_mutex_lock(&lock);
        state = PAUSED;
        stop_sound();
        start_at = at;
        written = 0;
        vx_mutex_unlock(&lock);
    } else if (state == PAUSED) {
        vx_mutex_lock(&lock);
        seek_to = start_at;
        state = PLAYING;
        vx_mutex_unlock(&lock);
    } else {
        play(selected >= 0 ? selected : shown_count ? shown[0] : 0);
    }
}

static void seek(double seconds) {
    if (playing < 0) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    if (playing_info.seconds && seconds > playing_info.seconds) {
        seconds = playing_info.seconds;
    }
    vx_mutex_lock(&lock);
    if (state == PAUSED) {
        start_at = seconds;
        written = 0;
    } else {
        seek_to = seconds;
    }
    vx_mutex_unlock(&lock);
}

static void set_volume(int v) {
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
}

static void save_volume(void) {
    struct vx_settings s;
    vx_settings_load(&s, "music.conf");
    vx_settings_set_int(&s, "volume", volume);
    vx_settings_save(&s);
}

/* ---- The list ---- */

static const char *title_of(const struct track *t) {
    if (t->info.title[0]) {
        return t->info.title;
    }
    const char *name = strrchr(t->path, '/');
    return name ? name + 1 : t->path;
}

static const char *artist_of(const struct track *t) {
    return t->info.artist[0] ? t->info.artist : "Unknown Artist";
}

static const char *album_of(const struct track *t) {
    return t->info.album[0] ? t->info.album : "Unknown Album";
}

static bool contains(const char *text, const char *part) {
    size_t n = strlen(part);
    for (; *text; text++) {
        if (!strncasecmp(text, part, n)) {
            return true;
        }
    }
    return n == 0;
}

/* What the song list shows now. */
static void update_shown(void) {
    shown_count = 0;
    for (int i = 0; i < track_count; i++) {
        const struct track *t = &tracks[i];
        if (filter_artist[0] && strcmp(artist_of(t), filter_artist)) {
            continue;
        }
        if (filter_album[0] && strcmp(album_of(t), filter_album)) {
            continue;
        }
        if (search[0] && !contains(title_of(t), search) && !contains(artist_of(t), search) &&
            !contains(album_of(t), search)) {
            continue;
        }
        shown[shown_count++] = i;
    }
    /* The artists or albums, each with its first song and how many. */
    group_count = 0;
    for (int i = 0; i < track_count; i++) {
        const char *name = view == V_ALBUMS ? album_of(&tracks[i]) : artist_of(&tracks[i]);
        int g = 0;
        while (g < group_count && strcmp(groups[g], name)) {
            g++;
        }
        if (g == group_count) {
            snprintf(groups[g], sizeof(groups[g]), "%s", name);
            group_first[g] = i;
            group_size[g] = 0;
            group_count++;
        }
        group_size[g]++;
    }
}

static bool playable(const char *name) {
    static const char *const kinds[] = {".mp3", ".ogg", ".oga", ".flac", ".wav"};
    size_t n = strlen(name);
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        size_t e = strlen(kinds[k]);
        if (n > e && !strcasecmp(name + n - e, kinds[k])) {
            return true;
        }
    }
    return false;
}

static void add_track(const char *path) {
    for (int i = 0; i < track_count; i++) {
        if (!strcmp(tracks[i].path, path)) {
            return;
        }
    }
    if (track_count < MAX_TRACKS) {
        struct track *t = &tracks[track_count];
        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "%s", path);
        if (read_info(t)) {
            track_count++;
        }
    }
}

static int compare_paths(const void *a, const void *b) {
    return strcasecmp(((const struct track *)a)->path, ((const struct track *)b)->path);
}

static void add_folder(const char *folder) {
    DIR *dir = opendir(folder);
    if (!dir) {
        return;
    }
    int first = track_count;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] != '.' && playable(e->d_name)) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
            add_track(path);
        }
    }
    closedir(dir);
    qsort(tracks + first, (size_t)(track_count - first), sizeof(tracks[0]), compare_paths);
}

static void add_any(const char *path) {
    struct vx_stat st;
    if (vx_stat(path, &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
        add_folder(path);
    } else if (playable(path)) {
        add_track(path);
    }
    update_shown();
}

static void remove_track(int i) {
    if (i < 0 || i >= track_count) {
        return;
    }
    if (i == playing) {
        vx_mutex_lock(&lock);
        state = STOPPED;
        stop_sound();
        playing = -1;
        vx_mutex_unlock(&lock);
    } else if (playing > i) {
        playing--;
    }
    memmove(tracks + i, tracks + i + 1, (size_t)(track_count - i - 1) * sizeof(tracks[0]));
    track_count--;
    if (selected >= track_count) {
        selected = track_count - 1;
    }
    update_shown();
}

/* The song after (or before) `from`, among those shown. */
static int next_track(int from, int step) {
    if (!shown_count) {
        return -1;
    }
    if (shuffle && shown_count > 1) {
        int n;
        do {
            n = shown[vx_uptime() * 7919 % shown_count];
        } while (n == from);
        return n;
    }
    int at = -1;
    for (int k = 0; k < shown_count; k++) {
        if (shown[k] == from) {
            at = k;
        }
    }
    int k = at < 0 ? (step > 0 ? 0 : shown_count - 1) : at + step;
    if (k >= shown_count) {
        return repeat ? shown[0] : -1;
    }
    return shown[k < 0 ? 0 : k];
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

static struct rect main_area(void) {
    return (struct rect){SIDEBAR, 0, W() - SIDEBAR, H() - BAR};
}

static struct rect list_area(void) {
    struct rect m = main_area();
    return (struct rect){m.x, HERO + HEADER, m.w, m.h - HERO - HEADER};
}

static struct rect search_rect(void) {
    return (struct rect){W() - 24 - 220, HERO + 4, 220, 26};
}

/* The transport's parts. */
enum part {
    P_NONE, P_SHUFFLE, P_BACK, P_PLAY, P_NEXT, P_REPEAT, P_SCRUB, P_VOLUME, P_MUTE,
    P_SONGS, P_ARTISTS, P_ALBUMS, P_FOLDER, P_ADD, P_CLEAR_FILTER, P_SEARCH,
};

static int bar_center(void) {
    return SIDEBAR + (W() - SIDEBAR) / 2;
}

static struct rect part_rect(enum part p) {
    int cx = bar_center(), y = H() - BAR;
    switch (p) {
    case P_SHUFFLE: return (struct rect){cx - 128, y + 10, 32, 32};
    case P_BACK: return (struct rect){cx - 78, y + 10, 36, 32};
    case P_PLAY: return (struct rect){cx - 23, y + 4, 46, 46};
    case P_NEXT: return (struct rect){cx + 42, y + 10, 36, 32};
    case P_REPEAT: return (struct rect){cx + 96, y + 10, 32, 32};
    case P_SCRUB: return (struct rect){SIDEBAR + 70, y + 54, W() - SIDEBAR - 70 - 290, 18};
    case P_MUTE: return (struct rect){W() - 190, y + 46, 24, 30};
    case P_VOLUME: return (struct rect){W() - 160, y + 52, 130, 18};
    case P_SONGS: return (struct rect){12, 108, SIDEBAR - 24, 30};
    case P_ARTISTS: return (struct rect){12, 140, SIDEBAR - 24, 30};
    case P_ALBUMS: return (struct rect){12, 172, SIDEBAR - 24, 30};
    case P_ADD: return (struct rect){12, 300, SIDEBAR - 24, 30};
    case P_CLEAR_FILTER: return (struct rect){SIDEBAR + 24, HERO + 4, 0, 26};
    case P_SEARCH: return search_rect();
    default: return (struct rect){0, 0, 0, 0};
    }
}

static bool in(struct rect r, int x, int y) {
    return vx_inside(x, y, r.x, r.y, r.w, r.h);
}

static enum part hot = P_NONE;
static int hot_row = -1, hot_card = -1, hot_scrub_x = -1;

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

static void sidebar_item(struct vx_surface *s, enum part p, const char *label, int count,
                         bool on) {
    struct rect r = part_rect(p);
    if (on) {
        vx_draw_gel(s, r.x, r.y, r.w, r.h, 9, VX_COLOR_ACCENT);
    } else if (hot == p) {
        vx_fill_rounded(s, r.x, r.y, r.w, r.h, 9, 0xffffff, 22);
    }
    /* Its sign: a note, a person, a record. */
    int ix = r.x + 12, cy = r.y + r.h / 2;
    uint32_t ink = on ? 0xffffff : 0xc8cce0;
    if (p == P_SONGS) {
        lk_note(s, ix - 2, cy - 9, 18, ink);
    } else if (p == P_ARTISTS) {
        lk_circle(s, ix + 7, cy - 4, 4, ink, 255);
        vx_fill_rounded(s, ix, cy + 1, 14, 8, 4, ink, 255);
    } else if (p == P_ALBUMS) {
        lk_circle(s, ix + 7, cy, 8, ink, 255);
        lk_circle(s, ix + 7, cy, 2, on ? VX_COLOR_ACCENT : 0x2a2e44, 255);
    } else if (p == P_ADD) {
        vx_fill(s, ix + 1, cy - 1, 12, 2, ink);
        vx_fill(s, ix + 6, cy - 6, 2, 12, ink);
    }
    text(s, on ? bold(13) : sans(13), r.x + 36, cy - 8, label, on ? 0xffffff : 0xe2e4ee);
    if (count >= 0) {
        char n[12];
        snprintf(n, sizeof(n), "%d", count);
        const struct vx_font *f = sans(12);
        text(s, f, r.x + r.w - 12 - vx_text_width_font(f, n), cy - 8, n, on ? 0xe8f0ff : 0x8a8fa8);
    }
}

static int count_groups(bool albums) {
    enum view saved = view;
    view = albums ? V_ALBUMS : V_ARTISTS;
    update_shown();
    int n = group_count;
    view = saved;
    update_shown();
    return n;
}

static void draw_sidebar(struct vx_surface *s) {
    int h = H();
    lk_sidebar(s, 0, 0, SIDEBAR, h);
    lk_logo(s, 20, 22, 46, false);
    text(s, bold(18), 78, 26, "Music", 0xffffff);
    text(s, sans(12), 78, 50, track_count == 1 ? "1 song" : "", 0xc8cce0);
    if (track_count != 1) {
        char n[32];
        snprintf(n, sizeof(n), "%d songs", track_count);
        text(s, sans(12), 78, 50, n, 0xc8cce0);
    }
    text(s, bold(11), 24, 88, "LIBRARY", 0x8a8fa8);
    sidebar_item(s, P_SONGS, "Songs", track_count,
                 view == V_SONGS && !filter_artist[0] && !filter_album[0]);
    sidebar_item(s, P_ARTISTS, "Artists", count_groups(false), view == V_ARTISTS || filter_artist[0]);
    sidebar_item(s, P_ALBUMS, "Albums", count_groups(true), view == V_ALBUMS || filter_album[0]);
    text(s, bold(11), 24, 220, "FOLDERS", 0x8a8fa8);
    for (int i = 0; i < 2; i++) {
        int y = 240 + i * 26;
        vx_fill_rounded(s, 26, y + 3, 14, 11, 2, 0xf0c050, 255);
        vx_fill_rounded(s, 26, y + 1, 7, 4, 1, 0xf0c050, 255);
        text_fit(s, sans(12), 48, y, SIDEBAR - 64, i ? "Shared music" : "Your Music folder", 0xd8dbe8);
    }
    sidebar_item(s, P_ADD, "Add Music...", -1, false);
    /* What's playing, at the bottom. */
    if (playing >= 0) {
        const struct track *t = &tracks[playing];
        int y = h - BAR - 76;
        vx_fill_rounded(s, 12, y, SIDEBAR - 24, 64, 12, 0xffffff, 18);
        lk_art(s, 20, y + 8, 48, 8, album_of(t));
        text_fit(s, bold(12), 78, y + 14, SIDEBAR - 100, title_of(t), 0xffffff);
        text_fit(s, sans(12), 78, y + 33, SIDEBAR - 100, artist_of(t), 0xb8bcd0);
    }
}

static void draw_spectrum(struct vx_surface *s, int x, int y, int w, int h) {
    int gap = 3, bw = (w - gap * (BANDS - 1)) / BANDS;
    bw = bw < 3 ? 3 : bw;
    uint32_t base = vx_theme.dark ? 0xffffff : 0x000000;
    for (int b = 0; b < BANDS; b++) {
        int bx = x + b * (bw + gap);
        vx_fill_rounded(s, bx, y, bw, h, bw / 2, base, vx_theme.dark ? 14 : 10);
        int bh = (int)(bands[b] * h);
        if (bh > 2) {
            uint32_t c = lk_hsv(210 + b * 4, 150, 255);
            c = vx_mix(VX_COLOR_ACCENT, c, 90);
            vx_draw_gel(s, bx, y + h - bh, bw, bh, bw / 2, c);
        }
        int py = y + h - (int)(peaks[b] * h) - 3;
        if (peaks[b] > 0.02f) {
            vx_fill_rounded(s, bx, py, bw, 3, 1, VX_COLOR_ACCENT, 230);
        }
    }
}

static void draw_hero(struct vx_surface *s) {
    struct rect m = main_area();
    uint32_t tint = vx_mix(VX_COLOR_WINDOW, VX_COLOR_ACCENT, vx_theme.dark ? 30 : 22);
    lk_gradient(s, m.x, 0, m.w, HERO, tint, VX_COLOR_WINDOW);
    int x = m.x + 24, y = 22;
    if (playing < 0) {
        lk_art(s, x, y, 124, 16, "Music");
        text(s, bold(26), x + 148, y + 14, "Your music", VX_COLOR_TEXT);
        text(s, sans(14), x + 148, y + 52,
             track_count ? "Double-click a song to play it, or press Space."
                         : "Add some: drop files here, or Add Music... on the left.",
             VX_COLOR_DIM);
        text(s, sans(13), x + 148, y + 76, "MP3, Ogg Vorbis, FLAC and WAV.", VX_COLOR_DIM);
        return;
    }
    const struct track *t = &tracks[playing];
    vx_fill_rounded(s, x + 3, y + 6, 124, 124, 16, 0x000000, 60); /* Its shadow. */
    lk_art(s, x, y, 124, 16, album_of(t));
    int tx = x + 148, tw = m.x + m.w - 24 - tx;
    text(s, bold(11), tx, y + 4, state == PAUSED ? "PAUSED" : "NOW PLAYING", VX_COLOR_ACCENT);
    text_fit(s, bold(24), tx, y + 20, tw, title_of(t), VX_COLOR_TEXT);
    char line[300];
    snprintf(line, sizeof(line), "%s  -  %s", artist_of(t), album_of(t));
    text_fit(s, sans(14), tx, y + 54, tw, line, VX_COLOR_DIM);
    char rate[24];
    snprintf(rate, sizeof(rate), "%u.%u kHz", t->info.rate / 1000, t->info.rate % 1000 / 100);
    uint32_t chip = vx_mix(VX_COLOR_WINDOW, VX_COLOR_ACCENT, 50), ink = VX_COLOR_TEXT;
    int cx = tx;
    cx += lk_chip(s, cx, y + 78, t->info.format, chip, ink) + 6;
    cx += lk_chip(s, cx, y + 78, rate, chip, ink) + 6;
    lk_chip(s, cx, y + 78, t->info.channels == 2 ? "Stereo" : "Mono", chip, ink);
    draw_spectrum(s, tx, y + 104, tw < 520 ? tw : 520, 22);
}

/* The little bars beside the song playing, moving with the sound. */
static void draw_eq(struct vx_surface *s, int x, int cy, uint32_t color) {
    for (int k = 0; k < 3; k++) {
        float level = state == PLAYING ? bands[3 + k * 6] : 0.2f;
        int h = 3 + (int)(level * 10);
        vx_fill_rounded(s, x + k * 4, cy + 6 - h, 3, h, 1, color, 255);
    }
}

static void draw_songs(struct vx_surface *s) {
    struct rect l = list_area();
    int col_title = l.x + 52, col_artist = l.x + l.w * 46 / 100, col_album = l.x + l.w * 68 / 100;
    int col_time = l.x + l.w - 70;
    static const char *const headings[] = {"#", "Title", "Artist", "Album", "Time"};
    int cols[5] = {l.x + 20, col_title, col_artist, col_album, col_time};
    for (int c = 0; c < 5; c++) {
        text(s, bold(11), cols[c], l.y + 6, headings[c], VX_COLOR_DIM);
    }
    vx_fill(s, l.x + 16, l.y + 24, l.w - 32, 1, VX_COLOR_LINE);
    int y0 = l.y + 28, rows = (l.h - 28) / ROW;
    if (rows < 1) {
        return;
    }
    int at = -1;
    for (int k = 0; k < shown_count; k++) {
        if (shown[k] == selected) {
            at = k;
        }
    }
    if (at >= 0 && at < top) {
        top = at;
    } else if (at >= top + rows) {
        top = at - rows + 1;
    }
    top = top > shown_count - rows ? shown_count - rows : top;
    top = top < 0 ? 0 : top;
    if (!shown_count) {
        lk_centered(s, sans(14), l.x + l.w / 2, y0 + 40,
                    track_count ? "Nothing matches." : "No songs yet.", VX_COLOR_DIM);
    }
    for (int r = 0; r < rows && top + r < shown_count; r++) {
        int i = shown[top + r], ry = y0 + r * ROW;
        const struct track *t = &tracks[i];
        bool on = i == selected, now = i == playing;
        if (on) {
            vx_draw_gel(s, l.x + 10, ry, l.w - 20, ROW - 2, 9, VX_COLOR_ACCENT);
        } else if (top + r == hot_row) {
            vx_fill_rounded(s, l.x + 10, ry, l.w - 20, ROW - 2, 9, VX_COLOR_ACCENT, 30);
        }
        uint32_t ink = on ? 0xffffff : now ? VX_COLOR_ACCENT : VX_COLOR_TEXT;
        uint32_t dim = on ? 0xe6eeff : VX_COLOR_DIM;
        int ty = ry + (ROW - 2 - 16) / 2;
        if (now) {
            draw_eq(s, cols[0], ry + ROW / 2 - 2, on ? 0xffffff : VX_COLOR_ACCENT);
        } else {
            char n[8];
            snprintf(n, sizeof(n), "%d", top + r + 1);
            text(s, sans(12), cols[0], ty, n, dim);
        }
        text_fit(s, now || on ? bold(13) : sans(13), col_title, ty, col_artist - col_title - 14,
                 title_of(t), ink);
        text_fit(s, sans(13), col_artist, ty, col_album - col_artist - 14, artist_of(t), dim);
        text_fit(s, sans(13), col_album, ty, col_time - col_album - 14, album_of(t), dim);
        char length[16];
        lk_time(length, sizeof(length), t->info.seconds);
        text(s, sans(13), col_time, ty, length, dim);
    }
}

#define CARD 150
#define CARD_H 196

static void draw_cards(struct vx_surface *s) {
    struct rect l = list_area();
    int per_row = (l.w - 32) / (CARD + 18);
    per_row = per_row < 1 ? 1 : per_row;
    int rows = (l.h - 10) / (CARD_H + 10) + 1;
    group_top = group_top > group_count - per_row ? group_count - per_row : group_top;
    group_top = group_top < 0 ? 0 : group_top / per_row * per_row;
    for (int k = 0; k < rows * per_row && group_top + k < group_count; k++) {
        int g = group_top + k;
        int cx = l.x + 24 + (k % per_row) * (CARD + 18), cy = l.y + 10 + (k / per_row) * (CARD_H + 10);
        if (cy + CARD_H > l.y + l.h + CARD_H / 2) {
            break;
        }
        bool lit = g == hot_card;
        if (lit) {
            vx_fill_rounded(s, cx - 8, cy - 6, CARD + 16, CARD_H, 14, VX_COLOR_ACCENT, 36);
        }
        const struct track *t = &tracks[group_first[g]];
        if (view == V_ARTISTS) { /* Artists: round. */
            vx_fill_rounded(s, cx + 3, cy + 5, CARD, CARD, CARD / 2, 0x000000, 50);
            lk_art(s, cx, cy, CARD, CARD / 2, groups[g]);
        } else {
            vx_fill_rounded(s, cx + 3, cy + 5, CARD, CARD, 14, 0x000000, 50);
            lk_art(s, cx, cy, CARD, 14, album_of(t));
        }
        if (lit) { /* A play badge. */
            lk_circle(s, cx + CARD - 26, cy + CARD - 26, 18, VX_COLOR_ACCENT, 255);
            lk_play_sign(s, cx + CARD - 25, cy + CARD - 26, 14, false, 0xffffff);
        }
        text_fit(s, bold(13), cx, cy + CARD + 10, CARD, groups[g], VX_COLOR_TEXT);
        char line[160];
        if (view == V_ALBUMS) {
            snprintf(line, sizeof(line), "%s", artist_of(t));
        } else {
            snprintf(line, sizeof(line), "%d song%s", group_size[g], group_size[g] == 1 ? "" : "s");
        }
        text_fit(s, sans(12), cx, cy + CARD + 28, CARD, line, VX_COLOR_DIM);
    }
}

static void draw_header(struct vx_surface *s) {
    struct rect m = main_area();
    const char *title = view == V_ARTISTS ? "Artists" : view == V_ALBUMS ? "Albums" : "Songs";
    char t[200];
    snprintf(t, sizeof(t), "%s", title);
    if (view == V_SONGS && (filter_artist[0] || filter_album[0])) {
        snprintf(t, sizeof(t), "%s", filter_album[0] ? filter_album : filter_artist);
    }
    int x = m.x + 24;
    text_fit(s, bold(18), x, HERO + 6, 320, t, VX_COLOR_TEXT);
    if (view == V_SONGS && (filter_artist[0] || filter_album[0])) {
        /* A way back to every song. */
        int tw = vx_text_width_font(bold(18), t);
        tw = tw > 320 ? 320 : tw;
        struct rect r = {x + tw + 12, HERO + 8, 92, 22};
        vx_draw_button_flags(s, r.x, r.y, r.w, r.h, "All Songs", hot == P_CLEAR_FILTER ? VX_BUTTON_HOT : 0);
    }
    struct rect r = search_rect();
    vx_draw_field(s, r.x, r.y, r.w, search[0] || searching ? search : "", searching);
    if (!search[0] && !searching) {
        text(s, sans(13), r.x + 28, r.y + 5, "Search", VX_COLOR_DIM);
    }
    /* A magnifier. */
    lk_circle(s, r.x + 13, r.y + 11, 5, VX_COLOR_DIM, 255);
    lk_circle(s, r.x + 13, r.y + 11, 3, VX_COLOR_VIEW, 255);
    lk_line(s, r.x + 16, r.y + 15, r.x + 19, r.y + 19, 2, VX_COLOR_DIM);
    if (search[0] || searching) {
        text(s, sans(13), r.x + 28, r.y + 5, search, VX_COLOR_TEXT);
    }
}

static void transport_button(struct vx_surface *s, enum part p, bool on) {
    struct rect r = part_rect(p);
    bool lit = hot == p;
    uint32_t ink = vx_theme.dark ? 0xe8eaf2 : 0x2a3040;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (p == P_PLAY) {
        vx_fill_rounded(s, r.x + 1, r.y + 3, r.w, r.h, r.w / 2, 0x000000, 60);
        vx_draw_gel(s, r.x, r.y, r.w, r.h, r.w / 2,
                    lit ? vx_mix(VX_COLOR_ACCENT, 0xffffff, 50) : VX_COLOR_ACCENT);
        lk_play_sign(s, cx + (state == PLAYING ? 0 : 2), cy, 18, state == PLAYING, 0xffffff);
        return;
    }
    if (lit || on) {
        vx_fill_rounded(s, r.x, r.y, r.w, r.h, r.h / 2, on ? VX_COLOR_ACCENT : ink, on ? 60 : 24);
    }
    uint32_t c = on ? VX_COLOR_ACCENT : ink;
    switch (p) {
    case P_BACK: lk_skip_sign(s, cx - 1, cy, 12, true, c); break;
    case P_NEXT: lk_skip_sign(s, cx + 1, cy, 12, false, c); break;
    case P_SHUFFLE: lk_shuffle_sign(s, cx, cy, c); break;
    case P_REPEAT: lk_repeat_sign(s, cx, cy, c); break;
    default: break;
    }
}

static void draw_bar(struct vx_surface *s) {
    int y = H() - BAR, w = W();
    /* Glass: lighter at the top, a bright line, then the color of the window. */
    uint32_t topc = vx_theme.dark ? 0x30303e : 0xfafbfd, bottom = vx_theme.dark ? 0x1c1c26 : 0xe8eaf0;
    lk_gradient(s, SIDEBAR, y, w - SIDEBAR, BAR, topc, bottom);
    vx_fill(s, SIDEBAR, y, w - SIDEBAR, 1, VX_COLOR_LINE);
    vx_fill(s, SIDEBAR, y + 1, w - SIDEBAR, 1, vx_theme.dark ? 0x3c3c4c : 0xffffff);
    transport_button(s, P_SHUFFLE, shuffle);
    transport_button(s, P_BACK, false);
    transport_button(s, P_PLAY, false);
    transport_button(s, P_NEXT, false);
    transport_button(s, P_REPEAT, repeat);
    /* The scrubber, with the times. */
    struct rect r = part_rect(P_SCRUB);
    double at = playing >= 0 ? position() : 0, total = playing >= 0 ? playing_info.seconds : 0;
    char elapsed[16], left[20], rest[16];
    lk_time(elapsed, sizeof(elapsed), at);
    lk_time(rest, sizeof(rest), total - at);
    snprintf(left, sizeof(left), "-%s", rest);
    const struct vx_font *f = sans(12);
    text(s, f, r.x - 12 - vx_text_width_font(f, elapsed), r.y + 1, elapsed, VX_COLOR_DIM);
    text(s, f, r.x + r.w + 12, r.y + 1, playing >= 0 ? left : "", VX_COLOR_DIM);
    lk_track(s, r.x, r.y + r.h / 2, r.w, total > 0 ? at / total : 0, hot == P_SCRUB,
             vx_theme.dark);
    if (hot == P_SCRUB && hot_scrub_x >= 0 && total > 0) {
        /* The time there, above the pointer. */
        char there[16];
        lk_time(there, sizeof(there), (double)(hot_scrub_x - r.x) / r.w * total);
        int tw = vx_text_width_font(f, there) + 12;
        int tx = hot_scrub_x - tw / 2;
        vx_fill_rounded(s, tx, r.y - 24, tw, 20, 6, 0x101420, 220);
        text(s, f, tx + 6, r.y - 22, there, 0xffffff);
    }
    /* The volume. */
    struct rect m = part_rect(P_MUTE), v = part_rect(P_VOLUME);
    lk_speaker_sign(s, m.x, m.y + 14, volume, vx_theme.dark ? 0xe8eaf2 : 0x2a3040);
    lk_track(s, v.x, v.y + v.h / 2, v.w, volume / 100.0, hot == P_VOLUME, vx_theme.dark);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    vx_fill(s, 0, 0, W(), H(), VX_COLOR_WINDOW);
    struct rect l = list_area();
    vx_fill(s, l.x, l.y - HEADER, l.w, l.h + HEADER, VX_COLOR_VIEW);
    draw_hero(s);
    vx_fill(s, l.x, HERO, l.w, HEADER, VX_COLOR_VIEW);
    draw_header(s);
    if (view == V_SONGS) {
        draw_songs(s);
    } else {
        draw_cards(s);
    }
    draw_bar(s);
    draw_sidebar(s);
    vx_window_present(window, 0, 0, W(), H());
}

static void update_title(void) {
    char title[200];
    if (playing >= 0) {
        snprintf(title, sizeof(title), "%s - Music", title_of(&tracks[playing]));
    } else {
        snprintf(title, sizeof(title), "Music");
    }
    vx_window_set_title(window, title);
}

/* ---- Input ---- */

static int row_at(int px, int py) {
    struct rect l = list_area();
    int y0 = l.y + 28;
    if (view != V_SONGS || !in(l, px, py) || py < y0) {
        return -1;
    }
    int k = top + (py - y0) / ROW;
    return k < shown_count ? k : -1;
}

static int card_at(int px, int py) {
    struct rect l = list_area();
    if (view == V_SONGS || !in(l, px, py)) {
        return -1;
    }
    int per_row = (l.w - 32) / (CARD + 18);
    per_row = per_row < 1 ? 1 : per_row;
    int col = (px - l.x - 24) / (CARD + 18), row = (py - l.y - 10) / (CARD_H + 10);
    if (px < l.x + 24 || col >= per_row || (px - l.x - 24) % (CARD + 18) > CARD) {
        return -1;
    }
    int g = group_top + row * per_row + col;
    return g < group_count ? g : -1;
}

static enum part part_at(int px, int py) {
    static const enum part parts[] = {P_SHUFFLE, P_BACK, P_PLAY, P_NEXT, P_REPEAT, P_SCRUB,
                                      P_MUTE, P_VOLUME, P_SONGS, P_ARTISTS, P_ALBUMS, P_ADD,
                                      P_SEARCH};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        struct rect r = part_rect(parts[i]);
        if (parts[i] == P_SCRUB || parts[i] == P_VOLUME) {
            r.x -= 8, r.w += 16, r.y -= 6, r.h += 12;
        }
        if (in(r, px, py)) {
            return parts[i];
        }
    }
    if (view == V_SONGS && (filter_artist[0] || filter_album[0])) {
        const char *t = filter_album[0] ? filter_album : filter_artist;
        int tw = vx_text_width_font(bold(18), t);
        tw = tw > 320 ? 320 : tw;
        if (vx_inside(px, py, SIDEBAR + 24 + tw + 12, HERO + 8, 92, 22)) {
            return P_CLEAR_FILTER;
        }
    }
    return P_NONE;
}

static void add_from_dialog(void) {
    char path[256];
    if (vx_open_dialog("Add Music", vx_home(), path, sizeof(path))) {
        add_any(path);
    }
}

static void show_view(enum view v) {
    view = v;
    filter_artist[0] = filter_album[0] = '\0';
    top = group_top = 0;
    update_shown();
}

static void press(enum part p, int px) {
    switch (p) {
    case P_BACK:
        if (playing >= 0 && position() > 3) {
            seek(0);
        } else {
            play(next_track(playing, -1));
        }
        break;
    case P_PLAY: toggle_pause(); break;
    case P_NEXT: play(next_track(playing, 1)); break;
    case P_SHUFFLE: shuffle = !shuffle; break;
    case P_REPEAT: repeat = !repeat; break;
    case P_SCRUB: {
        struct rect r = part_rect(P_SCRUB);
        seek((double)(px - r.x) / r.w * playing_info.seconds);
        break;
    }
    case P_VOLUME: {
        struct rect r = part_rect(P_VOLUME);
        set_volume((px - r.x) * 100 / r.w);
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
    case P_SONGS: show_view(V_SONGS); break;
    case P_ARTISTS: show_view(V_ARTISTS); break;
    case P_ALBUMS: show_view(V_ALBUMS); break;
    case P_ADD: add_from_dialog(); break;
    case P_CLEAR_FILTER: show_view(V_SONGS); break;
    case P_SEARCH: searching = true; break;
    default: break;
    }
    if (p != P_SEARCH) {
        searching = searching && p == P_NONE;
    }
    update_title();
}

static void pointer(const struct vx_gui_event *e, int *held) {
    static long last_click;
    static int last_row = -1;
    static enum part dragging = P_NONE;
    hot = part_at(e->x, e->y);
    hot_row = row_at(e->x, e->y);
    hot_card = card_at(e->x, e->y);
    hot_scrub_x = hot == P_SCRUB ? e->x : -1;
    bool down = (e->buttons & 1) && !(*held & 1), up = !(e->buttons & 1) && (*held & 1);
    *held = e->buttons;
    if (e->wheel) {
        if (view == V_SONGS) {
            top -= e->wheel * 3;
        } else {
            group_top -= e->wheel * 3;
        }
    }
    if (dragging != P_NONE && (e->buttons & 1)) { /* Dragging the scrubber or the volume. */
        if (dragging == P_VOLUME) {
            press(P_VOLUME, e->x);
        } else {
            hot_scrub_x = e->x;
        }
        return;
    }
    if (up && dragging != P_NONE) {
        if (dragging == P_SCRUB) {
            press(P_SCRUB, e->x);
        } else {
            save_volume();
        }
        dragging = P_NONE;
        return;
    }
    if (!down) {
        return;
    }
    if (hot == P_SCRUB && playing >= 0) {
        dragging = P_SCRUB;
        return;
    }
    if (hot == P_VOLUME) {
        dragging = P_VOLUME;
        press(P_VOLUME, e->x);
        return;
    }
    if (hot != P_NONE) {
        press(hot, e->x);
        return;
    }
    searching = false;
    if (hot_card >= 0) { /* An artist's or album's songs; the first one plays. */
        const char *name = groups[hot_card];
        bool albums = view == V_ALBUMS;
        view = V_SONGS;
        snprintf(albums ? filter_album : filter_artist, 128, "%s", name);
        top = 0;
        update_shown();
        if (shown_count) {
            play(shown[0]);
            update_title();
        }
        return;
    }
    int k = hot_row;
    if (k >= 0) {
        int i = shown[k];
        long t = vx_uptime();
        if (i == last_row && t - last_click < 500) {
            play(i);
            update_title();
        }
        selected = i;
        last_row = i;
        last_click = t;
    }
}

static bool ctrl;

static void key(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
        return;
    }
    if (!e->value) {
        return;
    }
    if (searching) {
        if (e->key == VX_KEY_ESC) {
            search[0] = '\0';
            searching = false;
        } else if (e->key == VX_KEY_ENTER || e->key == VX_KEY_DOWN) {
            searching = false;
            if (shown_count) {
                selected = shown[0];
            }
        } else if (vx_field_key(search, sizeof(search), e)) {
            view = V_SONGS;
            top = 0;
        }
        update_shown();
        return;
    }
    int at = -1;
    for (int k = 0; k < shown_count; k++) {
        if (shown[k] == selected) {
            at = k;
        }
    }
    switch (e->key) {
    case VX_KEY_UP:
        if (shown_count) {
            selected = shown[at > 0 ? at - 1 : 0];
        }
        break;
    case VX_KEY_DOWN:
        if (shown_count) {
            selected = shown[at + 1 < shown_count ? at + 1 : shown_count - 1];
        }
        break;
    case VX_KEY_LEFT:
        if (ctrl) {
            press(P_BACK, 0);
        } else {
            seek(position() - 5);
        }
        break;
    case VX_KEY_RIGHT:
        if (ctrl) {
            press(P_NEXT, 0);
        } else {
            seek(position() + 5);
        }
        break;
    case VX_KEY_DELETE: remove_track(selected); break;
    case VX_KEY_ESC:
        if (view != V_SONGS || filter_artist[0] || filter_album[0]) {
            show_view(V_SONGS);
        }
        break;
    case VX_KEY_ENTER:
        play(selected);
        update_title();
        break;
    default:
        if (ctrl && (e->character == 'f' || e->character == 'F' || e->character == 6)) {
            searching = true;
        } else if (e->character == ' ') {
            toggle_pause();
            update_title();
        } else if (e->character == '+' || e->character == '=') {
            set_volume(volume + 10);
            save_volume();
        } else if (e->character == '-') {
            set_volume(volume - 10);
            save_volume();
        }
        break;
    }
}

/* ---- Starting ---- */

static int play_headless(const char *path) {
    snprintf(tracks[0].path, sizeof(tracks[0].path), "%s", path);
    track_count = 1;
    play(0);
    if (playing < 0) {
        return 1;
    }
    while (state == PLAYING) {
        vx_sleep(50);
    }
    printf("music: done\n");
    return 0;
}

int main(int argc, char **argv) {
    audio = vx_open("/dev/audio0", VX_OPEN_WRITE);
    if (audio < 0) {
        fprintf(stderr, "music: no sound device (/dev/audio0)\n");
    }
    vx_thread_create(player, NULL);
    if (argc == 3 && !strcmp(argv[1], "--play")) {
        return play_headless(argv[2]);
    }
    struct vx_settings settings;
    vx_settings_load(&settings, "music.conf");
    set_volume(vx_settings_int(&settings, "volume", 100));
    window = vx_window_create_flags("Music", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "music: no desktop to open a window on\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        add_any(argv[i]);
    }
    if (argc > 1 && track_count) {
        update_shown();
        play(0);
    } else {
        char music[300];
        vx_home_path(music, sizeof(music), "Music");
        add_folder(music);
        add_folder("/share/music");
    }
    update_shown();
    update_title();
    printf("music: %d song%s\n", track_count, track_count == 1 ? "" : "s");
    fflush(stdout);
    int held = 0;
    for (;;) {
        if (ended) { /* On to the next song. */
            ended = false;
            int n = next_track(playing, 1);
            if (n >= 0) {
                play(n);
            } else {
                playing = -1;
            }
            update_title();
        }
        update_bands();
        draw();
        struct vx_gui_event e;
        bool moving = state == PLAYING;
        for (int b = 0; b < BANDS && !moving; b++) {
            moving = bands[b] > 0.01f || peaks[b] > 0.01f;
        }
        int got = vx_gui_wait(&e, moving ? 50 : 1000);
        if (got < 0) {
            return 0;
        }
        while (got > 0) {
            switch (e.type) {
            case VX_GUI_CLOSE:
                stop_sound();
                vx_window_destroy(window);
                return 0;
            case VX_GUI_KEY: key(&e); break;
            case VX_GUI_POINTER: pointer(&e, &held); break;
            case VX_GUI_RESIZE:
                if (e.width >= 720 && e.height >= 440) {
                    vx_window_resize(window, e.width, e.height);
                }
                break;
            case VX_GUI_DROP: {
                char *paths = vx_drop_paths(&e);
                for (char *line = paths ? strtok(paths, "\n") : NULL; line;
                     line = strtok(NULL, "\n")) {
                    add_any(line);
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
