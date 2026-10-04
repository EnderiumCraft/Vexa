/* installer: the Installer app. Puts Vexa on a disk so it starts from there:
 * a welcome, the disk (and whether the Linux programs come along), your
 * account (the new system's administrator), a summary to confirm that the
 * disk will be erased, then the work itself, done by /bin/install (its
 * output, one line per step, drives the progress shown here).
 *
 * A sidebar shows the steps, the one you're on lit up. The keyboard does it
 * all: Enter goes on (to the next field, on the account page), Escape goes
 * back, Up and Down choose a disk, Space the Linux programs, Tab goes
 * through the account's fields.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 820
#define HEIGHT 560
#define SIDEBAR 230
#define CONTENT (SIDEBAR + 36)              /* Where the page's content starts. */
#define CONTENT_W (WIDTH - CONTENT - 36)
#define FOOTER 64                           /* The buttons' bar at the bottom. */
#define MAX_DISKS 16
#define MAX_STEPS 10

enum page { WELCOME, CHOOSE, ACCOUNT, CONFIRM, INSTALLING, DONE, FAILED };

/* The sidebar's steps (FAILED shows as Installing). */
static const char *const step_names[] = {"Welcome", "Disk", "Account", "Summary", "Installing",
                                         "Finished"};
#define STEP_COUNT 6

struct disk {
    char name[16];    /* "vda" */
    char model[64];
    char details[128];
    unsigned long long size; /* Bytes, or 0 if unknown. */
};

static struct vx_window *window;
static enum page page = WELCOME;
static struct disk disks[MAX_DISKS];
static int disk_count, chosen = -1;
static bool with_linux = true;
static bool booted_from_cd;
static struct vx_image *disk_icon, *folder_icon, *program_icon, *computer_icon;

/* The account: full name, account name, password (twice); which is typed in. */
enum { F_FULL, F_NAME, F_PASSWORD, F_AGAIN, FIELD_COUNT };
static char fields[FIELD_COUNT][64];
static int field;
static bool name_typed; /* The account name was typed, not made from the full name. */
static char account_problem[96];

/* While installing: the program, its output, and what it says. */
static int child = -1, output = -1;
static char pending[512];
static size_t pending_length;
static char steps[MAX_STEPS][128];
static int step_count, percent;
static char message[160];
static long started_ms;

/* What can be clicked, as drawn this time. */
enum hit_kind { HIT_NEXT, HIT_BACK, HIT_DISK, HIT_LINUX, HIT_FIELD };
struct hit {
    int x, y, w, h;
    enum hit_kind kind;
    int index;
};
static struct hit hits[32];
static int hit_count, hot_hit = -1;

static void add_hit(int x, int y, int w, int h, enum hit_kind kind, int index) {
    if (hit_count < (int)(sizeof(hits) / sizeof(hits[0]))) {
        hits[hit_count++] = (struct hit){x, y, w, h, kind, index};
    }
}

static long now_ms(void) {
    return (long)vx_uptime();
}

/* ---- What's there ---- */

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
        k->size = 0;
        int handle = vx_open(d->location, VX_OPEN_READ);
        struct vx_block_info info;
        if (handle >= 0 && vx_control(handle, VX_BLOCK_INFO, &info, sizeof(info)) == 0) {
            k->size = info.size;
        }
        if (handle >= 0) {
            vx_close(handle);
        }
    }
    if (disk_count == 1) {
        chosen = 0;
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

static void format_size(char *out, size_t size, unsigned long long bytes) {
    if (bytes >= 10ULL << 30) {
        snprintf(out, size, "%llu GB", bytes / 1000000000ULL);
    } else if (bytes >= 1ULL << 30) {
        snprintf(out, size, "%llu.%llu GB", bytes / 1000000000ULL,
                 bytes % 1000000000ULL / 100000000ULL);
    } else {
        snprintf(out, size, "%llu MB", bytes / 1000000ULL);
    }
}

/* ---- Drawing helpers ---- */

static const struct vx_font *bold(int size) {
    return vx_font(VX_FACE_BOLD, size);
}

static const struct vx_font *sans(int size) {
    return vx_font(VX_FACE_SANS, size);
}

static void text(struct vx_surface *s, const struct vx_font *f, int x, int y, const char *t,
                 uint32_t color) {
    vx_text(s, f, x, y, t, color, VX_TRANSPARENT);
}

static void centered(struct vx_surface *s, const struct vx_font *f, int cx, int y, const char *t,
                     uint32_t color) {
    vx_text(s, f, cx - vx_text_width_font(f, t) / 2, y, t, color, VX_TRANSPARENT);
}

/* Words wrapped into `width`, a line every `line` pixels; returns the y after. */
static int paragraph(struct vx_surface *s, const struct vx_font *f, int x, int y, int width,
                     int line, const char *t, uint32_t color) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", t);
    char *p = buffer;
    while (*p) {
        size_t fit = vx_text_fit_bytes(f, p, width);
        if (fit < strlen(p)) {
            size_t cut = fit;
            while (cut > 0 && p[cut] != ' ') {
                cut--;
            }
            fit = cut ? cut : fit;
        }
        char saved = p[fit];
        p[fit] = '\0';
        text(s, f, x, y, p, color);
        p[fit] = saved;
        p += fit;
        while (*p == ' ') {
            p++;
        }
        y += line;
    }
    return y;
}

/* The same, each line centered on cx. */
static void centered_paragraph(struct vx_surface *s, const struct vx_font *f, int cx, int y,
                               int width, int line, const char *t, uint32_t color) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", t);
    char *p = buffer;
    while (*p) {
        size_t fit = vx_text_fit_bytes(f, p, width);
        if (fit < strlen(p)) {
            size_t cut = fit;
            while (cut > 0 && p[cut] != ' ') {
                cut--;
            }
            fit = cut ? cut : fit;
        }
        char saved = p[fit];
        p[fit] = '\0';
        centered(s, f, cx, y, p, color);
        p[fit] = saved;
        p += fit;
        while (*p == ' ') {
            p++;
        }
        y += line;
    }
}

/* A vertical gradient. */
static void gradient(struct vx_surface *s, int x, int y, int w, int h, uint32_t top,
                     uint32_t bottom) {
    for (int row = 0; row < h; row++) {
        vx_fill(s, x, y + row, w, 1, vx_mix(top, bottom, h > 1 ? row * 255 / (h - 1) : 0));
    }
}

static void circle(struct vx_surface *s, int cx, int cy, int r, uint32_t color) {
    vx_fill_rounded(s, cx - r, cy - r, 2 * r, 2 * r, r, color, 255);
}

/* A tick in a box of `size` at (x, y), `t` pixels thick. */
static void tick(struct vx_surface *s, int x, int y, int size, int t, uint32_t color) {
    int ax = x + size * 2 / 10, ay = y + size * 5 / 10;
    int bx = x + size * 4 / 10, by = y + size * 7 / 10;
    int cx = x + size * 8 / 10, cy = y + size * 3 / 10;
    for (int i = 0; i <= bx - ax; i++) {
        vx_fill(s, ax + i, ay + i * (by - ay) / (bx - ax ? bx - ax : 1), t, t, color);
    }
    for (int i = 0; i <= cx - bx; i++) {
        vx_fill(s, bx + i, by - i * (by - cy) / (cx - bx ? cx - bx : 1), t, t, color);
    }
}

/* A cross in a box of `size`. */
static void cross(struct vx_surface *s, int x, int y, int size, int t, uint32_t color) {
    for (int i = size / 4; i <= size * 3 / 4; i++) {
        vx_fill(s, x + i - t / 2, y + i - t / 2, t, t, color);
        vx_fill(s, x + size - i - t / 2, y + i - t / 2, t, t, color);
    }
}

/* Vexa's mark: a glossy rounded square with a V in it. */
static void logo(struct vx_surface *s, int x, int y, int size) {
    vx_fill_rounded(s, x + 2, y + 4, size, size, size / 4, 0x000000, 50); /* Shadow. */
    vx_draw_gel(s, x, y, size, size, size / 4, VX_COLOR_ACCENT);
    const struct vx_font *f = bold(size * 6 / 10);
    centered(s, f, x + size / 2, y + (size - vx_font_height(f)) / 2, "V", 0xffffff);
}

static void icon(struct vx_surface *s, const struct vx_image *image, int x, int y, int size) {
    if (image) {
        vx_blit_alpha(s, x, y, size, size, &image->surface);
    }
}

/* A button: the primary one a gel of `color`, the others plain. */
static void button(struct vx_surface *s, int x, int y, int w, const char *label, bool primary,
                   uint32_t color, bool enabled, enum hit_kind kind) {
    int index = hit_count;
    add_hit(x, y, w, 30, kind, 0);
    bool hot = hot_hit == index && enabled;
    if (primary && enabled) {
        vx_draw_gel(s, x, y, w, 30, 15, hot ? vx_mix(color, 0xffffff, 40) : color);
        centered(s, bold(VX_UI_FONT_SIZE), x + w / 2, y + 7, label, 0xffffff);
    } else {
        vx_draw_button_flags(s, x, y, w, 30, label, enabled ? (hot ? VX_BUTTON_HOT : 0)
                                                             : VX_BUTTON_DISABLED);
    }
}

/* The page's title and what it's about; returns where its content starts. */
static int heading(struct vx_surface *s, const char *title, const char *subtitle) {
    text(s, bold(24), CONTENT, 34, title, VX_COLOR_TEXT);
    int y = 34 + vx_font_height(bold(24)) + 6;
    if (subtitle) {
        y = paragraph(s, sans(14), CONTENT, y, CONTENT_W, 20, subtitle, VX_COLOR_DIM);
    }
    return y + 18;
}

/* The bar along the bottom, with Back and the button that goes on. */
static void footer(struct vx_surface *s, const char *back, const char *next, bool next_enabled,
                   uint32_t next_color) {
    vx_fill(s, SIDEBAR, HEIGHT - FOOTER, WIDTH - SIDEBAR, 1, VX_COLOR_LINE);
    int y = HEIGHT - FOOTER + 17;
    int x = WIDTH - 36;
    if (next) {
        int w = vx_text_width_font(bold(VX_UI_FONT_SIZE), next) + 48;
        w = w < 120 ? 120 : w;
        x -= w;
        button(s, x, y, w, next, true, next_color, next_enabled, HIT_NEXT);
        x -= 10;
    }
    if (back) {
        int w = vx_text_width(back) + 40;
        w = w < 100 ? 100 : w;
        x -= w;
        button(s, x, y, w, back, false, 0, true, HIT_BACK);
    }
}

/* ---- The sidebar ---- */

static int current_step(void) {
    switch (page) {
    case WELCOME: return 0;
    case CHOOSE: return 1;
    case ACCOUNT: return 2;
    case CONFIRM: return 3;
    case INSTALLING:
    case FAILED: return 4;
    case DONE: return 5;
    }
    return 0;
}

static void draw_sidebar(struct vx_surface *s) {
    uint32_t top = vx_mix(0x1c2033, VX_COLOR_ACCENT, 70);
    uint32_t bottom = vx_mix(0x0d0f18, VX_COLOR_ACCENT, 25);
    gradient(s, 0, 0, SIDEBAR, HEIGHT, top, bottom);
    /* A soft sheen across the top, and an edge on the right. */
    for (int row = 0; row < 120; row++) {
        vx_fill_rounded(s, 0, row, SIDEBAR, 1, 0, 0xffffff, (120 - row) * 18 / 120);
    }
    vx_fill(s, SIDEBAR - 1, 0, 1, HEIGHT, vx_mix(bottom, 0x000000, 80));

    logo(s, 28, 30, 52);
    text(s, bold(18), 92, 34, "Vexa", 0xffffff);
    text(s, sans(13), 92, 58, "Installer", 0xc8cce0);

    int current = current_step();
    int y0 = 130, gap = 46;
    for (int i = 0; i < STEP_COUNT; i++) {
        int cy = y0 + i * gap;
        if (i + 1 < STEP_COUNT) {
            /* The line to the next step: lit where it's done. */
            vx_fill(s, 41, cy + 11, 2, gap - 22, i < current ? VX_COLOR_ACCENT : 0x4a4f66);
        }
        bool failed = page == FAILED && i == current;
        if (i < current || (page == DONE && i == current)) {
            circle(s, 42, cy, 11, VX_COLOR_ACCENT);
            tick(s, 31, cy - 11, 22, 2, 0xffffff);
        } else if (i == current) {
            circle(s, 42, cy, 15, vx_mix(failed ? 0xd9534f : VX_COLOR_ACCENT, top, 140)); /* Glow. */
            circle(s, 42, cy, 11, failed ? 0xd9534f : 0xffffff);
            if (failed) {
                cross(s, 31, cy - 11, 22, 2, 0xffffff);
            } else {
                circle(s, 42, cy, 5, VX_COLOR_ACCENT);
            }
        } else {
            circle(s, 42, cy, 11, 0x4a4f66);
            circle(s, 42, cy, 9, vx_mix(top, bottom, i * 255 / STEP_COUNT));
        }
        const struct vx_font *f = i == current ? bold(14) : sans(14);
        uint32_t color = i == current ? 0xffffff : i < current ? 0xd8dbe8 : 0x8a8fa8;
        text(s, f, 66, cy - vx_font_height(f) / 2, step_names[i], color);
    }
    text(s, sans(11), 28, HEIGHT - 30, "Vexa: a small system of its own", 0x7d82a0);
}

/* ---- The pages ---- */

static void feature(struct vx_surface *s, int y, const struct vx_image *image, const char *title,
                    const char *line) {
    vx_fill_rounded(s, CONTENT, y, 44, 44, 12, vx_mix(VX_COLOR_VIEW, VX_COLOR_ACCENT, 30), 255);
    icon(s, image, CONTENT + 6, y + 6, 32);
    text(s, bold(14), CONTENT + 60, y + 3, title, VX_COLOR_TEXT);
    text(s, sans(13), CONTENT + 60, y + 23, line, VX_COLOR_DIM);
}

static void draw_welcome(struct vx_surface *s) {
    if (!booted_from_cd) {
        int y = heading(s, "Vexa is already installed",
                        "This Vexa is running from a disk. To install it on another one, start "
                        "the computer from the Vexa CD (or USB stick).");
        feature(s, y, computer_icon, "Started from a disk", "Nothing on it needs installing again.");
        footer(s, NULL, "Close", true, VX_COLOR_ACCENT);
        return;
    }
    int y = heading(s, "Welcome to Vexa",
                    "Put Vexa on a disk, so the computer starts it from there: no CD needed, "
                    "and everything you do is kept.");
    feature(s, y, disk_icon, "Starts from your disk",
            "A boot menu for BIOS and UEFI computers alike.");
    feature(s, y + 64, folder_icon, "Keeps your files and settings",
            "Your account, your home folder, your choices in Settings.");
    feature(s, y + 128, program_icon, "With the Linux programs, if you like",
            "bash, Python, X and GTK programs come along (about 190 MB).");
    vx_fill_rounded(s, CONTENT, y + 200, CONTENT_W, 46, 10, vx_mix(VX_COLOR_VIEW, 0xf0ad4e, 40), 255);
    paragraph(s, sans(13), CONTENT + 16, y + 206, CONTENT_W - 32, 17,
              "The disk you choose is erased. Copy what you want to keep off it first.",
              VX_COLOR_TEXT);
    footer(s, NULL, "Continue", true, VX_COLOR_ACCENT);
}

static void draw_choose(struct vx_surface *s) {
    int y = heading(s, "Choose a disk", "Vexa takes all of it. Everything on it will be erased.");
    if (disk_count == 0) {
        vx_fill_rounded(s, CONTENT, y, CONTENT_W, 80, 12, VX_COLOR_VIEW, 255);
        centered(s, bold(14), CONTENT + CONTENT_W / 2, y + 22, "There are no disks to install on.",
                 VX_COLOR_TEXT);
        centered(s, sans(13), CONTENT + CONTENT_W / 2, y + 44,
                 "Vexa needs one of at least 512 MB (1 GB with the Linux programs).",
                 VX_COLOR_DIM);
    }
    for (int i = 0; i < disk_count && i < 4; i++) {
        int ry = y + i * 74;
        bool on = i == chosen;
        add_hit(CONTENT, ry, CONTENT_W, 64, HIT_DISK, i);
        if (on) {
            vx_fill_rounded(s, CONTENT - 2, ry - 2, CONTENT_W + 4, 68, 14, VX_COLOR_ACCENT, 255);
            vx_fill_rounded(s, CONTENT, ry, CONTENT_W, 64, 12,
                            vx_mix(VX_COLOR_VIEW, VX_COLOR_ACCENT, 28), 255);
        } else {
            vx_fill_rounded(s, CONTENT, ry, CONTENT_W, 64, 12, VX_COLOR_LINE, 255);
            vx_fill_rounded(s, CONTENT + 1, ry + 1, CONTENT_W - 2, 62, 11, VX_COLOR_VIEW, 255);
        }
        icon(s, disk_icon, CONTENT + 14, ry + 12, 40);
        char line[128], size[24];
        snprintf(line, sizeof(line), "%s", disks[i].model);
        text(s, bold(14), CONTENT + 66, ry + 12, line, VX_COLOR_TEXT);
        if (disks[i].size) {
            format_size(size, sizeof(size), disks[i].size);
            snprintf(line, sizeof(line), "%s  -  %s", size, disks[i].name);
        } else {
            snprintf(line, sizeof(line), "%s", disks[i].name);
        }
        text(s, sans(13), CONTENT + 66, ry + 34, line, VX_COLOR_DIM);
        if (disks[i].size && disks[i].size < 512ULL * 1024 * 1024) {
            text(s, sans(12), CONTENT + CONTENT_W - 120, ry + 24, "Too small", 0xd9534f);
        } else if (on) {
            circle(s, CONTENT + CONTENT_W - 30, ry + 32, 12, VX_COLOR_ACCENT);
            tick(s, CONTENT + CONTENT_W - 42, ry + 20, 24, 2, 0xffffff);
        }
    }
    /* The Linux programs: an option card. */
    int cy = HEIGHT - FOOTER - 78;
    add_hit(CONTENT, cy, CONTENT_W, 58, HIT_LINUX, 0);
    vx_fill_rounded(s, CONTENT, cy, CONTENT_W, 58, 12, VX_COLOR_LINE, 255);
    vx_fill_rounded(s, CONTENT + 1, cy + 1, CONTENT_W - 2, 56, 11, VX_COLOR_VIEW, 255);
    vx_draw_check(s, CONTENT + 16, cy + 21, with_linux);
    text(s, bold(14), CONTENT + 46, cy + 10, "Install the Linux programs too", VX_COLOR_TEXT);
    text(s, sans(13), CONTENT + 46, cy + 31, "bash, Python, X and GTK programs: about 190 MB more",
         VX_COLOR_DIM);
    footer(s, "Back", "Continue", chosen >= 0, VX_COLOR_ACCENT);
}

static const char *const field_labels[FIELD_COUNT] = {"Your name", "Account name", "Password",
                                                      "Again"};

static int field_y(int i) {
    return 150 + i * 50;
}

static void draw_account(struct vx_surface *s) {
    heading(s, "Make your account",
            "You'll log in with it. It can change the system: install apps, add accounts.");
    /* A picture: the name's first letter. */
    int ax = WIDTH - 36 - 84, ay = field_y(0);
    circle(s, ax + 42, ay + 42, 44, vx_mix(VX_COLOR_ACCENT, 0xffffff, 60));
    circle(s, ax + 42, ay + 42, 40, VX_COLOR_ACCENT);
    char initial[2] = {fields[F_FULL][0] ? fields[F_FULL][0] : '?', '\0'};
    if (initial[0] >= 'a' && initial[0] <= 'z') {
        initial[0] = (char)(initial[0] - 32);
    }
    const struct vx_font *big = bold(34);
    centered(s, big, ax + 42, ay + 42 - vx_font_height(big) / 2, initial, 0xffffff);
    if (fields[F_NAME][0]) {
        char home[80];
        snprintf(home, sizeof(home), "/home/%s", fields[F_NAME]);
        centered(s, sans(12), ax + 42, ay + 96, home, VX_COLOR_DIM);
    }
    int fx = CONTENT + 120, fw = ax - 30 - fx;
    for (int i = 0; i < FIELD_COUNT; i++) {
        int y = field_y(i);
        text(s, sans(13), CONTENT, y + 5, field_labels[i], VX_COLOR_TEXT);
        char shown[200] = "";
        if (i >= F_PASSWORD) {
            size_t n = 0;
            for (const char *p = fields[i]; *p && n + 4 < sizeof(shown);) {
                vx_utf8_next(&p);
                n += (size_t)snprintf(shown + n, sizeof(shown) - n, "\xe2\x80\xa2");
            }
        } else {
            snprintf(shown, sizeof(shown), "%s", fields[i]);
        }
        vx_draw_field(s, fx, y, fw, shown, i == field);
        add_hit(fx, y, fw, 26, HIT_FIELD, i);
    }
    int y = field_y(FIELD_COUNT) + 4;
    bool problem = account_problem[0] != '\0';
    vx_fill_rounded(s, CONTENT, y, CONTENT_W, 44, 10,
                    vx_mix(VX_COLOR_VIEW, problem ? 0xd9534f : VX_COLOR_ACCENT, 30), 255);
    paragraph(s, sans(13), CONTENT + 14, y + 6, CONTENT_W - 28, 17,
              problem ? account_problem
                      : "Account names: small letters, digits, - and _. The password may be left "
                        "empty: then anyone at the computer can log in.",
              problem ? 0xc9302c : VX_COLOR_TEXT);
    footer(s, "Back", "Continue", fields[F_NAME][0] != '\0', VX_COLOR_ACCENT);
}

static void summary_row(struct vx_surface *s, int y, const char *label, const char *value,
                        uint32_t color) {
    text(s, sans(13), CONTENT + 20, y, label, VX_COLOR_DIM);
    vx_text(s, bold(13), CONTENT + 170, y, value, color, VX_TRANSPARENT);
}

/* A warning sign: a red triangle with "!". */
static void warning(struct vx_surface *s, int x, int y, int size) {
    for (int row = 0; row < size; row++) {
        int half = row * size / (2 * size) + row / 2;
        half = half > size / 2 ? size / 2 : half;
        vx_fill(s, x + size / 2 - half, y + row, 2 * half + 1, 1, 0xd9534f);
    }
    vx_fill(s, x + size / 2 - 1, y + size / 3, 3, size / 3, 0xffffff);
    vx_fill(s, x + size / 2 - 1, y + size * 3 / 4, 3, 3, 0xffffff);
}

static void draw_confirm(struct vx_surface *s) {
    const struct disk *d = &disks[chosen];
    int y = heading(s, "Ready to install", "Check that it's all right: this is the last step back.");
    vx_draw_sheet(s, CONTENT, y, CONTENT_W, 170);
    char line[160], size[24] = "";
    if (d->size) {
        format_size(size, sizeof(size), d->size);
    }
    snprintf(line, sizeof(line), "%s (%s%s%s)", d->model, d->name, size[0] ? ", " : "", size);
    summary_row(s, y + 20, "Disk", line, VX_COLOR_TEXT);
    snprintf(line, sizeof(line), "%s%s%s", fields[F_FULL][0] ? fields[F_FULL] : fields[F_NAME],
             fields[F_FULL][0] ? ", " : "", fields[F_FULL][0] ? fields[F_NAME] : "");
    summary_row(s, y + 48, "Account", line, VX_COLOR_TEXT);
    summary_row(s, y + 76, "Password", fields[F_PASSWORD][0] ? "Set" : "None", VX_COLOR_TEXT);
    summary_row(s, y + 104, "Linux programs", with_linux ? "Yes (about 190 MB)" : "No",
                VX_COLOR_TEXT);
    summary_row(s, y + 132, "Partitions", "128 MB to start from, the rest for Vexa",
                VX_COLOR_TEXT);
    int wy = y + 190;
    vx_fill_rounded(s, CONTENT, wy, CONTENT_W, 56, 12, vx_mix(VX_COLOR_VIEW, 0xd9534f, 40), 255);
    warning(s, CONTENT + 16, wy + 14, 26);
    snprintf(line, sizeof(line), "Everything on %s will be erased, for good.", d->name);
    text(s, bold(14), CONTENT + 56, wy + 10, line, VX_COLOR_TEXT);
    vx_draw_text_fit(s, CONTENT + 56, wy + 31, CONTENT_W - 72, d->details, VX_COLOR_DIM,
                     VX_TRANSPARENT);
    footer(s, "Back", "Erase and Install", true, 0xd9534f);
}

/* An arc of dots going round: something is happening. */
static void spinner(struct vx_surface *s, int cx, int cy, int r) {
    static const int dx[8] = {0, 7, 10, 7, 0, -7, -10, -7}, dy[8] = {-10, -7, 0, 7, 10, 7, 0, -7};
    int phase = (int)(now_ms() / 90 % 8);
    for (int i = 0; i < 8; i++) {
        int age = (phase - i + 8) % 8;
        circle(s, cx + dx[i] * r / 10, cy + dy[i] * r / 10, r / 4 > 1 ? r / 4 : 1,
               vx_mix(VX_COLOR_ACCENT, VX_COLOR_WINDOW, age * 30));
    }
}

/* How far it is, overall: the steps install takes (about 8), and the
 * current one's progress. */
static int overall_percent(void) {
    if (page == DONE) {
        return 100;
    }
    int done = step_count > 0 ? step_count - 1 : 0;
    int p = (done * 100 + percent) / 9;
    return p > 99 ? 99 : p;
}

static void draw_installing(struct vx_surface *s) {
    if (page == DONE) {
        int cx = CONTENT + CONTENT_W / 2, cy = 170;
        circle(s, cx, cy, 58, vx_mix(0x3fbf6f, VX_COLOR_WINDOW, 150));
        circle(s, cx, cy, 50, 0x3fbf6f);
        tick(s, cx - 34, cy - 34, 68, 6, 0xffffff);
        centered(s, bold(24), cx, cy + 80, "Vexa is installed", VX_COLOR_TEXT);
        centered_paragraph(s, sans(14), cx, cy + 120, CONTENT_W - 80, 20,
                  "Take the CD (or USB stick) out and restart: the computer starts Vexa from the "
                  "disk now, and you log in with your account.",
                  VX_COLOR_DIM);
        footer(s, "Close", "Restart", true, VX_COLOR_ACCENT);
        return;
    }
    if (page == FAILED) {
        int cx = CONTENT + CONTENT_W / 2, cy = 150;
        circle(s, cx, cy, 50, 0xd9534f);
        cross(s, cx - 30, cy - 30, 60, 6, 0xffffff);
        centered(s, bold(24), cx, cy + 70, "It didn't work", VX_COLOR_TEXT);
        centered_paragraph(s, sans(14), cx, cy + 110, CONTENT_W - 80, 20,
                  message[0] ? message : "The installer stopped.", 0xc9302c);
        footer(s, NULL, "Close", true, VX_COLOR_ACCENT);
        return;
    }
    int y = heading(s, "Installing Vexa", "This takes a few minutes. The computer can't be used "
                                          "for anything else meanwhile.");
    /* The step it's on, large, with a spinner. */
    const char *now = step_count ? steps[step_count - 1] : "Starting";
    spinner(s, CONTENT + 14, y + 12, 12);
    vx_draw_text_fit(s, CONTENT + 38, y + 2, CONTENT_W - 100, now, VX_COLOR_TEXT, VX_TRANSPARENT);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", overall_percent());
    text(s, bold(14), CONTENT + CONTENT_W - vx_text_width_font(bold(14), pct), y + 2, pct,
         VX_COLOR_TEXT);
    vx_draw_progress(s, CONTENT, y + 32, CONTENT_W, 14, (unsigned)overall_percent(), 100);
    /* What's been done. */
    int ly = y + 70;
    vx_fill_rounded(s, CONTENT, ly, CONTENT_W, HEIGHT - FOOTER - 24 - ly, 12, VX_COLOR_VIEW, 255);
    int first = step_count > 7 ? step_count - 7 : 0;
    for (int i = first; i < step_count; i++) {
        int ry = ly + 14 + (i - first) * 26;
        bool last = i == step_count - 1;
        if (last) {
            spinner(s, CONTENT + 24, ry + 8, 7);
        } else {
            circle(s, CONTENT + 24, ry + 8, 8, 0x3fbf6f);
            tick(s, CONTENT + 16, ry, 16, 2, 0xffffff);
        }
        vx_draw_text_fit(s, CONTENT + 44, ry, CONTENT_W - 60, steps[i],
                         last ? VX_COLOR_TEXT : VX_COLOR_DIM, VX_TRANSPARENT);
    }
    long seconds = (now_ms() - started_ms) / 1000;
    char elapsed[48];
    snprintf(elapsed, sizeof(elapsed), "%ld:%02ld so far", seconds / 60, seconds % 60);
    text(s, sans(12), CONTENT, HEIGHT - FOOTER + 24, elapsed, VX_COLOR_DIM);
    vx_fill(s, SIDEBAR, HEIGHT - FOOTER, WIDTH - SIDEBAR, 1, VX_COLOR_LINE);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    hit_count = 0;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
    draw_sidebar(s);
    switch (page) {
    case WELCOME: draw_welcome(s); break;
    case CHOOSE: draw_choose(s); break;
    case ACCOUNT: draw_account(s); break;
    case CONFIRM: draw_confirm(s); break;
    default: draw_installing(s); break;
    }
    vx_window_present(window, 0, 0, s->width, s->height);
}

/* ---- The account ---- */

/* Whether the account is good to go; if not, account_problem says why. */
static bool account_ok(void) {
    account_problem[0] = '\0';
    const char *name = fields[F_NAME];
    bool ok = name[0] >= 'a' && name[0] <= 'z' && strlen(name) < 32;
    for (const char *c = name; ok && *c; c++) {
        ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
    }
    if (!ok) {
        snprintf(account_problem, sizeof(account_problem),
                 "The account name: a small letter first, then letters, digits, - and _.");
    } else if (!strcmp(name, "root") || !strcmp(name, "nobody") || !strcmp(name, "admin")) {
        snprintf(account_problem, sizeof(account_problem), "\"%s\" is taken: choose another.", name);
    } else if (strchr(fields[F_FULL], ':')) {
        snprintf(account_problem, sizeof(account_problem), "Your name can't have a ':' in it.");
    } else if (strcmp(fields[F_PASSWORD], fields[F_AGAIN])) {
        snprintf(account_problem, sizeof(account_problem), "The two passwords are not the same.");
    }
    return !account_problem[0];
}

/* The account name follows the full name (its first word, small letters)
 * until it's typed itself. */
static void suggest_name(void) {
    if (name_typed) {
        return;
    }
    size_t n = 0;
    for (const char *c = fields[F_FULL]; *c && *c != ' ' && n + 1 < sizeof(fields[F_NAME]); c++) {
        char low = *c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c;
        if ((low >= 'a' && low <= 'z') || (n && low >= '0' && low <= '9')) {
            fields[F_NAME][n++] = low;
        }
    }
    fields[F_NAME][n] = '\0';
}

/* ---- Installing ---- */

static void start(void) {
    int ends[2], to_install[2];
    if (vx_pipe(ends) < 0) {
        snprintf(message, sizeof(message), "Couldn't start the installer.");
        page = FAILED;
        return;
    }
    if (vx_pipe(to_install) < 0) {
        vx_close(ends[0]);
        vx_close(ends[1]);
        snprintf(message, sizeof(message), "Couldn't start the installer.");
        page = FAILED;
        return;
    }
    const char *argv[11];
    int argc = 0;
    argv[argc++] = "install";
    argv[argc++] = "--yes";
    if (!with_linux) {
        argv[argc++] = "--no-linux";
    }
    argv[argc++] = "--user";
    argv[argc++] = fields[F_NAME];
    argv[argc++] = "--full";
    argv[argc++] = fields[F_FULL];
    argv[argc++] = "--password-stdin";
    argv[argc++] = disks[chosen].name;
    argv[argc] = NULL;
    extern char **environ;
    unsigned long envc = 0;
    while (environ && environ[envc]) {
        envc++;
    }
    struct vx_spawn spawn = {.argv = argv, .argc = (unsigned long)argc,
                             .envp = (const char *const *)environ, .envc = envc,
                             .handles = {to_install[0], ends[1], ends[1]}};
    child = vx_spawn("/bin/install", &spawn);
    vx_close(ends[1]);
    vx_close(to_install[0]);
    /* The password, on its input (not where others could see it). */
    char line[80];
    int n = snprintf(line, sizeof(line), "%s\n", fields[F_PASSWORD]);
    if (child >= 0) {
        vx_write(to_install[1], line, (size_t)n);
    }
    vx_close(to_install[1]);
    memset(line, 0, sizeof(line));
    memset(fields[F_PASSWORD], 0, sizeof(fields[F_PASSWORD]));
    memset(fields[F_AGAIN], 0, sizeof(fields[F_AGAIN]));
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
    started_ms = now_ms();
    printf("installer: installing on %s%s for %s\n", disks[chosen].name,
           with_linux ? "" : " (no Linux)", fields[F_NAME]);
    fflush(stdout);
}

static void line_from_install(const char *line) {
    if (!strncmp(line, "progress: ", 10)) {
        percent = atoi(line + 10);
    } else if (!strncmp(line, "step: ", 6)) {
        if (step_count == MAX_STEPS) {
            memmove(steps, steps + 1, sizeof(steps[0]) * (MAX_STEPS - 1));
            step_count--;
        }
        snprintf(steps[step_count++], sizeof(steps[0]), "%s", line + 6);
        char *first = steps[step_count - 1];
        if (first[0] >= 'a' && first[0] <= 'z') {
            first[0] = (char)(first[0] - 32);
        }
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

/* Goes on; true if that closes the Installer. */
static bool go_on(void) {
    switch (page) {
    case WELCOME:
        if (!booted_from_cd) {
            return true;
        }
        page = CHOOSE;
        break;
    case CHOOSE:
        if (chosen >= 0) {
            page = ACCOUNT;
            field = F_FULL;
        }
        break;
    case ACCOUNT:
        if (account_ok()) {
            page = CONFIRM;
            printf("installer: the account is %s\n", fields[F_NAME]);
            fflush(stdout);
        }
        break;
    case CONFIRM: start(); break;
    case DONE: vx_power(VX_POWER_RESTART); break;
    case FAILED: return true;
    case INSTALLING: break;
    }
    return false;
}

/* Goes back; true if that closes the Installer. */
static bool go_back(void) {
    switch (page) {
    case CHOOSE: page = WELCOME; return false;
    case ACCOUNT: page = CHOOSE; return false;
    case CONFIRM: page = ACCOUNT; return false;
    case INSTALLING: return false; /* (Not while it works.) */
    default: return true;
    }
}

static int hit_at(int x, int y) {
    for (int i = hit_count - 1; i >= 0; i--) {
        if (vx_inside(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
            return i;
        }
    }
    return -1;
}

static bool pointer(const struct vx_gui_event *e, int *held) {
    bool click = (e->buttons & 1) && !(*held & 1);
    *held = e->buttons;
    int at = hit_at(e->x, e->y);
    bool is_button = at >= 0 && (hits[at].kind == HIT_NEXT || hits[at].kind == HIT_BACK);
    hot_hit = is_button ? at : -1;
    if (!click || at < 0) {
        return false;
    }
    switch (hits[at].kind) {
    case HIT_NEXT: return go_on();
    case HIT_BACK: return go_back();
    case HIT_DISK: chosen = hits[at].index; break;
    case HIT_LINUX: with_linux = !with_linux; break;
    case HIT_FIELD: field = hits[at].index; break;
    }
    return false;
}

/* A key; true if it closes the Installer. */
static bool key(const struct vx_gui_event *e) {
    if (page == ACCOUNT && e->key != VX_KEY_ESC) {
        if (e->key == VX_KEY_ENTER && field + 1 < FIELD_COUNT) {
            field++;
        } else if (e->key == VX_KEY_TAB || e->key == VX_KEY_DOWN) {
            field = (field + 1) % FIELD_COUNT;
        } else if (e->key == VX_KEY_UP) {
            field = (field + FIELD_COUNT - 1) % FIELD_COUNT;
        } else if (e->key == VX_KEY_ENTER) {
            return go_on();
        } else if (vx_field_key(fields[field], sizeof(fields[field]), e)) {
            if (field == F_FULL) {
                suggest_name();
            } else if (field == F_NAME) {
                name_typed = true;
            }
            account_problem[0] = '\0';
        }
        return false;
    }
    if (e->key == VX_KEY_DOWN && page == CHOOSE && disk_count) {
        chosen = chosen + 1 < disk_count ? chosen + 1 : chosen;
    } else if (e->key == VX_KEY_UP && page == CHOOSE && chosen > 0) {
        chosen--;
    } else if (e->key == VX_KEY_SPACE && page == CHOOSE) {
        with_linux = !with_linux;
    } else if (e->key == VX_KEY_ENTER) {
        return go_on();
    } else if (e->key == VX_KEY_ESC) {
        return go_back() && page != WELCOME;
    }
    return false;
}

static struct vx_image *resource(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "/apps/Files.vxapp/Contents/Resources/%s.png", name);
    return vx_image_load(path, VX_IMAGE_ALPHA);
}

int main(void) {
    window = vx_window_create("Installer", WIDTH, HEIGHT);
    if (!window) {
        fprintf(stderr, "installer: no desktop to open a window on\n");
        return 1;
    }
    disk_icon = resource("disk");
    folder_icon = resource("folder");
    program_icon = resource("program");
    computer_icon = resource("computer");
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
        /* (While installing, the spinners turn.) */
        vx_poll(polls, (size_t)count, page == INSTALLING ? 90 : -1);
        if (output >= 0 && polls[1].ready) {
            read_output();
        }
        struct vx_gui_event e;
        int got;
        while ((got = vx_gui_wait(&e, 0)) > 0) {
            bool quit = false;
            switch (e.type) {
            case VX_GUI_CLOSE: quit = page != INSTALLING; break;
            case VX_GUI_POINTER: quit = pointer(&e, &held); break;
            case VX_GUI_KEY: quit = e.value && key(&e); break;
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
