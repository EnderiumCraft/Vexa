/* The screensaver and the lock screen.
 *
 * After some minutes without the keyboard or the mouse (Settings, Lock
 * Screen: "screensaver_minutes"), the screen goes dark, with the time
 * drifting over a blurred wallpaper. Any key or movement wakes it; with
 * "lock_on_wake" (or the Vexa menu's Lock Screen, Super+L) the lock screen
 * comes first: the time and date, and the password (if one is set:
 * "lock_password", its hash) or any key to go back to the desktop. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

bool locked, saver_on;
static int saver_minutes = 10;
static bool lock_on_wake;
static char password_hash[24];
static char typed[64];
static long last_input, wrong_at = -1, unlocked_minute = -1;
static struct vx_surface blurred;
static int saver_x, saver_y, saver_dx = 1, saver_dy = 1;
static long saver_moved;
static long shown_minute = -1;

#define SAVER_W 360
#define SAVER_H 110
#define SHAKE_MS 400

void lock_settings(void) {
    saver_minutes = vx_settings_int(&config, "screensaver_minutes", 10);
    lock_on_wake = vx_settings_bool(&config, "lock_on_wake", false);
    snprintf(password_hash, sizeof(password_hash), "%s",
             vx_settings_get(&config, "lock_password", ""));
}

void lock_screen_changed(void) {
    if (blurred.pixels) {
        vx_unmap(blurred.pixels, (size_t)blurred.stride * blurred.height * 4);
        blurred.pixels = NULL;
    }
    if (locked || saver_on) {
        make_blurred(&blurred, &wallpaper, 110);
        damage_all();
    }
}

static void cover(void) {
    if (!blurred.pixels || blurred.width != screen.width || blurred.height != screen.height) {
        make_blurred(&blurred, &wallpaper, 110);
    }
    damage_all();
}

void lock_now(void) {
    if (locked) {
        return;
    }
    locked = true;
    saver_on = false;
    typed[0] = '\0';
    wrong_at = -1;
    cover();
    printf("desktop: locked\n");
}

static void unlock(void) {
    locked = false;
    typed[0] = '\0';
    damage_all();
    printf("desktop: unlocked\n");
}

bool lock_input(void) {
    last_input = now_ms();
    if (saver_on) {
        saver_on = false;
        if (lock_on_wake) {
            locked = true;
            typed[0] = '\0';
            printf("desktop: locked\n");
        }
        damage_all();
        printf("desktop: screensaver off\n");
        return false; /* Waking it is all the event does. */
    }
    return true;
}

void lock_key(int key, int value, int character) {
    if (!value) {
        return;
    }
    damage_all();
    if (!password_hash[0]) {
        if (key == VX_KEY_ENTER || key == VX_KEY_SPACE || key == VX_KEY_ESC || character) {
            unlock();
        }
        return;
    }
    if (key == VX_KEY_ENTER) {
        char hash[17];
        vx_password_hash(typed, hash);
        if (!strcmp(hash, password_hash)) {
            unlock();
        } else {
            printf("desktop: wrong password\n");
            wrong_at = now_ms();
            typed[0] = '\0';
            want_frames();
        }
        return;
    }
    if (key == VX_KEY_ESC) {
        typed[0] = '\0';
        return;
    }
    struct vx_gui_event e = {.type = VX_GUI_KEY, .key = key, .value = value, .character = character};
    vx_field_key(typed, sizeof(typed), &e);
}

void lock_button(bool down) {
    if (down && !password_hash[0]) {
        unlock();
    }
}

void lock_tick(void) {
    long now = now_ms();
    if (!locked && !saver_on && saver_minutes > 0 && now - last_input > saver_minutes * 60000L) {
        saver_on = true;
        saver_x = (int)(now % (screen.width - SAVER_W > 1 ? screen.width - SAVER_W : 1));
        saver_y = screen.height / 3;
        saver_moved = now;
        cover();
        printf("desktop: screensaver on\n");
        return;
    }
    if (saver_on) {
        /* The time drifts, bouncing off the edges (about 30 pixels a second). */
        long steps = (now - saver_moved) / 33;
        if (steps > 0) {
            add_damage((struct rect){saver_x, saver_y, SAVER_W, SAVER_H});
            saver_moved += steps * 33;
            while (steps-- > 0) {
                if (saver_x + saver_dx < 0 || saver_x + saver_dx + SAVER_W > screen.width) {
                    saver_dx = -saver_dx;
                }
                if (saver_y + saver_dy < 0 || saver_y + saver_dy + SAVER_H > screen.height) {
                    saver_dy = -saver_dy;
                }
                saver_x += saver_dx;
                saver_y += saver_dy;
            }
            add_damage((struct rect){saver_x, saver_y, SAVER_W, SAVER_H});
        }
    }
    if (locked && wrong_at >= 0) {
        damage_all();
        if (now - wrong_at < SHAKE_MS) {
            want_frames();
        } else if (now - wrong_at > 2500) {
            wrong_at = -1;
        }
    }
    long minute = vx_time() / 60;
    if ((locked || saver_on) && minute != shown_minute) {
        shown_minute = minute;
        damage_all();
    }
    (void)unlocked_minute;
}

long lock_wait_ms(void) {
    if (saver_on) {
        return 33;
    }
    if (locked) {
        return wrong_at >= 0 ? 30 : 1000;
    }
    if (saver_minutes <= 0) {
        return 60000;
    }
    long left = saver_minutes * 60000L - (now_ms() - last_input);
    return left < 1 ? 1 : left;
}

static void centered(struct vx_surface *view, const struct vx_font *f, int cx, int y,
                     const char *text, uint32_t color) {
    vx_text(view, f, cx - vx_text_width_font(f, text) / 2, y, text, color, VX_TRANSPARENT);
}

void lock_draw(struct vx_surface *view, int ox, int oy) {
    if (blurred.pixels) {
        vx_blit(view, ox, oy, &blurred, 0, 0, blurred.width, blurred.height);
    } else {
        vx_fill(view, ox, oy, screen.width, screen.height, 0x000000);
    }
    struct vx_date d;
    bool known = local_date(&d);
    char time_text[16] = "", date_text[64] = "";
    if (known) {
        int hours = vx_settings_int(&config, "clock", 24);
        snprintf(time_text, sizeof(time_text), hours == 12 ? "%d:%02d" : "%02d:%02d",
                 hours == 12 ? (d.hour % 12 ? d.hour % 12 : 12) : d.hour, d.minute);
        snprintf(date_text, sizeof(date_text), "%s %d %s", vx_weekday_names[d.weekday], d.day,
                 vx_month_names[d.month - 1]);
    }
    if (saver_on) {
        int cx = saver_x + SAVER_W / 2 + ox, y = saver_y + oy;
        centered(view, vx_font(VX_FACE_SANS, 72), cx, y, time_text, 0xe8e8f0);
        centered(view, vx_font(VX_FACE_SANS, 18), cx, y + 84, date_text, 0xb8b8c8);
        return;
    }
    int cx = screen.width / 2 + ox;
    int top = screen.height / 6 + oy;
    centered(view, vx_font(VX_FACE_SANS, 22), cx, top, date_text, 0xf0f0f8);
    centered(view, vx_font(VX_FACE_BOLD, 104), cx, top + 30, time_text, 0xffffff);

    /* Who's here: the computer's name, and how to get in. */
    char name[80] = "";
    vx_get_hostname(name, sizeof(name));
    int y = screen.height * 2 / 3 - 40 + oy;
    struct rect avatar = {cx - 32, y, 64, 64};
    fill_rounded(view, avatar, 32, vx_theme.accent);
    char initial[8] = "V";
    if (name[0]) {
        initial[0] = (char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]);
        initial[1] = '\0';
    }
    const struct vx_font *letter = vx_font(VX_FACE_BOLD, 30);
    centered(view, letter, cx, y + 32 - vx_font_height(letter) / 2, initial, 0xffffff);
    centered(view, vx_font(VX_FACE_BOLD, 15), cx, y + 72, name[0] ? name : "Vexa", 0xffffff);
    y += 102;
    if (!password_hash[0]) {
        centered(view, vx_font_ui(), cx, y + 8, "Press any key or click to unlock", 0xe0e0e8);
        return;
    }
    int shake = 0;
    if (wrong_at >= 0 && now_ms() - wrong_at < SHAKE_MS) {
        long t = now_ms() - wrong_at;
        int phase = (int)(t / 40 % 4);
        shake = (phase == 0 ? 8 : phase == 1 ? 0 : phase == 2 ? -8 : 0) *
                (int)(SHAKE_MS - t) / SHAKE_MS;
    }
    struct rect field = {cx - 120 + shake, y, 240, 34};
    blend_rect(view, field, 0xffffff, 60);
    outline_rounded(view, field, 10, 0xffffff);
    if (typed[0]) {
        /* A dot for each character. */
        int n = 0;
        for (const char *p = typed; *p; n++) {
            vx_utf8_next(&p);
        }
        n = n > 24 ? 24 : n;
        int start = field.x + 16;
        for (int i = 0; i < n; i++) {
            fill_rounded(view, (struct rect){start + i * 12, field.y + 13, 8, 8}, 4, 0xffffff);
        }
    } else {
        vx_draw_text(view, field.x + 14, field.y + 9, "Password", 0xd0d0d8, VX_TRANSPARENT);
    }
    if (wrong_at >= 0) {
        centered(view, vx_font_ui(), cx, y + 46, "Wrong password", 0xff8f9f);
    } else {
        centered(view, vx_font_ui(), cx, y + 46, "Enter unlocks", 0xd0d0d8);
    }
}
