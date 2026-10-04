/* monitor: Activity Monitor. The processes (CPU, memory, threads), sorted
 * by a click on a column, updated every second; CPU and memory in use over
 * the last minute; Quit and Force Quit for the one selected.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 700
#define HEIGHT 520
#define TOOLBAR 40
#define HEADER 24
#define ROW 22
#define GRAPHS 110
#define MAX_PROCESSES 256
#define SAMPLES 60

struct process {
    unsigned id, threads;
    char name[32];
    unsigned long long cpu_ticks; /* 1/100 s, all its life. */
    unsigned long long memory;    /* Bytes in memory. */
    int percent;                  /* Of one CPU, in tenths, over the last second. */
};

static struct vx_window *window;
static struct process list[MAX_PROCESSES], previous[MAX_PROCESSES];
static int count, previous_count;
static long sampled_ms;
static int sort_column = 2; /* 0 name, 1 id, 2 CPU, 3 memory, 4 threads */
static bool sort_down = true;
static unsigned selected_id;
static int top, hot_button = -1;
static int cpu_history[SAMPLES], memory_history[SAMPLES]; /* Percent. */
static int cpus = 1;
static unsigned long long memory_total, memory_used;

static bool read_file(const char *path, char *buffer, size_t size) {
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        return false;
    }
    long n = vx_read(handle, buffer, size - 1);
    vx_close(handle);
    buffer[n > 0 ? n : 0] = '\0';
    return n > 0;
}

/* /proc/<id>/stat: the fields after the name (which may have spaces). */
static void read_stat(struct process *p) {
    char path[48], text[512];
    snprintf(path, sizeof(path), "/proc/%u/stat", p->id);
    if (!read_file(path, text, sizeof(text))) {
        return;
    }
    char *after = strrchr(text, ')');
    if (!after) {
        return;
    }
    /* Field 3 is the state; 14 the CPU time; 20 the threads; 24 the pages in memory. */
    int field = 3;
    for (char *t = after + 2; *t && field <= 24; field++) {
        unsigned long long v = strtoul(t, NULL, 10);
        if (field == 14) {
            p->cpu_ticks = v;
        } else if (field == 20) {
            p->threads = (unsigned)v;
        } else if (field == 24) {
            p->memory = v * 4096;
        }
        while (*t && *t != ' ') {
            t++;
        }
        while (*t == ' ') {
            t++;
        }
    }
}

static int compare(const void *a, const void *b) {
    const struct process *x = a, *y = b;
    long long d = 0;
    switch (sort_column) {
    case 0: d = strcmp(x->name, y->name); break;
    case 1: d = (long long)x->id - y->id; break;
    case 2: d = x->percent - y->percent; break;
    case 3: d = (long long)(x->memory > y->memory) - (long long)(x->memory < y->memory); break;
    case 4: d = (long long)x->threads - y->threads; break;
    }
    if (!d) {
        d = (long long)x->id - y->id;
    }
    return (sort_down ? d < 0 : d > 0) ? 1 : -1;
}

static void sample(void) {
    long now = vx_uptime();
    long elapsed = sampled_ms ? now - sampled_ms : 0;
    sampled_ms = now;
    memcpy(previous, list, sizeof(list[0]) * (size_t)count);
    previous_count = count;
    struct vx_process_info info[MAX_PROCESSES];
    long n = vx_process_list(info, MAX_PROCESSES);
    count = 0;
    unsigned long long busy = 0;
    for (long i = 0; i < n; i++) {
        if (info[i].state != 0) {
            continue; /* Gone, not yet waited for. */
        }
        struct process *p = &list[count++];
        memset(p, 0, sizeof(*p));
        p->id = info[i].id;
        snprintf(p->name, sizeof(p->name), "%s", info[i].name);
        read_stat(p);
        for (int k = 0; k < previous_count; k++) {
            if (previous[k].id == p->id && elapsed > 0) {
                unsigned long long ticks = p->cpu_ticks - previous[k].cpu_ticks;
                p->percent = (int)(ticks * 10 * 1000 / (unsigned long long)elapsed);
                busy += ticks;
            }
        }
    }
    qsort(list, (size_t)count, sizeof(list[0]), compare);
    struct vx_system_info system;
    if (vx_system_info(&system) == 0) {
        cpus = system.cpus ? (int)system.cpus : 1;
        memory_total = system.memory_total;
        memory_used = system.memory_total - system.memory_free;
    }
    memmove(cpu_history, cpu_history + 1, sizeof(int) * (SAMPLES - 1));
    memmove(memory_history, memory_history + 1, sizeof(int) * (SAMPLES - 1));
    int cpu = elapsed > 0 ? (int)(busy * 1000 / (unsigned long long)elapsed / (unsigned)cpus) : 0;
    cpu_history[SAMPLES - 1] = cpu > 100 ? 100 : cpu;
    memory_history[SAMPLES - 1] = memory_total ? (int)(memory_used * 100 / memory_total) : 0;
}

static void format_bytes(char *out, size_t size, unsigned long long bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(out, size, "%llu.%llu GB", bytes >> 30, (bytes >> 20) % 1024 * 10 / 1024);
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%llu.%llu MB", bytes >> 20, (bytes >> 10) % 1024 * 10 / 1024);
    } else {
        snprintf(out, size, "%llu KB", bytes >> 10);
    }
}

static const char *const columns[5] = {"Process", "ID", "% CPU", "Memory", "Threads"};
static const int column_x[6] = {12, 300, 380, 470, 590, WIDTH - 12};

static int visible_rows(void) {
    return (window->surface.height - TOOLBAR - HEADER - GRAPHS) / ROW;
}

static void graph(struct vx_surface *s, int x, int y, int w, int h, const int *values,
                  uint32_t color, const char *title, const char *now) {
    vx_fill_rounded(s, x, y, w, h, 6, VX_COLOR_LINE, 255);
    vx_fill_rounded(s, x + 1, y + 1, w - 2, h - 2, 5, VX_COLOR_VIEW, 255);
    for (int i = 0; i < SAMPLES; i++) {
        int bar = values[i] * (h - 2) / 100;
        int bx = x + 1 + i * (w - 2) / SAMPLES, bw = (w - 2) / SAMPLES;
        vx_fill(s, bx, y + h - 1 - bar, bw > 1 ? bw - 1 : 1, bar, color);
    }
    vx_draw_text(s, x + 6, y + 4, title, VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_text(s, x + w - 6 - vx_text_width(now), y + 4, now, VX_COLOR_DIM, VX_TRANSPARENT);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    int w = s->width, h = s->height;
    vx_fill(s, 0, 0, w, h, VX_COLOR_WINDOW);
    /* The toolbar. */
    vx_draw_toolbar(s, 0, 0, w, TOOLBAR);
    vx_draw_button(s, 10, 8, 70, 24, "Quit", hot_button == 0);
    vx_draw_button(s, 86, 8, 96, 24, "Force Quit", hot_button == 1);
    char line[96];
    snprintf(line, sizeof(line), "%d processes   %d CPU%s", count, cpus, cpus == 1 ? "" : "s");
    vx_draw_text(s, w - 12 - vx_text_width(line), 12, line, VX_COLOR_DIM, VX_TRANSPARENT);
    /* The list. */
    int ly = TOOLBAR;
    vx_fill(s, 0, ly, w, HEADER, VX_COLOR_WINDOW);
    for (int c = 0; c < 5; c++) {
        char title[32];
        snprintf(title, sizeof(title), "%s%s", columns[c],
                 c == sort_column ? (sort_down ? " \xe2\x96\xbe" : " \xe2\x96\xb4") : "");
        vx_draw_text(s, column_x[c], ly + 4, title, c == sort_column ? VX_COLOR_ACCENT : VX_COLOR_DIM,
                     VX_TRANSPARENT);
    }
    vx_fill(s, 0, ly + HEADER - 1, w, 1, VX_COLOR_LINE);
    int rows = visible_rows();
    vx_fill(s, 0, ly + HEADER, w, rows * ROW, VX_COLOR_VIEW);
    top = top > count - rows ? count - rows : top;
    top = top < 0 ? 0 : top;
    for (int r = 0; r < rows && top + r < count; r++) {
        struct process *p = &list[top + r];
        int y = ly + HEADER + r * ROW;
        if (p->id == selected_id) {
            vx_draw_selection(s, 2, y, w - 4, ROW);
        } else if (r % 2) {
            vx_fill(s, 0, y, w, ROW, vx_theme.stripe);
        }
        vx_draw_text_fit(s, column_x[0], y + 3, column_x[1] - column_x[0] - 8, p->name,
                         VX_COLOR_TEXT, VX_TRANSPARENT);
        snprintf(line, sizeof(line), "%u", p->id);
        vx_draw_text(s, column_x[1], y + 3, line, VX_COLOR_DIM, VX_TRANSPARENT);
        snprintf(line, sizeof(line), "%d.%d", p->percent / 10, p->percent % 10);
        vx_draw_text(s, column_x[2], y + 3, line, p->percent >= 200 ? VX_COLOR_ACCENT : VX_COLOR_TEXT,
                     VX_TRANSPARENT);
        format_bytes(line, sizeof(line), p->memory);
        vx_draw_text(s, column_x[3], y + 3, line, VX_COLOR_TEXT, VX_TRANSPARENT);
        snprintf(line, sizeof(line), "%u", p->threads);
        vx_draw_text(s, column_x[4], y + 3, line, VX_COLOR_DIM, VX_TRANSPARENT);
    }
    /* The graphs. */
    int gy = h - GRAPHS + 10, gw = (w - 30) / 2, gh = GRAPHS - 20;
    snprintf(line, sizeof(line), "%d%%", cpu_history[SAMPLES - 1]);
    graph(s, 10, gy, gw, gh, cpu_history, VX_COLOR_ACCENT, "CPU", line);
    char used[24], total[24];
    format_bytes(used, sizeof(used), memory_used);
    format_bytes(total, sizeof(total), memory_total);
    snprintf(line, sizeof(line), "%s of %s", used, total);
    graph(s, 20 + gw, gy, gw, gh, memory_history, 0x3fbf6f, "Memory", line);
    vx_window_present(window, 0, 0, w, h);
}

static void quit_selected(bool force) {
    if (!selected_id) {
        return;
    }
    long error = vx_kill(selected_id, force ? VX_SIGKILL : VX_SIGTERM);
    printf("monitor: %s process %u%s\n", force ? "force quit" : "quit", selected_id,
           error ? " failed" : "");
    fflush(stdout);
    sample();
}

static void pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    hot_button = vx_inside(e->x, e->y, 10, 8, 70, 24) ? 0 : vx_inside(e->x, e->y, 86, 8, 96, 24) ? 1 : -1;
    if (e->wheel) {
        top -= e->wheel * 3;
    }
    if (!click) {
        return;
    }
    if (hot_button >= 0) {
        quit_selected(hot_button == 1);
        return;
    }
    if (e->y >= TOOLBAR && e->y < TOOLBAR + HEADER) {
        for (int c = 0; c < 5; c++) {
            if (e->x >= column_x[c] && e->x < column_x[c + 1]) {
                sort_down = c == sort_column ? !sort_down : c >= 2;
                sort_column = c;
                qsort(list, (size_t)count, sizeof(list[0]), compare);
            }
        }
        return;
    }
    int r = (e->y - TOOLBAR - HEADER) / ROW;
    if (e->y >= TOOLBAR + HEADER && r < visible_rows() && top + r < count) {
        selected_id = list[top + r].id;
    }
}

int main(void) {
    window = vx_window_create_flags("Activity Monitor", WIDTH, HEIGHT, VX_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "monitor: no desktop to open a window on\n");
        return 1;
    }
    sample();
    int held = 0;
    long next = vx_uptime() + 1000;
    for (;;) {
        draw();
        struct vx_gui_event e;
        long left = next - vx_uptime();
        int got = vx_gui_wait(&e, left < 0 ? 0 : left);
        if (got < 0) {
            return 0;
        }
        if (got == 0) {
            sample();
            next = vx_uptime() + 1000;
            continue;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_POINTER: pointer(&e, &held); break;
        case VX_GUI_KEY:
            if (e.value && (e.key == VX_KEY_UP || e.key == VX_KEY_DOWN)) {
                int at = 0;
                while (at < count && list[at].id != selected_id) {
                    at++;
                }
                at += e.key == VX_KEY_DOWN ? 1 : -1;
                if (at >= 0 && at < count) {
                    selected_id = list[at].id;
                    if (at < top) {
                        top = at;
                    } else if (at >= top + visible_rows()) {
                        top = at - visible_rows() + 1;
                    }
                }
            } else if (e.value && e.key == VX_KEY_DELETE) {
                quit_selected(false);
            }
            break;
        case VX_GUI_RESIZE:
            if (e.width >= 500 && e.height >= 320) {
                vx_window_resize(window, e.width, e.height);
            }
            break;
        }
    }
}
