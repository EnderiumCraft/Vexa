#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

/*
 * vx_alert and vx_ask_save_changes (see <vexa/gui.h>): a question in a
 * window of its own, with an icon at the left, the question in bold, a
 * sentence below it, and buttons at the bottom right. Like the Open and Save
 * dialogs it keeps the app's other windows waiting until it's answered.
 */

#define MARGIN 24
#define ICON 56
#define TEXT_X (MARGIN + ICON + 18)
#define BUTTON_H 28
#define BUTTON_GAP 10
#define MAX_LINES 6

struct alert_state {
    struct vx_window *window;
    const struct vx_alert *alert;
    struct vx_image *icon;
    int width, height;
    int widths[8];      /* Each button's. */
    int hot;            /* Under the pointer, or -1. */
    int pressed;        /* Chosen (by a click), or -1. */
    char lines[MAX_LINES][160];   /* The detail, wrapped. */
    int line_count;
    char heading[3][160];         /* The question, wrapped. */
    int heading_count;
};

int vx_text_wrap(char lines[][160], int max, const char *text, int width,
                 const struct vx_font *font) {
    int count = 0;
    size_t most = 159;
    while (text && *text && count < max) {
        size_t fit = 0, i = 0;
        for (;;) {
            size_t j = i; /* The end of the word that starts at i. */
            while (text[j] && text[j] != ' ' && text[j] != '\n') {
                j++;
            }
            char candidate[160];
            if (j > most) {
                break;
            }
            memcpy(candidate, text, j);
            candidate[j] = '\0';
            int w = font ? vx_text_width_font(font, candidate) : vx_text_width(candidate);
            if (fit && w > width) {
                break;
            }
            fit = j;
            if (!text[j] || text[j] == '\n') {
                break;
            }
            i = j + 1;
        }
        if (!fit) {
            fit = strnlen(text, most);
        }
        memcpy(lines[count], text, fit);
        lines[count++][fit] = '\0';
        text += fit;
        if (*text == ' ' || *text == '\n') {
            text++;
        }
    }
    return count;
}

static void button_rect(const struct alert_state *a, int i, int *x, int *y) {
    int right = a->width - MARGIN;
    for (int j = a->alert->button_count - 1; j > i; j--) {
        right -= a->widths[j] + BUTTON_GAP;
    }
    *x = right - a->widths[i];
    *y = a->height - MARGIN - BUTTON_H + 6;
}

static void draw(struct alert_state *a) {
    struct vx_surface *s = &a->window->surface;
    const struct vx_alert *alert = a->alert;
    vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
    if (a->icon) {
        vx_blit_alpha(s, MARGIN, MARGIN - 2, ICON, ICON, &a->icon->surface);
    }
    int y = MARGIN;
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 15);
    for (int i = 0; i < a->heading_count; i++) {
        vx_text(s, bold, TEXT_X, y, a->heading[i], VX_COLOR_TEXT, VX_TRANSPARENT);
        y += vx_font_height(bold) + 2;
    }
    y += 8;
    for (int i = 0; i < a->line_count; i++) {
        vx_draw_text(s, TEXT_X, y, a->lines[i], VX_COLOR_DIM, VX_TRANSPARENT);
        y += VX_LINE_HEIGHT;
    }
    for (int i = 0; i < alert->button_count; i++) {
        int x, by;
        button_rect(a, i, &x, &by);
        unsigned flags = i == alert->default_button || i == a->hot ? VX_BUTTON_HOT : 0;
        vx_draw_button_flags(s, x, by, a->widths[i], BUTTON_H, alert->buttons[i], flags);
    }
    vx_window_present(a->window, 0, 0, s->width, s->height);
}

/* The button a key chooses, or -1. */
static int key_choice(const struct alert_state *a, const struct vx_gui_event *e) {
    const struct vx_alert *alert = a->alert;
    if (e->key == VX_KEY_ENTER) {
        return alert->default_button;
    }
    if (e->key == VX_KEY_ESC) {
        return alert->cancel_button;
    }
    if (e->character > ' ' && e->character < 127) {
        for (int i = 0; i < alert->button_count; i++) {
            if ((alert->buttons[i][0] | 0x20) == (e->character | 0x20)) {
                return i;
            }
        }
    }
    return -1;
}

int vx_alert(const struct vx_alert *alert) {
    struct alert_state a = {.alert = alert, .hot = -1, .pressed = -1};
    int count = alert->button_count > 8 ? 8 : alert->button_count;
    if (count <= 0 || !alert->buttons) {
        return alert->cancel_button;
    }
    struct vx_alert limited = *alert;
    limited.button_count = count;
    a.alert = &limited;
    int buttons_width = 0;
    for (int i = 0; i < count; i++) {
        int w = vx_text_width(alert->buttons[i]) + 36;
        a.widths[i] = w < 84 ? 84 : w;
        buttons_width += a.widths[i] + (i ? BUTTON_GAP : 0);
    }
    int needed = buttons_width + 2 * MARGIN;
    a.width = TEXT_X + 380 + MARGIN;
    a.width = a.width < needed ? needed : a.width;
    a.width = a.width > 620 ? 620 : a.width;
    const struct vx_font *bold = vx_font(VX_FACE_BOLD, 15);
    int room = a.width - TEXT_X - MARGIN;
    a.heading_count = vx_text_wrap(a.heading, 3, alert->message, room, bold);
    a.line_count = vx_text_wrap(a.lines, MAX_LINES, alert->detail, room, NULL);
    int text_height = a.heading_count * (vx_font_height(bold) + 2) + 8 + a.line_count * VX_LINE_HEIGHT;
    int content = text_height > ICON ? text_height : ICON;
    a.height = MARGIN + content + 22 + BUTTON_H + MARGIN - 6;
    if (alert->icon) {
        char path[96];
        snprintf(path, sizeof(path), "/share/icons/%s.png", alert->icon);
        a.icon = vx_image_load(path, VX_IMAGE_ALPHA);
    }
    a.window = vx_window_create_flags(alert->title ? alert->title : "", a.width, a.height, 0);
    if (!a.window) {
        vx_image_free(a.icon);
        return alert->cancel_button;
    }
    int held = 0, answer = -2;
    while (answer == -2) {
        draw(&a);
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            answer = alert->cancel_button;
            break;
        }
        if (e.window != a.window->id) {
            continue; /* Another window's: it waits (and is lost). */
        }
        switch (e.type) {
        case VX_GUI_CLOSE: answer = alert->cancel_button; break;
        case VX_GUI_KEY:
            if (e.value == 1) {
                int choice = key_choice(&a, &e);
                if (choice >= 0) {
                    answer = choice;
                }
            }
            break;
        case VX_GUI_POINTER: {
            bool click = (e.buttons & 1) && !(held & 1);
            held = e.buttons;
            a.hot = -1;
            for (int i = 0; i < count; i++) {
                int x, y;
                button_rect(&a, i, &x, &y);
                if (vx_inside(e.x, e.y, x, y, a.widths[i], BUTTON_H)) {
                    a.hot = i;
                }
            }
            if (click && a.hot >= 0) {
                answer = a.hot;
            }
            break;
        }
        }
    }
    vx_window_destroy(a.window);
    vx_image_free(a.icon);
    printf("alert: \"%s\" answered \"%s\"\n", alert->message,
           answer >= 0 && answer < count ? alert->buttons[answer] : "(none)");
    fflush(stdout);
    return answer;
}

int vx_ask_save_changes(const char *name, const char *verb) {
    char message[200], detail[200];
    snprintf(message, sizeof(message), "Do you want to save the changes you made to \"%s\"?",
             name && name[0] ? name : "Untitled");
    snprintf(detail, sizeof(detail), "Your changes will be lost if you don't save them%s%s.",
             verb && verb[0] ? " before you " : "", verb && verb[0] ? verb : "");
    static const char *const buttons[] = {"Don't Save", "Cancel", "Save"};
    struct vx_alert alert = {
        .title = "Save Changes",
        .icon = "document",
        .message = message,
        .detail = detail,
        .buttons = buttons,
        .button_count = 3,
        .default_button = 2,
        .cancel_button = 1,
    };
    switch (vx_alert(&alert)) {
    case 0: return VX_SAVE_NO;
    case 2: return VX_SAVE_YES;
    default: return VX_SAVE_CANCEL;
    }
}
