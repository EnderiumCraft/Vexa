/* The screensaver, the lock screen and the login screen.
 *
 * After some minutes without the keyboard or the mouse (Settings, Lock
 * Screen: "screensaver_minutes"), the screen goes dark, with the time
 * drifting over a blurred wallpaper. Any key or movement wakes it; with
 * "lock_on_wake" (or the Vexa menu's Lock Screen, Super+L) the lock screen
 * comes first: the time and date, and the account's password (if it has
 * one) or any key to go back to the desktop.
 *
 * The login screen looks the same, with the accounts to choose from (click
 * one, or Left and Right) above the password. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

bool locked, saver_on;
static int saver_minutes = 10;
static bool lock_on_wake;
static char typed[64];
/* The login screen: the accounts, which one is chosen, and whether each
 * has a password. */
static bool login_mode;
static struct vx_user login_users[VX_USERS_MAX];
static bool login_password[VX_USERS_MAX];
static int login_count, login_chosen;
static long last_input, wrong_at = -1;
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
}

/* Whether getting in needs a password: the chosen account's, or the
 * session's. */
static bool needs_password(void) {
    return login_mode ? login_password[login_chosen] : session_has_password;
}

void login_begin(const struct vx_user *users, int count) {
    login_count = count < VX_USERS_MAX ? count : VX_USERS_MAX;
    for (int i = 0; i < login_count; i++) {
        login_users[i] = users[i];
        login_password[i] = vx_user_has_password(users[i].name);
    }
    login_chosen = 0;
    login_mode = locked = true;
    saver_on = false;
    typed[0] = '\0';
    wrong_at = -1;
    if (!blurred.pixels || blurred.width != screen.width || blurred.height != screen.height) {
        make_blurred(&blurred, &wallpaper, 110);
    }
    damage_all();
    printf("desktop: login screen (%d accounts)\n", login_count);
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
    /* (The password may have changed since the session started.) */
    session_has_password = !vx_password_check(session_user.name, "");
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
    if (login_mode) {
        login_mode = false;
        printf("desktop: logged in as %s\n", login_users[login_chosen].name);
        session_start(&login_users[login_chosen]);
        return;
    }
    printf("desktop: unlocked\n");
}

static void choose(int index) {
    if (index >= 0 && index < login_count && index != login_chosen) {
        login_chosen = index;
        typed[0] = '\0';
        wrong_at = -1;
        damage_all();
    }
}

bool lock_input(void) {
    last_input = now_ms();
    if (saver_on) {
        saver_on = false;
        if (lock_on_wake && !login_mode) {
            session_has_password = !vx_password_check(session_user.name, "");
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
    if (login_mode && (key == VX_KEY_LEFT || key == VX_KEY_RIGHT)) {
        choose(login_chosen + (key == VX_KEY_LEFT ? -1 : 1));
        return;
    }
    if (!needs_password()) {
        if (key == VX_KEY_ENTER || key == VX_KEY_SPACE || (!login_mode && (key == VX_KEY_ESC || character))) {
            unlock();
        }
        return;
    }
    if (key == VX_KEY_ENTER) {
        const char *name = login_mode ? login_users[login_chosen].name : session_user.name;
        if (vx_password_check(name, typed)) {
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

/* Where account `i`'s picture is on the login screen. */
static struct rect avatar_rect(int i) {
    int size = 64, gap = 40;
    int total = login_count * size + (login_count - 1) * gap;
    return (struct rect){screen.width / 2 - total / 2 + i * (size + gap),
                         screen.height * 2 / 3 - 40, size, size};
}

void lock_button(bool down) {
    if (!down) {
        return;
    }
    if (login_mode) {
        for (int i = 0; i < login_count; i++) {
            struct rect r = avatar_rect(i);
            if (pointer_x >= r.x - 20 && pointer_x < r.x + r.width + 20 && pointer_y >= r.y &&
                pointer_y < r.y + r.height + 24) {
                if (i == login_chosen && !needs_password()) {
                    unlock();
                } else {
                    choose(i);
                }
                return;
            }
        }
        return;
    }
    if (!needs_password()) {
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

    /* Who's here (the accounts to choose from, or whose screen it is), and
     * how to get in. */
    int y = screen.height * 2 / 3 - 40 + oy;
    int shown = login_mode ? login_count : 1;
    for (int i = 0; i < shown; i++) {
        const struct vx_user *user = login_mode ? &login_users[i] : &session_user;
        struct rect avatar = login_mode ? avatar_rect(i)
                                        : (struct rect){screen.width / 2 - 32, y - oy, 64, 64};
        avatar.x += ox;
        avatar.y += oy;
        bool chosen = !login_mode || i == login_chosen;
        if (chosen && login_mode && login_count > 1) {
            fill_rounded(view, (struct rect){avatar.x - 4, avatar.y - 4, 72, 72}, 36, 0xffffff);
        }
        fill_rounded(view, avatar, 32, chosen ? vx_theme.accent : 0x707080);
        const char *name = user->full_name[0] ? user->full_name : user->name;
        char initial[8] = "V";
        if (name[0]) {
            initial[0] = (char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]);
            initial[1] = '\0';
        }
        const struct vx_font *letter = vx_font(VX_FACE_BOLD, 30);
        int acx = avatar.x + 32;
        centered(view, letter, acx, avatar.y + 32 - vx_font_height(letter) / 2, initial, 0xffffff);
        centered(view, vx_font(VX_FACE_BOLD, 15), acx, avatar.y + 72, name,
                 chosen ? 0xffffff : 0xc8c8d0);
    }
    y += 102;
    if (!needs_password()) {
        centered(view, vx_font_ui(), cx, y + 8,
                 login_mode ? "Press Enter or click to log in" : "Press any key or click to unlock",
                 0xe0e0e8);
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
        centered(view, vx_font_ui(), cx, y + 46, login_mode ? "Enter logs in" : "Enter unlocks",
                 0xd0d0d8);
    }
}
