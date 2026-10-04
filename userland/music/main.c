/* Music: plays MP3, Ogg Vorbis, FLAC and WAV files, in the spirit of the
 * old iTunes: Back, Play/Pause and Next on a brushed toolbar, a glossy
 * display with the song, the time and a scrubber, and the songs in a list
 * (double-click one to play it). Without files to open it looks in
 * ~/Music and /share/music. Files dropped on it join the list.
 *
 *   music [file...]       the window, playing the first file given
 *   music --play file     plays it without a window, and says what it is
 *
 * Keys: Space plays or pauses, Return plays the selected song, Up/Down
 * select, Left/Right go 5 seconds back or on, Delete takes a song out of
 * the list. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/thread.h>
#include "decode.h"

#define WIDTH 760
#define HEIGHT 480
#define TOOLBAR 74
#define HEADER 22
#define ROW 22
#define MAX_TRACKS 256

struct track {
    char path[256];
    struct decoder_info info;
    bool known; /* (Its info has been read.) */
};

static struct vx_window *window;
static struct track tracks[MAX_TRACKS];
static int track_count, selected = -1, top;
static bool shuffle, repeat;

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
static bool headless;

static void stop_sound(void) {
    if (audio >= 0) {
        vx_control(audio, VX_AUDIO_DROP, NULL, 0);
    }
}

/* Where the song is now (what's been heard), in seconds. */
static double position(void) {
    vx_mutex_lock(&lock);
    double at = start_at;
    if (decoder && playing_info.rate) {
        unsigned delay = 0;
        if (state == PLAYING && audio >= 0) {
            vx_control(audio, VX_AUDIO_DELAY, &delay, sizeof(delay));
        }
        long long heard = (long long)written - (long long)delay;
        at += (heard > 0 ? (double)heard : 0) / playing_info.rate;
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
            seek_to = -1;
            stop_sound();
        }
        int n = decoder_read(decoder, buffer, 2048, playing_info.channels);
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
        const char *p = (const char *)buffer;
        long left = (long)n * playing_info.channels * 2;
        while (left > 0) {
            long w = vx_write(audio, p, (size_t)left);
            if (w <= 0) {
                break;
            }
            p += w, left -= w;
        }
        vx_mutex_lock(&lock);
        written += (unsigned long long)n;
        vx_mutex_unlock(&lock);
    }
    return NULL;
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
        play(selected >= 0 ? selected : 0);
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

static int next_track(int from, int step) {
    if (!track_count) {
        return -1;
    }
    if (shuffle && track_count > 1) {
        int n;
        do {
            n = (int)(vx_uptime() * 7919 % track_count);
        } while (n == from);
        return n;
    }
    int n = from + step;
    if (n >= track_count) {
        return repeat ? 0 : -1;
    }
    return n < 0 ? 0 : n;
}

/* ---- The list ---- */

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
}

/* ---- Drawing ---- */

static const char *title_of(const struct track *t) {
    if (t->info.title[0]) {
        return t->info.title;
    }
    const char *name = strrchr(t->path, '/');
    return name ? name + 1 : t->path;
}

static void time_text(char *out, size_t size, double seconds) {
    int s = seconds < 0 ? 0 : (int)seconds;
    snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

enum { B_BACK, B_PLAY, B_NEXT, B_ADD, B_SHUFFLE, B_REPEAT, BUTTONS };
static int hot = -1;

static void button_rect(int b, int *x, int *y, int *w, int *h) {
    int width = window->surface.width;
    switch (b) {
    case B_BACK: *x = 14, *y = 22, *w = 40, *h = 30; break;
    case B_PLAY: *x = 58, *y = 15, *w = 44, *h = 44; break;
    case B_NEXT: *x = 106, *y = 22, *w = 40, *h = 30; break;
    case B_ADD: *x = width - 76, *y = 10, *w = 62, *h = 24; break;
    case B_SHUFFLE: *x = width - 146, *y = 42, *w = 64, *h = 24; break;
    default: *x = width - 76, *y = 42, *w = 62, *h = 24; break;
    }
}

static void display_rect(int *x, int *y, int *w, int *h) {
    *x = 162;
    *y = 9;
    *w = window->surface.width - 162 - 164;
    *h = TOOLBAR - 18;
}

static void scrubber_rect(int *x, int *y, int *w) {
    int dx, dy, dw, dh;
    display_rect(&dx, &dy, &dw, &dh);
    *x = dx + 52;
    *y = dy + dh - 16;
    *w = dw - 104;
}

/* A transport button: a gel, with a triangle or two, or two bars. */
static void draw_transport(struct vx_surface *s, int b) {
    int x, y, w, h;
    button_rect(b, &x, &y, &w, &h);
    bool lit = hot == b;
    vx_draw_gel(s, x, y, w, h, h / 2, b == B_PLAY ? (lit ? 0x7fb1ff : VX_COLOR_ACCENT)
                                                  : (lit ? 0xeef2f8 : 0xd8dce4));
    uint32_t ink = b == B_PLAY ? 0xffffff : 0x303846;
    int cx = x + w / 2, cy = y + h / 2;
    if (b == B_PLAY && state == PLAYING) {
        vx_fill(s, cx - 7, cy - 8, 5, 16, ink);
        vx_fill(s, cx + 2, cy - 8, 5, 16, ink);
        return;
    }
    int size = b == B_PLAY ? 9 : 6;
    for (int k = 0; k < (b == B_PLAY ? 1 : 2); k++) {
        int ox = b == B_PLAY ? cx - 3 : cx - 7 + k * 7;
        for (int i = 0; i < size; i++) { /* A triangle: pointing right, or left for Back. */
            int len = (size - i) * 2 - 1;
            int tx = b == B_BACK ? ox + 6 - i : ox + i;
            vx_fill(s, tx, cy - len / 2, 1, len, ink);
        }
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int width = s->width, height = s->height;
    vx_fill(s, 0, 0, width, height, VX_COLOR_VIEW);
    vx_draw_toolbar(s, 0, 0, width, TOOLBAR);
    for (int b = B_BACK; b <= B_NEXT; b++) {
        draw_transport(s, b);
    }
    int x, y, w, h;
    button_rect(B_ADD, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, "Add...", hot == B_ADD);
    button_rect(B_SHUFFLE, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, "Shuffle", shuffle || hot == B_SHUFFLE);
    button_rect(B_REPEAT, &x, &y, &w, &h);
    vx_draw_button(s, x, y, w, h, "Repeat", repeat || hot == B_REPEAT);
    /* The display: a pale, glossy panel, the song in the middle. */
    display_rect(&x, &y, &w, &h);
    uint32_t lcd = vx_theme.dark ? 0x2a3040 : 0xeef3e6;
    vx_fill_rounded(s, x, y, w, h, 9, vx_mix(VX_COLOR_LINE, 0x000000, 60), 255);
    for (int row = 1; row < h - 1; row++) {
        vx_fill(s, x + 2, y + row, w - 4, 1, vx_mix(vx_mix(lcd, 0xffffff, 120), lcd, row * 255 / h));
    }
    vx_fill_rounded(s, x + 1, y + 1, w - 2, h / 2, 8, 0xffffff, vx_theme.dark ? 18 : 70);
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 13);
    uint32_t ink = vx_theme.dark ? 0xe8eef8 : 0x1c2430;
    if (playing >= 0) {
        const struct track *t = &tracks[playing];
        const char *title = title_of(t);
        int tw = vx_text_width_font(bold, title);
        vx_text(s, bold, x + (w - (tw < w - 20 ? tw : w - 20)) / 2, y + 5, title, ink, VX_TRANSPARENT);
        char line[300];
        snprintf(line, sizeof(line), "%s%s%s", t->info.artist, t->info.artist[0] && t->info.album[0] ? " - " : "",
                 t->info.album);
        if (!line[0]) {
            snprintf(line, sizeof(line), "%s, %u Hz", t->info.format, t->info.rate);
        }
        int lw = vx_text_width(line);
        vx_draw_text_fit(s, x + (w - (lw < w - 20 ? lw : w - 20)) / 2, y + 21, w - 20, line,
                         vx_mix(ink, lcd, 90), VX_TRANSPARENT);
        double at = position(), total = playing_info.seconds;
        char elapsed[16], left[16];
        time_text(elapsed, sizeof(elapsed), at);
        time_text(left, sizeof(left), total - at);
        int sx, sy, sw;
        scrubber_rect(&sx, &sy, &sw);
        vx_draw_text(s, sx - 8 - vx_text_width(elapsed), sy - 4, elapsed, ink, VX_TRANSPARENT);
        char minus[20];
        snprintf(minus, sizeof(minus), "-%s", left);
        vx_draw_text(s, sx + sw + 8, sy - 4, minus, ink, VX_TRANSPARENT);
        vx_draw_progress(s, sx, sy, sw, 9, (unsigned long long)(at * 100),
                         (unsigned long long)(total > 0 ? total * 100 : 1));
    } else {
        const char *text = track_count ? "Double-click a song to play it" : "No music yet: Add... some";
        int tw = vx_text_width_font(bold, text);
        vx_text(s, bold, x + (w - tw) / 2, y + h / 2 - 9, text, vx_mix(ink, lcd, 80), VX_TRANSPARENT);
    }
    /* The list. */
    int list_y = TOOLBAR;
    int col_artist = width * 46 / 100, col_album = width * 68 / 100, col_time = width - 70;
    vx_draw_toolbar(s, 0, list_y, width, HEADER);
    static const char *const headings[] = {"Name", "Artist", "Album", "Time"};
    int cols[4] = {30, col_artist, col_album, col_time};
    for (int c = 0; c < 4; c++) {
        if (c) {
            vx_fill(s, cols[c] - 8, list_y + 3, 1, HEADER - 6, VX_COLOR_LINE);
        }
        vx_draw_text(s, cols[c], list_y + 3, headings[c], VX_COLOR_TEXT, VX_TRANSPARENT);
    }
    int rows = (height - list_y - HEADER) / ROW;
    if (selected >= 0 && selected < top) {
        top = selected;
    } else if (selected >= top + rows) {
        top = selected - rows + 1;
    }
    for (int r = 0; r < rows && top + r < track_count; r++) {
        int i = top + r, ry = list_y + HEADER + r * ROW;
        const struct track *t = &tracks[i];
        bool on = i == selected;
        if (on) {
            vx_draw_gel(s, 2, ry, width - 4, ROW, 6, VX_COLOR_ACCENT);
        } else if (r % 2) {
            vx_fill(s, 0, ry, width, ROW, vx_theme.stripe);
        }
        uint32_t text = on ? 0xffffff : VX_COLOR_TEXT, dim = on ? 0xeef4ff : VX_COLOR_DIM;
        if (i == playing) { /* A speaker: this one's playing. */
            uint32_t c = on ? 0xffffff : VX_COLOR_ACCENT;
            vx_fill(s, 10, ry + 8, 3, 6, c);
            for (int k = 0; k < 4; k++) {
                vx_fill(s, 13 + k, ry + 7 - k / 2, 1, 8 + k, c);
            }
        }
        vx_draw_text_fit(s, cols[0], ry + 3, col_artist - cols[0] - 12, title_of(t), text, VX_TRANSPARENT);
        vx_draw_text_fit(s, cols[1], ry + 3, col_album - cols[1] - 12, t->info.artist, dim, VX_TRANSPARENT);
        vx_draw_text_fit(s, cols[2], ry + 3, col_time - cols[2] - 12, t->info.album, dim, VX_TRANSPARENT);
        char length[16];
        time_text(length, sizeof(length), t->info.seconds);
        vx_draw_text(s, cols[3], ry + 3, length, dim, VX_TRANSPARENT);
    }
    vx_window_present(window, 0, 0, width, height);
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

static int row_at(int py) {
    int list = TOOLBAR + HEADER;
    if (py < list) {
        return -1;
    }
    int i = top + (py - list) / ROW;
    return i < track_count ? i : -1;
}

static int button_at(int px, int py) {
    for (int b = 0; b < BUTTONS; b++) {
        int x, y, w, h;
        button_rect(b, &x, &y, &w, &h);
        if (vx_inside(px, py, x, y, w, h)) {
            return b;
        }
    }
    return -1;
}

static void add_from_dialog(void) {
    char path[256];
    const char *home = getenv("HOME");
    if (vx_open_dialog("Add Music", home ? home : "/home", path, sizeof(path))) {
        struct vx_stat st;
        if (vx_stat(path, &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
            add_folder(path);
        } else {
            add_track(path);
        }
    }
}

static void press(int b) {
    switch (b) {
    case B_BACK:
        if (playing >= 0 && position() > 3) {
            seek(0);
        } else {
            play(next_track(playing < 0 ? 0 : playing, -1));
        }
        break;
    case B_PLAY: toggle_pause(); break;
    case B_NEXT: play(next_track(playing < 0 ? -1 : playing, 1)); break;
    case B_ADD: add_from_dialog(); break;
    case B_SHUFFLE: shuffle = !shuffle; break;
    case B_REPEAT: repeat = !repeat; break;
    }
    update_title();
}

static void pointer(const struct vx_gui_event *e, int *held) {
    static long last_click;
    static int last_row = -1;
    int now = button_at(e->x, e->y);
    if (now != hot) {
        hot = now;
    }
    bool down = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    if (e->wheel) {
        top -= e->wheel * 3;
        int rows = (window->surface.height - TOOLBAR - HEADER) / ROW;
        top = top > track_count - rows ? track_count - rows : top;
        top = top < 0 ? 0 : top;
    }
    if (!down) {
        return;
    }
    int sx, sy, sw;
    scrubber_rect(&sx, &sy, &sw);
    if (playing >= 0 && vx_inside(e->x, e->y, sx - 4, sy - 6, sw + 8, 20)) {
        seek((double)(e->x - sx) / sw * playing_info.seconds);
        return;
    }
    if (now >= 0) {
        press(now);
        return;
    }
    int row = row_at(e->y);
    if (row >= 0) {
        long t = vx_uptime();
        if (row == last_row && t - last_click < 500) {
            play(row);
            update_title();
        }
        selected = row;
        last_row = row;
        last_click = t;
    }
}

static void key(const struct vx_gui_event *e) {
    if (!e->value) {
        return;
    }
    switch (e->key) {
    case VX_KEY_UP: selected = selected > 0 ? selected - 1 : 0; break;
    case VX_KEY_DOWN: selected = selected + 1 < track_count ? selected + 1 : selected; break;
    case VX_KEY_LEFT: seek(position() - 5); break;
    case VX_KEY_RIGHT: seek(position() + 5); break;
    case VX_KEY_DELETE: remove_track(selected); break;
    case VX_KEY_ENTER:
        play(selected);
        update_title();
        break;
    default:
        if (e->character == ' ') {
            toggle_pause();
            update_title();
        }
        break;
    }
}

/* ---- Starting ---- */

static int play_headless(const char *path) {
    headless = true;
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
    window = vx_window_create_flags("Music", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "music: no desktop to open a window on\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        struct vx_stat st;
        if (vx_stat(argv[i], &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
            add_folder(argv[i]);
        } else {
            add_track(argv[i]);
        }
    }
    if (argc > 1 && track_count) {
        play(0);
    } else {
        const char *home = getenv("HOME");
        char music[256];
        snprintf(music, sizeof(music), "%s/Music", home ? home : "/home");
        add_folder(music);
        add_folder("/share/music");
    }
    update_title();
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
        draw();
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, state == PLAYING ? 250 : 1000);
        if (got < 0) {
            return 0;
        }
        if (got == 0) {
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            stop_sound();
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_RESIZE:
            if (e.width >= 560 && e.height >= 300) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        case VX_GUI_DROP: {
            char *paths = vx_drop_paths(&e);
            for (char *line = paths ? strtok(paths, "\n") : NULL; line; line = strtok(NULL, "\n")) {
                struct vx_stat st;
                if (vx_stat(line, &st) == 0 && st.type == VX_TYPE_DIRECTORY) {
                    add_folder(line);
                } else if (playable(line)) {
                    add_track(line);
                }
            }
            free(paths);
            break;
        }
        default:
            break;
        }
    }
}
