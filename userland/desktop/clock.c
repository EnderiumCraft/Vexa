/* Under the clock (a click on it): the time and date, a month's calendar
 * (the arrows go to other months, the month's name back to this one), and
 * the notifications seen lately, which "Clear" takes away. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

#define WIDTH 320
#define CELL 40
#define HISTORY 20
#define NOTE_ROW 46
#define NOTES_SHOWN 5
/* Where things are, from the popup's top. */
#define CALENDAR_TOP 76
#define ROW_H (CELL * 3 / 4)

bool clock_open;
int unread_notes;
static int year, month; /* The month shown. */
static int hot = -1;    /* What the pointer is on (HOT_*). */
enum { HOT_PREVIOUS, HOT_NEXT, HOT_TODAY, HOT_CLEAR };

static struct {
    char text[104];
    long time;
} history[HISTORY];
static int history_count;

void clock_remember(const char *text) {
    if (history_count == HISTORY) {
        memmove(history, history + 1, (HISTORY - 1) * sizeof(history[0]));
        history_count--;
    }
    snprintf(history[history_count].text, sizeof(history[0].text), "%s", text);
    history[history_count].time = vx_time();
    history_count++;
    if (!clock_open) {
        unread_notes++;
        add_damage(panel_rect());
    } else {
        add_damage(clock_rect());
    }
}

static int notes_height(void) {
    int shown = history_count < NOTES_SHOWN ? history_count : NOTES_SHOWN;
    return 34 + (shown ? shown * NOTE_ROW : 28) + 8;
}

struct rect clock_rect(void) {
    int height = CALENDAR_TOP + 34 + 22 + 6 * ROW_H + 12 + notes_height();
    return (struct rect){screen.width - WIDTH - 8, PANEL_HEIGHT + 6, WIDTH, height};
}

static struct rect damage_rect(void) {
    struct rect r = clock_rect();
    return (struct rect){r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW,
                         r.height + 2 * SHADOW + 6 + NOTE_ROW * NOTES_SHOWN};
}

static void this_month(void) {
    struct vx_date d;
    if (local_date(&d)) {
        year = d.year;
        month = d.month;
    } else {
        year = 2026;
        month = 1;
    }
}

void clock_toggle(void) {
    if (clock_open) {
        clock_close();
        return;
    }
    this_month();
    clock_open = true;
    unread_notes = 0;
    add_damage(damage_rect());
    add_damage(panel_rect());
    printf("desktop: calendar for %s %d\n", vx_month_names[month - 1], year);
}

void clock_close(void) {
    if (clock_open) {
        add_damage(damage_rect());
        clock_open = false;
    }
}

static int days_in(int y, int m) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

static struct rect arrow_rect(int which) {
    struct rect r = clock_rect();
    return (struct rect){which == HOT_PREVIOUS ? r.x + r.width - 64 : r.x + r.width - 34,
                         r.y + CALENDAR_TOP, 28, 26};
}

static struct rect title_rect(void) {
    struct rect r = clock_rect();
    return (struct rect){r.x + 14, r.y + CALENDAR_TOP, r.width - 90, 26};
}

static int notes_top(void) {
    return clock_rect().y + CALENDAR_TOP + 34 + 22 + 6 * ROW_H + 12;
}

static struct rect clear_rect(void) {
    struct rect r = clock_rect();
    return (struct rect){r.x + r.width - 70, notes_top() + 4, 56, 24};
}

static int hot_at(int x, int y) {
    if (inside(arrow_rect(HOT_PREVIOUS), x, y)) {
        return HOT_PREVIOUS;
    }
    if (inside(arrow_rect(HOT_NEXT), x, y)) {
        return HOT_NEXT;
    }
    if (inside(title_rect(), x, y)) {
        return HOT_TODAY;
    }
    if (history_count && inside(clear_rect(), x, y)) {
        return HOT_CLEAR;
    }
    return -1;
}

void clock_button(bool down) {
    if (!down) {
        return;
    }
    if (!inside(clock_rect(), pointer_x, pointer_y)) {
        clock_close();
        return;
    }
    switch (hot_at(pointer_x, pointer_y)) {
    case HOT_PREVIOUS:
        month = month == 1 ? (year--, 12) : month - 1;
        break;
    case HOT_NEXT:
        month = month == 12 ? (year++, 1) : month + 1;
        break;
    case HOT_TODAY: this_month(); break;
    case HOT_CLEAR:
        history_count = 0;
        printf("desktop: notifications cleared\n");
        break;
    default: return;
    }
    add_damage(damage_rect());
}

void clock_pointer(void) {
    int h = hot_at(pointer_x, pointer_y);
    if (h != hot) {
        hot = h;
        add_damage(clock_rect());
    }
}

/* "5 min ago", "2 h ago", or the time. */
static void ago(long when, char *out, size_t size) {
    long seconds = vx_time() - when;
    if (seconds < 60) {
        snprintf(out, size, "now");
    } else if (seconds < 3600) {
        snprintf(out, size, "%ld min ago", seconds / 60);
    } else {
        snprintf(out, size, "%ld h ago", seconds / 3600);
    }
}

static void draw_arrow(struct vx_surface *view, struct rect r, bool left, bool lit) {
    if (lit) {
        fill_rounded(view, r, 6, vx_theme.button_hot);
    }
    int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    for (int i = 0; i < 5; i++) {
        int x = left ? cx + 2 - i : cx - 2 + i;
        vx_fill(view, x, cy - 4 + i, 2, 1, vx_theme.text);
        vx_fill(view, x, cy + 4 - i, 2, 1, vx_theme.text);
    }
}

void clock_draw(struct vx_surface *view, int ox, int oy) {
    struct rect r = clock_rect();
    struct rect shifted = {r.x + ox, r.y + oy, r.width, r.height};
    draw_shadow(view, shifted, 150);
    fill_rounded(view, shifted, 12, vx_theme.menu);
    outline_rounded(view, shifted, 12, vx_theme.line);
    int x = r.x + ox, y = r.y + oy;

    /* The time, big, and the date. */
    struct vx_date d;
    bool known = local_date(&d);
    char line[80];
    if (known) {
        int hours = vx_settings_int(&config, "clock", 24);
        snprintf(line, sizeof(line), hours == 12 ? "%d:%02d" : "%02d:%02d",
                 hours == 12 ? (d.hour % 12 ? d.hour % 12 : 12) : d.hour, d.minute);
        int end = vx_text(view, vx_font(VX_FACE_SANS, 34), x + 14, y + 6, line, vx_theme.text,
                          VX_TRANSPARENT);
        if (hours == 12) {
            vx_draw_text(view, end + 4, y + 26, d.hour < 12 ? "am" : "pm", vx_theme.dim,
                         VX_TRANSPARENT);
        }
        snprintf(line, sizeof(line), "%s %d %s %d", vx_weekday_names[d.weekday], d.day,
                 vx_month_names[d.month - 1], d.year);
        vx_draw_text(view, x + 16, y + 50, line, vx_theme.dim, VX_TRANSPARENT);
    }
    vx_fill(view, x + 12, y + CALENDAR_TOP - 6, r.width - 24, 1, vx_theme.line);

    /* The month. */
    snprintf(line, sizeof(line), "%s %d", vx_month_names[month - 1], year);
    struct rect title = title_rect();
    if (hot == HOT_TODAY) {
        fill_rounded(view, (struct rect){title.x + ox - 4, title.y + oy, vx_text_width(line) + 12,
                                         title.height}, 6, vx_theme.button_hot);
    }
    vx_text(view, vx_font(VX_FACE_BOLD, 14), title.x + ox + 2, title.y + oy + 5, line,
            vx_theme.text, VX_TRANSPARENT);
    struct rect left = arrow_rect(HOT_PREVIOUS), right = arrow_rect(HOT_NEXT);
    left.x += ox, left.y += oy, right.x += ox, right.y += oy;
    draw_arrow(view, left, true, hot == HOT_PREVIOUS);
    draw_arrow(view, right, false, hot == HOT_NEXT);
    static const char *const days[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    int grid_x = x + 14, grid_w = (r.width - 28) / 7;
    int top = y + CALENDAR_TOP + 34;
    for (int i = 0; i < 7; i++) {
        int w = vx_text_width(days[i]);
        vx_draw_text(view, grid_x + i * grid_w + (grid_w - w) / 2, top, days[i],
                     i >= 5 ? vx_theme.accent : vx_theme.dim, VX_TRANSPARENT);
    }
    top += 22;
    struct vx_date first = {year, month, 1, 12, 0, 0, 0};
    struct vx_date check;
    vx_date_of(vx_seconds_of(&first), &check);
    int column = (check.weekday + 6) % 7; /* Monday first. */
    int count = days_in(year, month);
    for (int day = 1, row = 0; day <= count; day++) {
        int cx = grid_x + column * grid_w, cy = top + row * ROW_H;
        bool today = known && d.year == year && d.month == month && d.day == day;
        char number[4];
        snprintf(number, sizeof(number), "%d", day);
        int w = vx_text_width(number);
        if (today) {
            fill_rounded(view, (struct rect){cx + (grid_w - 26) / 2, cy - 1, 26, 26}, 13,
                         vx_theme.accent);
        }
        vx_draw_text(view, cx + (grid_w - w) / 2, cy + 4, number,
                     today ? 0xffffff : vx_theme.text, VX_TRANSPARENT);
        if (++column == 7) {
            column = 0;
            row++;
        }
    }

    /* The notifications. */
    int ny = notes_top() + oy;
    vx_fill(view, x + 12, ny - 4, r.width - 24, 1, vx_theme.line);
    vx_text(view, vx_font(VX_FACE_BOLD, 13), x + 14, ny + 8, "Notifications", vx_theme.text,
            VX_TRANSPARENT);
    if (history_count) {
        struct rect c = clear_rect();
        c.x += ox, c.y += oy;
        fill_rounded(view, c, 6, hot == HOT_CLEAR ? vx_theme.button_hot : vx_theme.button);
        vx_draw_text(view, c.x + (c.width - vx_text_width("Clear")) / 2, c.y + 4, "Clear",
                     vx_theme.text, VX_TRANSPARENT);
    }
    ny += 34;
    if (!history_count) {
        vx_draw_text(view, x + 14, ny + 4, "No notifications", vx_theme.dim, VX_TRANSPARENT);
        return;
    }
    /* The newest first. */
    for (int i = 0; i < NOTES_SHOWN && i < history_count; i++) {
        const char *text = history[history_count - 1 - i].text;
        char first[104], second[104] = "";
        const char *colon = strstr(text, ": ");
        if (colon) {
            snprintf(first, sizeof(first), "%.*s", (int)(colon - text), text);
            snprintf(second, sizeof(second), "%s", colon + 2);
        } else {
            snprintf(first, sizeof(first), "%s", text);
        }
        struct rect box = {x + 10, ny + i * NOTE_ROW, r.width - 20, NOTE_ROW - 6};
        fill_rounded(view, box, 8, vx_theme.view);
        char when[24];
        ago(history[history_count - 1 - i].time, when, sizeof(when));
        int ww = vx_text_width(when);
        vx_draw_text(view, box.x + box.width - ww - 8, box.y + 3, when, vx_theme.dim,
                     VX_TRANSPARENT);
        vx_draw_text_fit(view, box.x + 8, box.y + 3, box.width - ww - 24, first, vx_theme.accent,
                         VX_TRANSPARENT);
        vx_draw_text_fit(view, box.x + 8, box.y + 20, box.width - 16, second, vx_theme.text,
                         VX_TRANSPARENT);
    }
}
