/* installer: the Installer app. Puts Vexa on a disk so it starts from there:
 * choose the disk (and whether the Linux programs come along), confirm that
 * it'll be erased, and follow along while /bin/install does the work (its
 * output, one line per step, drives the progress shown here).
 *
 * The keyboard does it all: Up and Down choose a disk, Space the Linux
 * programs, Enter goes on, Escape goes back.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 640
#define HEIGHT 460
#define MAX_DISKS 16

enum page { CHOOSE, CONFIRM, INSTALLING, DONE, FAILED };

struct disk {
    char name[16];    /* "vda" */
    char model[64];
    char details[128];
};

static struct vx_window *window;
static enum page page = CHOOSE;
static struct disk disks[MAX_DISKS];
static int disk_count, chosen = -1;
static bool with_linux = true;
static bool booted_from_cd;
static int hot = -1; /* The button under the pointer. */

/* While installing: the program, its output, and what it says. */
static int child = -1, output = -1;
static char pending[512];
static size_t pending_length;
static char steps[8][128];
static int step_count, percent;
static char message[160];

static void find_disks(void) {
    static struct vx_device_info devices[256];
    long n = vx_device_list(devices, 256, NULL);
    disk_count = 0;
    for (long i = 0; i < n && i < 256 && disk_count < MAX_DISKS; i++) {
        const struct vx_device_info *d = &devices[i];
        if (d->kind != VX_DEVICE_DISK || strncmp(d->location, "/dev/", 5) || strstr(d->name, "CD")) {
            continue;
        }
        struct disk *k = &disks[disk_count++];
        snprintf(k->name, sizeof(k->name), "%s", d->location + 5);
        snprintf(k->model, sizeof(k->model), "%s", d->name);
        snprintf(k->details, sizeof(k->details), "%s", d->details);
    }
    struct vx_mount_info mounts[16];
    long m = vx_mounts(mounts, 16);
    booted_from_cd = true;
    for (long i = 0; i < m && i < 16; i++) {
        if (!strcmp(mounts[i].path, "/") && !strcmp(mounts[i].type, "ext2")) {
            booted_from_cd = false; /* (Already installed: started from a disk.) */
        }
    }
}

/* ---- Drawing ---- */

static void button(struct vx_surface *s, int index, int x, int y, int w, const char *label,
                   bool primary) {
    if (primary) {
        vx_fill(s, x, y, w, 30, hot == index ? vx_mix(VX_COLOR_ACCENT, 0xffffff, 40)
                                            : VX_COLOR_ACCENT);
        int tw = vx_text_width(label);
        vx_draw_text(s, x + (w - tw) / 2, y + 7, label, 0xffffff, VX_TRANSPARENT);
    } else {
        vx_draw_button(s, x, y, w, 30, label, hot == index);
    }
}

static int title(struct vx_surface *s, const char *text, const char *subtitle) {
    const struct vx_font *big = vx_font(VX_FACE_BOLD, 22);
    vx_text(s, big, 28, 24, text, VX_COLOR_TEXT, VX_TRANSPARENT);
    int y = 24 + vx_font_height(big) + 6;
    if (subtitle) {
        vx_draw_text(s, 28, y, subtitle, VX_COLOR_DIM, VX_TRANSPARENT);
        y += VX_LINE_HEIGHT + 4;
    }
    return y + 14;
}

static void draw_choose(struct vx_surface *s) {
    int y = title(s, "Install Vexa", "Put Vexa on a disk, so the computer starts it from there.");
    if (!booted_from_cd) {
        vx_draw_text(s, 28, y, "Vexa is already running from a disk. To install it on another",
                     VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_text(s, 28, y + 20, "one, start from the Vexa CD (or USB stick).", VX_COLOR_TEXT,
                     VX_TRANSPARENT);
        button(s, 1, WIDTH - 128, HEIGHT - 52, 100, "Close", false);
        return;
    }
    vx_draw_text(s, 28, y, "Which disk? Everything on it will be erased.", VX_COLOR_TEXT,
                 VX_TRANSPARENT);
    y += 26;
    if (disk_count == 0) {
        vx_draw_text(s, 40, y + 10, "There are no disks to install on.", VX_COLOR_DIM, VX_TRANSPARENT);
    }
    for (int i = 0; i < disk_count; i++) {
        int ry = y + i * 52;
        bool on = i == chosen;
        vx_fill(s, 28, ry, WIDTH - 56, 46, on ? VX_COLOR_SELECTED : VX_COLOR_VIEW);
        vx_draw_outline(s, 28, ry, WIDTH - 56, 46, on ? VX_COLOR_ACCENT : VX_COLOR_LINE);
        char line[96];
        snprintf(line, sizeof(line), "%s  (%s)", disks[i].model, disks[i].name);
        vx_text(s, vx_font(VX_FACE_BOLD, VX_UI_FONT_SIZE), 42, ry + 6, line, VX_COLOR_TEXT,
                VX_TRANSPARENT);
        vx_draw_text_fit(s, 42, ry + 25, WIDTH - 100, disks[i].details, VX_COLOR_DIM,
                         VX_TRANSPARENT);
    }
    int cy = HEIGHT - 100;
    vx_fill(s, 28, cy, 16, 16, VX_COLOR_VIEW);
    vx_draw_outline(s, 28, cy, 16, 16, VX_COLOR_LINE);
    if (with_linux) {
        vx_fill(s, 32, cy + 4, 8, 8, VX_COLOR_ACCENT);
    }
    vx_draw_text(s, 52, cy, "The Linux programs too (bash, Python, X, GTK...: about 190 MB)",
                 VX_COLOR_TEXT, VX_TRANSPARENT);
    button(s, 0, WIDTH - 148, HEIGHT - 52, 120, "Install...", chosen >= 0);
    button(s, 1, WIDTH - 256, HEIGHT - 52, 100, "Close", false);
}

static void draw_confirm(struct vx_surface *s) {
    const struct disk *d = &disks[chosen];
    int y = title(s, "Erase the disk?", NULL);
    char line[200];
    snprintf(line, sizeof(line), "Everything on %s (%s) will be erased, for good:", d->model, d->name);
    vx_draw_text(s, 28, y, line, VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_text_fit(s, 28, y + 22, WIDTH - 56, d->details, VX_COLOR_DIM, VX_TRANSPARENT);
    vx_draw_text(s, 28, y + 56, "Then Vexa goes on it: a small partition to start from (FAT32),",
                 VX_COLOR_TEXT, VX_TRANSPARENT);
    vx_draw_text(s, 28, y + 76, "and the rest for the system and your files (ext2).", VX_COLOR_TEXT,
                 VX_TRANSPARENT);
    button(s, 0, WIDTH - 188, HEIGHT - 52, 160, "Erase and Install", true);
    button(s, 1, WIDTH - 296, HEIGHT - 52, 100, "Back", false);
}

static void draw_installing(struct vx_surface *s) {
    int y = title(s, page == DONE ? "Vexa is installed" : page == FAILED ? "It didn't work"
                                                                         : "Installing Vexa...",
                  NULL);
    int bw = WIDTH - 56;
    vx_fill(s, 28, y, bw, 10, VX_COLOR_BUTTON);
    vx_fill(s, 28, y, bw * (page == DONE ? 100 : percent) / 100, 10,
            page == FAILED ? 0xd9534f : VX_COLOR_ACCENT);
    y += 26;
    for (int i = 0; i < step_count; i++) {
        bool last = i == step_count - 1;
        vx_draw_text_fit(s, 28, y + i * 22, WIDTH - 56, steps[i],
                         last && page == INSTALLING ? VX_COLOR_TEXT : VX_COLOR_DIM, VX_TRANSPARENT);
    }
    if (page == DONE) {
        vx_draw_text(s, 28, HEIGHT - 110, "Take the CD (or USB stick) out, and restart: the",
                     VX_COLOR_TEXT, VX_TRANSPARENT);
        vx_draw_text(s, 28, HEIGHT - 90, "computer starts Vexa from the disk now.", VX_COLOR_TEXT,
                     VX_TRANSPARENT);
        button(s, 0, WIDTH - 148, HEIGHT - 52, 120, "Restart", true);
        button(s, 1, WIDTH - 256, HEIGHT - 52, 100, "Close", false);
    } else if (page == FAILED) {
        vx_draw_text_fit(s, 28, HEIGHT - 100, WIDTH - 56, message, 0xd9534f, VX_TRANSPARENT);
        button(s, 1, WIDTH - 128, HEIGHT - 52, 100, "Close", false);
    }
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
    switch (page) {
    case CHOOSE: draw_choose(s); break;
    case CONFIRM: draw_confirm(s); break;
    default: draw_installing(s); break;
    }
    vx_window_present(window, 0, 0, s->width, s->height);
}

/* ---- Installing ---- */

static void start(void) {
    int ends[2];
    if (vx_pipe(ends) < 0) {
        snprintf(message, sizeof(message), "Couldn't start the installer.");
        page = FAILED;
        return;
    }
    const char *argv[5];
    int argc = 0;
    argv[argc++] = "install";
    argv[argc++] = "--yes";
    if (!with_linux) {
        argv[argc++] = "--no-linux";
    }
    argv[argc++] = disks[chosen].name;
    argv[argc] = NULL;
    extern char **environ;
    unsigned long envc = 0;
    while (environ && environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {.argv = argv, .argc = (unsigned long)argc,
                             .envp = (const char *const *)environ, .envc = envc,
                             .handles = {0, ends[1], ends[1]}};
    child = vx_spawn("/bin/install", &spawn);
    vx_close(ends[1]);
    if (child < 0) {
        vx_close(ends[0]);
        snprintf(message, sizeof(message), "Couldn't start /bin/install.");
        page = FAILED;
        return;
    }
    output = ends[0];
    page = INSTALLING;
    step_count = 0;
    percent = 0;
    printf("installer: installing on %s%s\n", disks[chosen].name, with_linux ? "" : " (no Linux)");
    fflush(stdout);
}

static void line_from_install(const char *line) {
    if (!strncmp(line, "progress: ", 10)) {
        percent = atoi(line + 10);
    } else if (!strncmp(line, "step: ", 6)) {
        if (step_count == 8) {
            memmove(steps, steps + 1, sizeof(steps[0]) * 7);
            step_count--;
        }
        snprintf(steps[step_count++], sizeof(steps[0]), "%s", line + 6);
        char first[2] = {(char)(line[6] >= 'a' && line[6] <= 'z' ? line[6] - 32 : line[6]), 0};
        steps[step_count - 1][0] = first[0];
        percent = 0;
        printf("installer: %s\n", line + 6);
        fflush(stdout);
    } else if (!strncmp(line, "error: ", 7)) {
        snprintf(message, sizeof(message), "%s", line + 7);
    }
}

static void read_output(void) {
    char buffer[256];
    long n = vx_read(output, buffer, sizeof(buffer));
    if (n <= 0) { /* It's finished. */
        vx_close(output);
        output = -1;
        long code = vx_wait(child, 0);
        vx_close(child);
        child = -1;
        page = code == 0 ? DONE : FAILED;
        if (page == FAILED && !message[0]) {
            snprintf(message, sizeof(message), "The installer stopped (code %ld).", code);
        }
        printf("installer: %s\n", page == DONE ? "done" : message);
        fflush(stdout);
        return;
    }
    for (long i = 0; i < n; i++) {
        if (buffer[i] == '\n' || pending_length == sizeof(pending) - 1) {
            pending[pending_length] = '\0';
            line_from_install(pending);
            pending_length = 0;
        } else {
            pending[pending_length++] = buffer[i];
        }
    }
}

/* ---- Input ---- */

static void go_on(void) {
    if (page == CHOOSE && chosen >= 0 && booted_from_cd) {
        page = CONFIRM;
    } else if (page == CONFIRM) {
        start();
    } else if (page == DONE) {
        vx_power(VX_POWER_RESTART);
    }
}

static bool go_back(void) {
    if (page == CONFIRM) {
        page = CHOOSE;
        return false;
    }
    return page != INSTALLING; /* Closing is fine, except while it works. */
}

static int button_at(int x, int y) {
    if (y < HEIGHT - 52 || y >= HEIGHT - 22) {
        return -1;
    }
    switch (page) {
    case CHOOSE:
        return x >= WIDTH - 148 && x < WIDTH - 28 ? 0 : x >= WIDTH - 256 && x < WIDTH - 156 ? 1 : -1;
    case CONFIRM:
        return x >= WIDTH - 188 && x < WIDTH - 28 ? 0 : x >= WIDTH - 296 && x < WIDTH - 196 ? 1 : -1;
    case DONE:
        return x >= WIDTH - 148 && x < WIDTH - 28 ? 0 : x >= WIDTH - 256 && x < WIDTH - 156 ? 1 : -1;
    case FAILED:
        return x >= WIDTH - 128 && x < WIDTH - 28 ? 1 : -1;
    default:
        return -1;
    }
}

static bool pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    hot = button_at(e->x, e->y);
    if (!click) {
        return false;
    }
    if (hot == 0) {
        go_on();
    } else if (hot == 1) {
        return go_back() || page == CHOOSE;
    } else if (page == CHOOSE) {
        int y = 24 + 22 + 6 + VX_LINE_HEIGHT + 4 + 14 + 26;
        if (e->y >= y && e->y < y + disk_count * 52 && e->x >= 28 && e->x < WIDTH - 28) {
            chosen = (e->y - y) / 52;
        } else if (e->y >= HEIGHT - 100 && e->y < HEIGHT - 84 && e->x < WIDTH - 28) {
            with_linux = !with_linux;
        }
    }
    return false;
}

int main(void) {
    window = vx_window_create("Installer", WIDTH, HEIGHT);
    if (!window) {
        fprintf(stderr, "installer: no desktop to open a window on\n");
        return 1;
    }
    find_disks();
    printf("installer: %d disk%s\n", disk_count, disk_count == 1 ? "" : "s");
    fflush(stdout);
    int held = 0;
    for (;;) {
        draw();
        struct vx_poll polls[2] = {{.handle = vx_gui_handle(), .events = VX_POLL_READ}};
        int count = 1;
        if (output >= 0) {
            polls[count++] = (struct vx_poll){.handle = output, .events = VX_POLL_READ};
        }
        vx_poll(polls, (size_t)count, -1);
        if (output >= 0 && polls[1].ready) {
            read_output();
        }
        struct vx_gui_event e;
        int got;
        while ((got = vx_gui_wait(&e, 0)) > 0) {
            bool quit = false;
            switch (e.type) {
            case VX_GUI_CLOSE:
                quit = page != INSTALLING;
                break;
            case VX_GUI_POINTER:
                quit = pointer(&e, &held);
                break;
            case VX_GUI_KEY:
                if (!e.value) {
                    break;
                }
                if (e.key == VX_KEY_DOWN && page == CHOOSE && disk_count) {
                    chosen = chosen + 1 < disk_count ? chosen + 1 : chosen;
                } else if (e.key == VX_KEY_UP && page == CHOOSE && chosen > 0) {
                    chosen--;
                } else if (e.key == VX_KEY_SPACE && page == CHOOSE) {
                    with_linux = !with_linux;
                } else if (e.key == VX_KEY_ENTER) {
                    go_on();
                } else if (e.key == VX_KEY_ESC) {
                    quit = go_back() && page != CHOOSE;
                }
                break;
            }
            if (quit) {
                vx_window_destroy(window);
                return 0;
            }
        }
        if (got < 0) {
            return 0;
        }
    }
}
