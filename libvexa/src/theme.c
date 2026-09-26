#include <string.h>
#include <vexa/desktop.h>
#include <vexa/gui.h>
#include <vexa/settings.h>

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
    uint32_t color = vx_accents[0].color;
    for (int i = 0; i < vx_accent_count; i++) {
        if (accent && !strcmp(accent, vx_accents[i].name)) {
            color = vx_accents[i].color;
        }
    }
    t->accent = color;
    t->dark = !name || strcmp(name, "light") != 0;
    if (t->dark) {
        t->window = 0x1a1030;
        t->view = 0x120b22;
        t->text = 0xe4dcf2;
        t->dim = 0x8a80a3;
        t->selected = vx_mix(0x120b22, color, 115);
        t->button = 0x2c1d4a;
        t->button_hot = 0x3f2a66;
        t->line = 0x3a2a5c;
        t->sidebar = 0x140c26;
        t->stripe = 0x160e29;
        t->shadow = 0x06030c;
        t->panel = 0x140c24;
        t->menu = 0x1c1230;
        t->title = 0x2c1d4a;
        t->title_text = 0xe4dcf2;
    } else {
        t->window = 0xf3f1f7;
        t->view = 0xffffff;
        t->text = 0x1d1830;
        t->dim = 0x6f6885;
        t->selected = vx_mix(0xffffff, color, 90);
        t->button = 0xe6e2ee;
        t->button_hot = 0xd9d3e6;
        t->line = 0xcfc9dc;
        t->sidebar = 0xeae7f1;
        t->stripe = 0xf8f7fb;
        t->shadow = 0x9d97ab;
        t->panel = 0xe7e4ee;
        t->menu = 0xfbfaff;
        t->title = 0xe2dfe9;
        t->title_text = 0x1d1830;
    }
    t->title_focused = t->selected;
}

struct vx_theme vx_theme = {
    true, 0x1a1030, 0x120b22, 0xe4dcf2, 0x8a80a3, 0xb07cff, 0x5b3a96, 0x2c1d4a, 0x3f2a66,
    0x3a2a5c, 0x140c26, 0x160e29, 0x06030c, 0x140c24, 0x1c1230, 0x2c1d4a, 0x5b3a96, 0xe4dcf2,
};

void vx_theme_load(void) {
    struct vx_settings s;
    vx_settings_load(&s, "desktop.conf");
    vx_theme_make(&vx_theme, vx_settings_get(&s, "theme", "dark"),
                  vx_settings_get(&s, "accent", "purple"));
}
