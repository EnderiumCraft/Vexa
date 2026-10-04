/* about: "About Vexa", a window with the version and how the system is
 * doing (CPUs, memory, uptime, processes, network), updated every second.
 */
#include <stdio.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/net.h>
#include <vexa/syscall.h>

#define WIDTH 360
#define HEIGHT 250
#define LEFT 20
#define VALUE_LEFT 130

#define COLOR_BACKGROUND VX_COLOR_WINDOW
#define COLOR_TITLE VX_COLOR_ACCENT
#define COLOR_LABEL VX_COLOR_DIM
#define COLOR_TEXT VX_COLOR_TEXT

static int line(struct vx_surface *s, int y, const char *label, const char *value) {
    vx_draw_text(s, LEFT, y, label, COLOR_LABEL, VX_TRANSPARENT);
    vx_draw_text(s, VALUE_LEFT, y, value, COLOR_TEXT, VX_TRANSPARENT);
    return y + VX_LINE_HEIGHT + 6;
}

static void draw(struct vx_window *window) {
    struct vx_surface *s = &window->surface;
    vx_fill(s, 0, 0, s->width, s->height, COLOR_BACKGROUND);
    struct vx_system_info info;
    if (vx_system_info(&info)) {
        memset(&info, 0, sizeof(info));
    }
    const struct vx_font *big = vx_font(VX_FACE_BOLD, 40);
    int end = vx_text(s, big, LEFT, 12, "Vexa", COLOR_TITLE, VX_TRANSPARENT);
    char text[96];
    snprintf(text, sizeof(text), "version %s", info.version);
    vx_draw_text(s, end + 12, 12 + vx_font_ascent(big) - 13, text, COLOR_LABEL, VX_TRANSPARENT);
    int y = 12 + vx_font_height(big) + 12;

    snprintf(text, sizeof(text), "%u", info.cpus);
    y = line(s, y, "CPUs", text);

    unsigned long long used = info.memory_total - info.memory_free;
    snprintf(text, sizeof(text), "%llu of %llu MiB used", used >> 20, info.memory_total >> 20);
    y = line(s, y, "Memory", text);
    int bar = WIDTH - VALUE_LEFT - LEFT;
    vx_draw_progress(s, VALUE_LEFT, y - 6, bar, 10, used, info.memory_total);
    y += 8;

    unsigned long long seconds = info.uptime_ms / 1000;
    snprintf(text, sizeof(text), "%llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60,
             seconds % 60);
    y = line(s, y, "Up for", text);

    struct vx_process_info processes[128];
    long count = vx_process_list(processes, 128);
    snprintf(text, sizeof(text), "%ld", count < 0 ? 0 : count);
    y = line(s, y, "Processes", text);

    struct vx_net_interface interfaces[8];
    long n = vx_net_info(interfaces, 8);
    const char *address = "none";
    char formatted[16];
    for (long i = 0; i < n && i < 8; i++) {
        if (!(interfaces[i].flags & VX_NET_LOOPBACK) && interfaces[i].address) {
            snprintf(text, sizeof(text), "%s (%s)",
                     vx_format_ipv4(interfaces[i].address, formatted), interfaces[i].name);
            address = text;
            break;
        }
    }
    y = line(s, y, "Network", address);

    vx_draw_text(s, LEFT, HEIGHT - VX_LINE_HEIGHT - 12, "A hobby operating system, from scratch.",
                 COLOR_LABEL, VX_TRANSPARENT);
    vx_window_present(window, 0, 0, s->width, s->height);
}

int main(void) {
    struct vx_window *window = vx_window_create("About Vexa", WIDTH, HEIGHT);
    if (!window) {
        fprintf(stderr, "about: no desktop to open a window on\n");
        return 1;
    }
    for (;;) {
        draw(window);
        struct vx_gui_event e;
        long deadline = vx_uptime() + 1000;
        int got = 0;
        long left;
        while ((left = deadline - vx_uptime()) > 0 && (got = vx_gui_wait(&e, left)) > 0) {
            if (e.type == VX_GUI_CLOSE) {
                vx_window_destroy(window);
                return 0;
            }
        }
        if (got < 0) {
            return 0; /* The desktop is gone. */
        }
    }
}
