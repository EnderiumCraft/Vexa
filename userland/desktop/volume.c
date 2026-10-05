/* The sound volume: a speaker on the panel (scrolling over it changes the
 * volume, a click opens a slider, Mute, and the outputs to choose from),
 * and the volume keys, which show a bubble with the level for a moment.
 * The kernel's sound core keeps the volume (/dev/audio0); the desktop keeps
 * it in desktop.conf too, and sets it again when it starts. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

#define KEY_MUTE 113
#define KEY_VOLUME_DOWN 114
#define KEY_VOLUME_UP 115
#define BUTTON_WIDTH 28
#define WIDTH 280
#define STEP 5
#define OSD_MS 1500
#define OSD_SIZE 180
/* Where things are, from the popup's top. */
#define SLIDER_Y 46
#define OUTPUTS_Y 92
#define OUTPUT_ROW 28

bool volume_open;
static int audio = -1;
static long next_check;
static struct vx_audio_volume vol = {.volume = 80};
static struct vx_audio_outputs outs;
static bool known; /* The sound core answered. */
static unsigned last_changes = ~0u;
static bool sliding;
static int hot = -1; /* HOT_* or an output's index. */
enum { HOT_MUTE = 100, HOT_SETTINGS, HOT_SLIDER };
static long osd_until;

static void save(void) {
    vx_settings_set_int(&config, "volume", (int)vol.volume);
    vx_settings_set_bool(&config, "muted", vol.muted != 0);
    vx_settings_save(&config);
}

static void apply(void) {
    if (audio >= 0) {
        vx_control(audio, VX_AUDIO_SET_VOLUME, &vol, sizeof(vol));
        last_changes = vol.changes;
    }
    printf("desktop: volume %u%s\n", vol.volume, vol.muted ? " (muted)" : "");
}

static void read_state(void) {
    struct vx_audio_volume now;
    if (audio < 0 || vx_control(audio, VX_AUDIO_GET_VOLUME, &now, sizeof(now)) < 0) {
        return;
    }
    if (now.changes != last_changes) {
        last_changes = now.changes;
        vol = now;
        vx_control(audio, VX_AUDIO_OUTPUTS, &outs, sizeof(outs));
        add_damage(panel_rect());
        if (volume_open) {
            add_damage(volume_rect());
        }
    }
}

void volume_check(void) {
    long now = now_ms();
    if (now < next_check) {
        return;
    }
    next_check = now + 500;
    if (audio < 0) {
        audio = vx_open("/dev/audio0", VX_OPEN_READ);
        if (audio < 0) {
            next_check = now + 2000; /* (A USB sound card may come.) */
            return;
        }
        /* The volume from last time, if there was one. */
        known = true;
        if (vx_settings_get(&config, "volume", NULL)) {
            vol.volume = (unsigned)vx_settings_int(&config, "volume", 80);
            vol.muted = vx_settings_bool(&config, "muted", false);
            vx_control(audio, VX_AUDIO_SET_VOLUME, &vol, sizeof(vol));
        }
        last_changes = ~0u;
    }
    read_state();
}

static void set_volume(int volume, bool muted) {
    vol.volume = (unsigned)(volume < 0 ? 0 : volume > 100 ? 100 : volume);
    vol.muted = muted;
    apply();
    save();
    add_damage(panel_rect());
    if (volume_open) {
        add_damage(volume_rect());
    }
}

/* ---- Drawing ---- */

static int isqrt(int n) {
    int r = 0;
    while ((r + 1) * (r + 1) <= n) {
        r++;
    }
    return r;
}

/* A speaker: a box and a cone, then up to three waves (or a cross: muted). */
static void draw_speaker(struct vx_surface *view, int x, int y, int size, uint32_t color,
                         unsigned volume, bool muted) {
    int u = size >= 20 ? 2 : 1; /* (Bigger in the bubble.) */
    int mid = y + 8 * u;
    vx_fill(view, x, mid - 2 * u, 3 * u, 5 * u, color);
    for (int i = 0; i < 5 * u; i++) {
        vx_fill(view, x + 3 * u + i, mid - 2 * u - i * 3 / 5 - u, 1, 5 * u + i * 6 / 5 + u, color);
    }
    int cx = x + 7 * u;
    if (muted) {
        for (int t = 0; t < u + 1; t++) {
            for (int i = 0; i <= 5 * u; i++) {
                vx_fill(view, cx + 4 * u + i, mid - 5 * u / 2 + i + t - u, 1, 1, color);
                vx_fill(view, cx + 4 * u + i, mid + 5 * u / 2 - i + t - u, 1, 1, color);
            }
        }
        return;
    }
    int waves = volume == 0 ? 0 : volume < 34 ? 1 : volume < 67 ? 2 : 3;
    for (int w = 0; w < waves; w++) {
        int r = (4 + w * 3) * u;
        for (int dy = -r * 7 / 10; dy <= r * 7 / 10; dy++) {
            int dx = isqrt(r * r - dy * dy);
            vx_fill(view, cx + dx, mid + dy, u, 1, color);
        }
    }
}

struct rect volume_button_rect(void) {
    struct rect c = clock_button_rect();
    return (struct rect){c.x - BUTTON_WIDTH - 2, 2, BUTTON_WIDTH, PANEL_HEIGHT - 4};
}

void volume_draw_button(struct vx_surface *view, int ox, int oy) {
    if (!known) {
        return;
    }
    struct rect b = volume_button_rect();
    if (volume_open) {
        fill_gel(view, (struct rect){ox + b.x, oy + b.y, b.width, b.height}, 6, vx_theme.accent, 255);
    }
    draw_speaker(view, ox + b.x + 6, oy + b.y + 3, 16, volume_open ? 0xffffff : vx_theme.text,
                 vol.volume, vol.muted);
}

struct rect volume_rect(void) {
    struct rect b = volume_button_rect();
    int height = OUTPUTS_Y + 24 + (outs.count ? (int)outs.count : 1) * OUTPUT_ROW + 44;
    int x = b.x + b.width - WIDTH;
    return (struct rect){x < 8 ? 8 : x, PANEL_HEIGHT + 6, WIDTH, height};
}

static struct rect slider_rect(void) {
    struct rect r = volume_rect();
    return (struct rect){r.x + 44, r.y + SLIDER_Y, WIDTH - 44 - 56, 20};
}

static struct rect mute_rect(void) {
    struct rect r = volume_rect();
    return (struct rect){r.x + WIDTH - 62, r.y + 12, 48, 22};
}

static struct rect output_rect(int i) {
    struct rect r = volume_rect();
    return (struct rect){r.x + 8, r.y + OUTPUTS_Y + 24 + i * OUTPUT_ROW, WIDTH - 16, OUTPUT_ROW - 2};
}

static struct rect settings_rect(void) {
    struct rect r = volume_rect();
    return (struct rect){r.x + 12, r.y + r.height - 38, WIDTH - 24, 26};
}

void volume_draw(struct vx_surface *view, int ox, int oy) {
    struct rect r = volume_rect();
    struct rect s = {r.x + ox, r.y + oy, r.width, r.height};
    draw_glass_popup(view, s, 12, 150);
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 13);
    vx_text(view, bold, s.x + 14, s.y + 14, "Sound", vx_theme.text, VX_TRANSPARENT);
    /* Mute: a little switch. */
    struct rect m = mute_rect();
    m.x += ox, m.y += oy;
    vx_draw_text(view, m.x - vx_text_width("Mute") - 8, m.y + 3, "Mute", vx_theme.dim,
                 VX_TRANSPARENT);
    if (vol.muted) {
        vx_draw_gel(view, m.x, m.y + 2, 36, 18, 9, vx_theme.accent);
    } else {
        fill_rounded(view, (struct rect){m.x, m.y + 2, 36, 18}, 9, vx_mix(vx_theme.line, 0x000000, 40));
        fill_rounded(view, (struct rect){m.x + 1, m.y + 3, 34, 16}, 8, vx_theme.button_hot);
    }
    vx_draw_gel(view, vol.muted ? m.x + 19 : m.x + 1, m.y + 3, 16, 16, 8, 0xf4f5f8);
    /* The slider, between a quiet speaker and a loud one. */
    struct rect t = slider_rect();
    t.x += ox, t.y += oy;
    draw_speaker(view, s.x + 14, t.y + 1, 16, vx_theme.dim, 0, false);
    draw_speaker(view, t.x + t.width + 12, t.y + 1, 16, vx_theme.dim, 100, false);
    int track_y = t.y + 7;
    fill_rounded(view, (struct rect){t.x, track_y, t.width, 6}, 3, vx_mix(vx_theme.line, 0x000000, 40));
    int at = t.x + (int)vol.volume * (t.width - 16) / 100;
    if (!vol.muted && at + 8 > t.x + 3) {
        vx_draw_gel(view, t.x, track_y, at + 8 - t.x, 6, 3, vx_theme.accent);
    }
    vx_draw_gel(view, at, t.y + 2, 16, 16, 8, 0xf4f5f8);
    /* The outputs: the one playing has a dot. */
    vx_fill(view, s.x + 12, s.y + OUTPUTS_Y - 4, WIDTH - 24, 1, vx_mix(vx_theme.line, vx_theme.menu, 80));
    vx_text(view, bold, s.x + 14, s.y + OUTPUTS_Y + 2, "Output", vx_theme.text, VX_TRANSPARENT);
    if (!outs.count) {
        vx_draw_text(view, s.x + 16, s.y + OUTPUTS_Y + 28, "No sound device", vx_theme.dim,
                     VX_TRANSPARENT);
    }
    for (unsigned i = 0; i < outs.count; i++) {
        struct rect o = output_rect((int)i);
        o.x += ox, o.y += oy;
        bool on = outs.output[i].id == outs.current;
        if (hot == (int)i) {
            vx_draw_gel(view, o.x, o.y, o.width, o.height, 6, vx_theme.accent);
        }
        uint32_t text = hot == (int)i ? 0xffffff : vx_theme.text;
        if (on) {
            draw_orb(view, o.x + 14, o.y + o.height / 2, 5, hot == (int)i ? 0xffffff : vx_theme.accent);
        }
        vx_draw_text_fit(view, o.x + 28, o.y + 5, o.width - 36, outs.output[i].name, text,
                         VX_TRANSPARENT);
    }
    struct rect g = settings_rect();
    vx_draw_button(view, g.x + ox, g.y + oy, g.width, g.height, "Sound Settings...",
                   hot == HOT_SETTINGS);
}

void volume_draw_osd(struct vx_surface *view, int ox, int oy) {
    if (now_ms() >= osd_until) {
        return;
    }
    struct rect r = {(screen.width - OSD_SIZE) / 2 + ox, screen.height - OSD_SIZE - 120 + oy,
                     OSD_SIZE, OSD_SIZE};
    draw_glass(view, r, 24, 24, vx_theme.dark ? 0x121214 : 0x303040, 150, 40);
    draw_speaker(view, r.x + 52, r.y + 40, 40, 0xffffff, vol.volume, vol.muted);
    /* 16 little steps. */
    int lit = vol.muted ? 0 : ((int)vol.volume * 16 + 50) / 100;
    for (int i = 0; i < 16; i++) {
        struct rect step = {r.x + 22 + i * 9, r.y + OSD_SIZE - 34, 7, 10};
        fill_rounded(view, step, 2, i < lit ? 0xffffff : 0x505060);
    }
}

static struct rect osd_rect(void) {
    return (struct rect){(screen.width - OSD_SIZE) / 2 - SHADOW, screen.height - OSD_SIZE - 120 - SHADOW,
                         OSD_SIZE + 2 * SHADOW, OSD_SIZE + 2 * SHADOW};
}

/* While the bubble shows: when to look again (-1 if it isn't showing). */
long volume_osd_wait(void) {
    long left = osd_until - now_ms();
    if (osd_until && left <= 0) {
        osd_until = 0;
        add_damage(osd_rect());
        return -1;
    }
    return osd_until ? left : -1;
}

static void show_osd(void) {
    osd_until = now_ms() + OSD_MS;
    add_damage(osd_rect());
}

/* ---- Input ---- */

bool volume_key(int key, int value) {
    if ((key != KEY_MUTE && key != KEY_VOLUME_DOWN && key != KEY_VOLUME_UP) || value == 0) {
        return key == KEY_MUTE || key == KEY_VOLUME_DOWN || key == KEY_VOLUME_UP;
    }
    if (!known) {
        return true;
    }
    if (key == KEY_MUTE) {
        if (value == 1) {
            set_volume((int)vol.volume, !vol.muted);
        }
    } else {
        int step = key == KEY_VOLUME_UP ? STEP : -STEP;
        set_volume((int)vol.volume + step, false);
    }
    show_osd();
    return true;
}

void volume_wheel(int wheel) {
    if (known && wheel) {
        set_volume((int)vol.volume + (wheel > 0 ? STEP : -STEP), false);
    }
}

void volume_toggle(void) {
    volume_open = !volume_open;
    hot = -1;
    if (volume_open && audio >= 0) {
        vx_control(audio, VX_AUDIO_OUTPUTS, &outs, sizeof(outs));
    }
    struct rect r = volume_rect();
    add_damage((struct rect){r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW,
                             r.height + 2 * SHADOW + OUTPUT_ROW * VX_AUDIO_MAX_OUTPUTS});
    add_damage(panel_rect());
}

void volume_close(void) {
    if (volume_open) {
        volume_toggle();
    }
    sliding = false;
}

static void slide_to(int x) {
    struct rect t = slider_rect();
    int v = (x - t.x - 8) * 100 / (t.width - 16);
    set_volume(v, false);
}

void volume_button(bool down) {
    if (!down) {
        sliding = false;
        return;
    }
    if (inside(slider_rect(), pointer_x, pointer_y)) {
        sliding = true;
        slide_to(pointer_x);
    } else if (inside(mute_rect(), pointer_x, pointer_y)) {
        set_volume((int)vol.volume, !vol.muted);
    } else if (inside(settings_rect(), pointer_x, pointer_y)) {
        volume_close();
        run_named("Settings", "Sound");
    } else {
        for (unsigned i = 0; i < outs.count; i++) {
            if (inside(output_rect((int)i), pointer_x, pointer_y)) {
                unsigned id = outs.output[i].id;
                if (audio >= 0 && vx_control(audio, VX_AUDIO_SET_OUTPUT, &id, sizeof(id)) == 0) {
                    printf("desktop: sound output \"%s\"\n", outs.output[i].name);
                    outs.current = id;
                    add_damage(volume_rect());
                }
            }
        }
    }
}

void volume_pointer(void) {
    if (sliding) {
        slide_to(pointer_x);
        return;
    }
    int now = -1;
    if (inside(settings_rect(), pointer_x, pointer_y)) {
        now = HOT_SETTINGS;
    }
    for (unsigned i = 0; i < outs.count; i++) {
        if (inside(output_rect((int)i), pointer_x, pointer_y)) {
            now = (int)i;
        }
    }
    if (now != hot) {
        hot = now;
        add_damage(volume_rect());
    }
}

bool volume_sliding(void) {
    return sliding;
}
