#include <stdio.h>
#include <string.h>
#include <vexa/desktop.h>
#include <vexa/gui.h>
#include <vexa/settings.h>
#include <vexa/time.h>

/* The theme (see <vexa/gui.h>). */

const struct vx_accent vx_accents[] = {
    {"purple", "Purple", 0xb07cff}, {"blue", "Blue", 0x4c8dff},   {"teal", "Teal", 0x2ec4b6},
    {"green", "Green", 0x3fbf6f},   {"orange", "Orange", 0xff8c3a}, {"pink", "Pink", 0xff5fa2},
    {"red", "Red", 0xf0524f},       {"graphite", "Graphite", 0x8e8ea0},
};
const int vx_accent_count = sizeof(vx_accents) / sizeof(vx_accents[0]);

uint32_t vx_mix(uint32_t a, uint32_t b, int amount) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
        out |= (uint32_t)(ca + (cb - ca) * amount / 255) << shift;
    }
    return out;
}

void vx_theme_make(struct vx_theme *t, const char *name, const char *accent) {
    uint32_t color = vx_accents[1].color; /* Blue, unless another is named. */
    for (int i = 0; i < vx_accent_count; i++) {
        if (accent && !strcmp(accent, vx_accents[i].name)) {
            color = vx_accents[i].color;
        }
    }
    t->accent = color;
    t->dark = name && !strcmp(name, "dark");
    if (t->dark) {
        /* Neutral dark greys (no tint), so only the accent has a color. */
        t->window = 0x252527;
        t->view = 0x1c1c1e;
        t->text = 0xe8e8ea;
        t->dim = 0x8e8e93;
        t->selected = vx_mix(0x1c1c1e, color, 115);
        t->button = 0x3a3a3d;
        t->button_hot = 0x4a4a4e;
        t->line = 0x3d3d40;
        t->sidebar = 0x202022;
        t->stripe = 0x212123;
        t->shadow = 0x050505;
        t->panel = 0x1e1e20;
        t->menu = 0x2b2b2e;
        t->title = 0x333336;
        t->title_text = 0xe8e8ea;
    } else {
        /* Cool, quiet greys (like Aqua's), so the glass and the accent stand out. */
        t->window = 0xeef0f4;
        t->view = 0xffffff;
        t->text = 0x1a1d24;
        t->dim = 0x6b7080;
        t->selected = vx_mix(0xffffff, color, 90);
        t->button = 0xe2e5eb;
        t->button_hot = 0xd5dae3;
        t->line = 0xc4c9d3;
        t->sidebar = 0xe6e9ef;
        t->stripe = 0xf5f7fa;
        t->shadow = 0x9aa0ab;
        t->panel = 0xe9ecf2;
        t->menu = 0xf8f9fc;
        t->title = 0xe0e3ea;
        t->title_text = 0x1a1d24;
    }
    t->title_focused = t->selected;
}

/* (Until vx_theme_load: the default, light with blue.) */
struct vx_theme vx_theme = {
    false, 0xeef0f4, 0xffffff, 0x1a1d24, 0x6b7080, 0x4c8dff, 0xbfd6ff, 0xe2e5eb, 0xd5dae3,
    0xc4c9d3, 0xe6e9ef, 0xf5f7fa, 0x9aa0ab, 0xe9ecf2, 0xf8f9fc, 0xe0e3ea, 0xbfd6ff, 0x1a1d24,
};

void vx_theme_wallpaper(const struct vx_theme *t, char *out, size_t size) {
    const char *accent = "blue";
    for (int i = 0; i < vx_accent_count; i++) {
        if (t->accent == vx_accents[i].color) {
            accent = vx_accents[i].name;
        }
    }
    snprintf(out, size, "/share/pictures/glass/%s-%s.png", accent, t->dark ? "dark" : "light");
}

bool vx_theme_night(void) {
    struct vx_date now;
    vx_local_now(&now);
    return now.hour < VX_THEME_DAY_STARTS || now.hour >= VX_THEME_NIGHT_STARTS;
}

void vx_theme_load(void) {
    struct vx_settings s;
    vx_settings_load(&s, "desktop.conf");
    const char *name = vx_settings_get(&s, "theme", "light");
    if (!strcmp(name, "auto")) {
        name = vx_theme_night() ? "dark" : "light";
    }
    vx_theme_make(&vx_theme, name, vx_settings_get(&s, "accent", "blue"));
}
