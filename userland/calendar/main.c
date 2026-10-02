/* calendar: the Calendar. A month, with today marked and dots on days that
 * have events; a click picks a day, whose events are listed on the right,
 * where new ones are added (a time is optional: "09:30 Dentist").
 * Events are kept in /home/.calendar, a line each: "2026-10-02 09:30 Dentist".
 *
 *     the arrows, Page Up/Down   the previous, next month (T: today)
 *     the arrow keys             another day
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>
#include <vexa/time.h>

#define WIDTH 780
#define HEIGHT 520
#define PANEL 260  /* The day's events, on the right. */
#define HEADER 56
#define EVENTS_FILE "/home/.calendar"
#define MAX_EVENTS 512

struct event {
    int year, month, day;
    char text[120]; /* With the time first, if it has one. */
};

static struct vx_window *window;
static struct event events[MAX_EVENTS];
static int event_count;
static int year, month, day; /* Shown, and the day picked. */
static struct vx_date today;
static char typed[120];
static bool typing;
static int hot = -1;
enum { HOT_PREVIOUS, HOT_NEXT, HOT_TODAY, HOT_ADD, HOT_FIELD };

static int days_in(int y, int m) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

static int weekday_of(int y, int m, int d) {
    struct vx_date date = {y, m, d, 12, 0, 0, 0};
    struct vx_date check;
    vx_date_of(vx_seconds_of(&date), &check);
    return check.weekday;
}

static int compare_events(const void *a, const void *b) {
    const struct event *x = a, *y = b;
    if (x->year != y->year) {
        return x->year - y->year;
    }
    if (x->month != y->month) {
        return x->month - y->month;
    }
    if (x->day != y->day) {
        return x->day - y->day;
    }
    return strcmp(x->text, y->text);
}

static void load(void) {
    event_count = 0;
    int handle = vx_open(EVENTS_FILE, VX_OPEN_READ);
    if (handle < 0) {
        return;
    }
    static char text[MAX_EVENTS * 140];
    long n = vx_read(handle, text, sizeof(text) - 1);
    vx_close(handle);
    text[n > 0 ? n : 0] = '\0';
    for (char *line = text, *next; line && *line && event_count < MAX_EVENTS; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        struct event *e = &events[event_count];
        if (strlen(line) > 11 && line[4] == '-' && line[7] == '-') {
            e->year = atoi(line);
            e->month = atoi(line + 5);
            e->day = atoi(line + 8);
            snprintf(e->text, sizeof(e->text), "%s", line + 11);
            event_count++;
        }
    }
    qsort(events, (size_t)event_count, sizeof(events[0]), compare_events);
}

static void save(void) {
    int handle = vx_open(EVENTS_FILE, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
    if (handle < 0) {
        return;
    }
    for (int i = 0; i < event_count; i++) {
        char line[160];
        int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %s\n", events[i].year, events[i].month,
                         events[i].day, events[i].text);
        vx_write(handle, line, (size_t)n);
    }
    vx_close(handle);
}

static void add_event(void) {
    if (!typed[0] || event_count == MAX_EVENTS) {
        return;
    }
    struct event *e = &events[event_count++];
    e->year = year, e->month = month, e->day = day;
    snprintf(e->text, sizeof(e->text), "%s", typed);
    qsort(events, (size_t)event_count, sizeof(events[0]), compare_events);
    save();
    printf("calendar: added \"%s\" on %04d-%02d-%02d\n", typed, year, month, day);
    fflush(stdout);
    typed[0] = '\0';
}

static void remove_event(int index) {
    memmove(events + index, events + index + 1, sizeof(events[0]) * (size_t)(event_count - index - 1));
    event_count--;
    save();
}

static void go_month(int by) {
    month += by;
    while (month < 1) {
        month += 12, year--;
    }
    while (month > 12) {
        month -= 12, year++;
    }
    int max = days_in(year, month);
    day = day > max ? max : day;
}

static void go_today(void) {
    vx_local_now(&today);
    year = today.year, month = today.month, day = today.day;
}

/* ---- Layout ---- */

static int grid_w(void) {
    return window->surface.width - PANEL - 20;
}

static int cell_w(void) {
    return grid_w() / 7;
}

static int cell_h(void) {
    return (window->surface.height - HEADER - 30 - 12) / 6;
}

static int first_column(void) {
    return (weekday_of(year, month, 1) + 6) % 7; /* Monday first. */
}

static void cell_rect(int d, int *x, int *y) {
    int index = first_column() + d - 1;
    *x = 10 + index % 7 * cell_w();
    *y = HEADER + 30 + index / 7 * cell_h();
}

static int day_at(int px, int py) {
    for (int d = 1; d <= days_in(year, month); d++) {
        int x, y;
        cell_rect(d, &x, &y);
        if (vx_inside(px, py, x, y, cell_w(), cell_h())) {
            return d;
        }
    }
    return 0;
}

/* ---- Drawing ---- */

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_VIEW);
    char line[160];
    snprintf(line, sizeof(line), "%s %d", vx_month_names[month - 1], year);
    vx_text(s, vx_font(VX_FACE_BOLD, 24), 14, 14, line, VX_COLOR_TEXT, VX_TRANSPARENT);
    int gx = 10 + grid_w();
    vx_draw_button(s, gx - 150, 16, 32, 26, "<", hot == HOT_PREVIOUS);
    vx_draw_button(s, gx - 112, 16, 70, 26, "Today", hot == HOT_TODAY);
    vx_draw_button(s, gx - 36, 16, 32, 26, ">", hot == HOT_NEXT);
    static const char *const names[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int i = 0; i < 7; i++) {
        vx_draw_text(s, 10 + i * cell_w() + 8, HEADER + 8, names[i],
                     i >= 5 ? VX_COLOR_ACCENT : VX_COLOR_DIM, VX_TRANSPARENT);
    }
    /* The grid's lines, then the days. */
    for (int r = 0; r <= 6; r++) {
        vx_fill(s, 10, HEADER + 30 + r * cell_h(), 7 * cell_w(), 1, VX_COLOR_LINE);
    }
    for (int c = 0; c <= 7; c++) {
        vx_fill(s, 10 + c * cell_w(), HEADER + 30, 1, 6 * cell_h(), VX_COLOR_LINE);
    }
    for (int d = 1; d <= days_in(year, month); d++) {
        int x, y;
        cell_rect(d, &x, &y);
        bool is_today = d == today.day && month == today.month && year == today.year;
        if (d == day) {
            vx_fill(s, x + 1, y + 1, cell_w() - 1, cell_h() - 1, VX_COLOR_SELECTED);
        }
        snprintf(line, sizeof(line), "%d", d);
        int tw = vx_text_width(line);
        if (is_today) {
            vx_fill(s, x + cell_w() - tw - 16, y + 4, tw + 10, 20, VX_COLOR_ACCENT);
        }
        vx_draw_text(s, x + cell_w() - tw - 11, y + 6, line, is_today ? 0xffffff : VX_COLOR_TEXT,
                     VX_TRANSPARENT);
        /* The day's first events, small. */
        int shown = 0;
        for (int i = 0; i < event_count && shown < (cell_h() - 30) / 15; i++) {
            if (events[i].year == year && events[i].month == month && events[i].day == d) {
                vx_fill(s, x + 6, y + 30 + shown * 15 + 5, 5, 5, VX_COLOR_ACCENT);
                vx_draw_text_fit(s, x + 14, y + 28 + shown * 15, cell_w() - 18, events[i].text,
                                 VX_COLOR_DIM, VX_TRANSPARENT);
                shown++;
            }
        }
    }
    /* The day's events. */
    int px = w - PANEL;
    vx_fill(s, px, 0, PANEL, h, VX_COLOR_WINDOW);
    vx_fill(s, px, 0, 1, h, VX_COLOR_LINE);
    snprintf(line, sizeof(line), "%s %d %s", vx_weekday_names[weekday_of(year, month, day)], day,
             vx_month_names[month - 1]);
    vx_text(s, vx_font(VX_FACE_BOLD, 15), px + 14, 18, line, VX_COLOR_TEXT, VX_TRANSPARENT);
    int y = 52, n = 0;
    for (int i = 0; i < event_count; i++) {
        if (events[i].year != year || events[i].month != month || events[i].day != day) {
            continue;
        }
        vx_fill(s, px + 12, y, PANEL - 24, 34, VX_COLOR_VIEW);
        vx_fill(s, px + 12, y, 3, 34, VX_COLOR_ACCENT);
        vx_draw_text_fit(s, px + 22, y + 9, PANEL - 64, events[i].text, VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_text(s, px + PANEL - 30, y + 9, "x", VX_COLOR_DIM, VX_TRANSPARENT);
        y += 40;
        n++;
    }
    if (!n) {
        vx_draw_text(s, px + 14, y, "No events", VX_COLOR_DIM, VX_TRANSPARENT);
    }
    vx_draw_text(s, px + 14, h - 92, "New event (a time first, if you like):", VX_COLOR_DIM,
                 VX_TRANSPARENT);
    vx_draw_field(s, px + 14, h - 70, PANEL - 28, typed, typing);
    vx_draw_button(s, px + 14, h - 38, PANEL - 28, 26, "Add", hot == HOT_ADD);
    vx_window_present(window, 0, 0, w, h);
}

/* ---- Events ---- */

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    int w = window->surface.width, h = window->surface.height, gx = 10 + grid_w();
    int px = w - PANEL;
    hot = vx_inside(e->x, e->y, gx - 150, 16, 32, 26) ? HOT_PREVIOUS
          : vx_inside(e->x, e->y, gx - 112, 16, 70, 26) ? HOT_TODAY
          : vx_inside(e->x, e->y, gx - 36, 16, 32, 26) ? HOT_NEXT
          : vx_inside(e->x, e->y, px + 14, h - 38, PANEL - 28, 26) ? HOT_ADD
          : vx_inside(e->x, e->y, px + 14, h - 70, PANEL - 28, 24) ? HOT_FIELD : -1;
    if (e->wheel) {
        go_month(e->wheel > 0 ? -1 : 1);
    }
    if (!click) {
        return;
    }
    switch (hot) {
    case HOT_PREVIOUS: go_month(-1); return;
    case HOT_NEXT: go_month(1); return;
    case HOT_TODAY: go_today(); return;
    case HOT_ADD: add_event(); return;
    case HOT_FIELD: typing = true; return;
    }
    if (e->x >= px) {
        /* An event's x removes it. */
        int y = 52;
        for (int i = 0; i < event_count; i++) {
            if (events[i].year != year || events[i].month != month || events[i].day != day) {
                continue;
            }
            if (vx_inside(e->x, e->y, px + PANEL - 40, y, 30, 34)) {
                remove_event(i);
                return;
            }
            y += 40;
        }
        return;
    }
    int d = day_at(e->x, e->y);
    if (d) {
        day = d;
        typing = true;
    }
}

static void key(const struct vx_gui_event *e) {
    if (!e->value) {
        return;
    }
    if (typing) {
        if (e->key == VX_KEY_ENTER) {
            add_event();
            return;
        }
        if (e->key == VX_KEY_ESC) {
            typing = false;
            return;
        }
        if (vx_field_key(typed, sizeof(typed), e) || typed[0]) {
            return;
        }
    }
    int max = days_in(year, month);
    switch (e->key) {
    case VX_KEY_LEFT: day > 1 ? (void)day-- : (go_month(-1), (void)(day = days_in(year, month))); break;
    case VX_KEY_RIGHT: day < max ? (void)day++ : (go_month(1), (void)(day = 1)); break;
    case VX_KEY_UP: day > 7 ? (void)(day -= 7) : (void)0; break;
    case VX_KEY_DOWN: day + 7 <= max ? (void)(day += 7) : (void)0; break;
    case VX_KEY_PAGEUP: go_month(-1); break;
    case VX_KEY_PAGEDOWN: go_month(1); break;
    default:
        if (e->character == 't' && !typing) {
            go_today();
        } else if (e->character >= ' ') {
            typing = true;
            vx_field_key(typed, sizeof(typed), e);
        }
    }
}

int main(void) {
    go_today();
    load();
    window = vx_window_create_flags("Calendar", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "calendar: no desktop to open a window on\n");
        return 1;
    }
    int held = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        int got = vx_gui_wait(&e, 60000);
        if (got < 0) {
            return 0;
        }
        if (got == 0) {
            vx_local_now(&today); /* Midnight moves today. */
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_KEY: key(&e); break;
        case VX_GUI_RESIZE:
            if (e.width >= 640 && e.height >= 400) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
