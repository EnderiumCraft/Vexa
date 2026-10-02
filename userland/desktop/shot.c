/* Screenshots: PrintScreen takes the screen, Alt+PrintScreen the window in
 * front, and Shift+PrintScreen an area dragged out with the pointer. They
 * go to the Pictures folder as PNG files ("Screenshot 2026-10-02 at
 * 20.45.13.png"); the screen flashes, and a notification says where. */
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>
#include "shell.h"

bool shot_selecting;
static struct vx_surface frozen; /* The screen when an area is chosen. */
static bool dragging;
static int start_x, start_y;
static long flash_start = -1;
#define FLASH_MS 260

/* The whole screen, without the pointer (main.c). */
bool capture_screen(struct vx_surface *out);

static void free_surface(struct vx_surface *s) {
    if (s->pixels) {
        vx_unmap(s->pixels, (size_t)s->stride * s->height * 4);
        s->pixels = NULL;
    }
}

static bool new_surface(struct vx_surface *s, int width, int height) {
    s->pixels = vx_map((size_t)width * height * 4, VX_MAP_WRITE);
    s->width = s->stride = width;
    s->height = height;
    return s->pixels != NULL;
}

static void save(const struct vx_surface *s) {
    vx_mkdir(HOME);
    vx_mkdir(PICTURES_FOLDER);
    struct vx_date d;
    char path[160];
    if (local_date(&d)) {
        snprintf(path, sizeof(path), "%s/Screenshot %d-%02d-%02d at %02d.%02d.%02d.png",
                 PICTURES_FOLDER, d.year, d.month, d.day, d.hour, d.minute, d.second);
    } else {
        snprintf(path, sizeof(path), "%s/Screenshot %ld.png", PICTURES_FOLDER, now_ms());
    }
    /* Two in one second: a number after the second. */
    struct vx_stat st;
    for (int n = 2; vx_stat(path, &st) == 0 && n < 100; n++) {
        char *dot = strrchr(path, '.');
        if (n > 2) {
            dot = strrchr(path, ' ');
        }
        snprintf(dot, sizeof(path) - (size_t)(dot - path), " %d.png", n);
    }
    int error = vx_image_save_png(path, s);
    char note[200];
    if (error) {
        snprintf(note, sizeof(note), "Screenshot: couldn't save it (%s)", vx_strerror(error));
    } else {
        snprintf(note, sizeof(note), "Screenshot: saved in Pictures (%s)", strrchr(path, '/') + 1);
    }
    printf("desktop: screenshot %dx%d saved to %s\n", s->width, s->height, path);
    add_note(note);
    flash_start = now_ms();
    want_frames();
}

void screenshot(enum shot_kind kind) {
    if (shot_selecting) {
        return;
    }
    struct vx_surface shot = {0};
    if (kind == SHOT_WINDOW) {
        struct window *w = focused;
        if (!w) {
            kind = SHOT_SCREEN;
        } else {
            struct rect f = frame_rect(w);
            if (!new_surface(&shot, f.width, f.height)) {
                return;
            }
            vx_fill(&shot, 0, 0, f.width, f.height, vx_theme.window);
            draw_window_at(&shot, w, 0, 0);
            save(&shot);
            free_surface(&shot);
            return;
        }
    }
    if (!new_surface(&shot, screen.width, screen.height) || !capture_screen(&shot)) {
        free_surface(&shot);
        return;
    }
    if (kind == SHOT_AREA) {
        frozen = shot;
        shot_selecting = true;
        dragging = false;
        damage_all();
        printf("desktop: choosing an area for a screenshot\n");
        return;
    }
    save(&shot);
    free_surface(&shot);
}

static struct rect selection(void) {
    int x0 = start_x < pointer_x ? start_x : pointer_x;
    int y0 = start_y < pointer_y ? start_y : pointer_y;
    int x1 = start_x > pointer_x ? start_x : pointer_x;
    int y1 = start_y > pointer_y ? start_y : pointer_y;
    return (struct rect){x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

void shot_cancel(void) {
    if (shot_selecting) {
        shot_selecting = false;
        free_surface(&frozen);
        damage_all();
    }
}

void shot_button(bool down) {
    if (down) {
        dragging = true;
        start_x = pointer_x;
        start_y = pointer_y;
        return;
    }
    if (!dragging) {
        return;
    }
    dragging = false;
    struct rect r = selection();
    if (r.width < 4 || r.height < 4) {
        shot_cancel();
        return;
    }
    struct vx_surface part = {frozen.pixels + (long)r.y * frozen.stride + r.x, r.width, r.height,
                              frozen.stride};
    save(&part);
    shot_cancel();
}

void shot_pointer(void) {
    if (dragging) {
        damage_all();
    }
}

void shot_tick(void) {
    if (flash_start < 0) {
        return;
    }
    damage_all();
    if (now_ms() - flash_start > FLASH_MS) {
        flash_start = -1;
    } else {
        want_frames();
    }
}

void shot_draw(struct vx_surface *view, int ox, int oy) {
    if (shot_selecting) {
        /* The screen as it was, dimmed but for the area chosen. */
        vx_blit(view, ox, oy, &frozen, 0, 0, frozen.width, frozen.height);
        struct rect r = dragging ? selection() : (struct rect){0, 0, 0, 0};
        struct rect parts[4] = {
            {0, 0, screen.width, r.y},
            {0, r.y + r.height, screen.width, screen.height - r.y - r.height},
            {0, r.y, r.x, r.height},
            {r.x + r.width, r.y, screen.width - r.x - r.width, r.height},
        };
        if (!dragging) {
            parts[0] = (struct rect){0, 0, screen.width, screen.height};
        }
        for (int i = 0; i < (dragging ? 4 : 1); i++) {
            parts[i].x += ox, parts[i].y += oy;
            blend_rect(view, parts[i], 0x000000, 110);
        }
        if (dragging) {
            vx_draw_outline(view, r.x + ox, r.y + oy, r.width, r.height, 0xffffff);
            char size[32];
            snprintf(size, sizeof(size), "%d x %d", r.width, r.height);
            int w = vx_text_width(size) + 12;
            int lx = r.x + r.width + 8 + w < screen.width ? r.x + r.width + 8 : r.x - w - 8;
            int ly = r.y + r.height + 26 < screen.height ? r.y + r.height + 6 : r.y - 26;
            fill_rounded(view, (struct rect){lx + ox, ly + oy, w, 20}, 6, 0x202020);
            vx_draw_text(view, lx + ox + 6, ly + oy + 2, size, 0xffffff, VX_TRANSPARENT);
        } else {
            const char *hint = "Drag over the area to take; Escape cancels";
            int w = vx_text_width(hint) + 24;
            struct rect box = {(screen.width - w) / 2 + ox, screen.height / 2 - 18 + oy, w, 36};
            fill_rounded(view, box, 10, 0x202020);
            vx_draw_text(view, box.x + 12, box.y + 10, hint, 0xffffff, VX_TRANSPARENT);
        }
    }
    if (flash_start >= 0) {
        long t = now_ms() - flash_start;
        int alpha = t >= FLASH_MS ? 0 : (int)(200 * (FLASH_MS - t) / FLASH_MS);
        blend_rect(view, (struct rect){ox, oy, screen.width, screen.height}, 0xffffff, alpha);
    }
}
