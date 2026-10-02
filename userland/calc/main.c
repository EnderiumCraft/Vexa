/* calc: the Calculator. Click the buttons or type: digits, + - * / %,
 * Enter or = for the answer, Backspace, Escape (or C) to clear, Ctrl+C to
 * copy the answer.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/gui.h>
#include <vexa/syscall.h>

#define WIDTH 300
#define HEIGHT 420
#define DISPLAY 110
#define PAD 10
#define COLS 4
#define ROWS 5

static struct vx_window *window;
static char entry[32] = "0";   /* What's being typed. */
static long double value;     /* What's kept, before the operator. */
static char op;               /* The operator waiting, or 0. */
static bool fresh = true;     /* The next digit starts a new number. */
static char history[64];      /* "12 × 3 =" above the number. */
static int hot = -1, pressed = -1;
static bool ctrl;

static const char *const keys[ROWS][COLS] = {
    {"C", "+/-", "%", "\xc3\xb7"},
    {"7", "8", "9", "\xc3\x97"},
    {"4", "5", "6", "\xe2\x88\x92"},
    {"1", "2", "3", "+"},
    {"0", "", ".", "="},
};

/* A number as text: up to 12 significant digits, without trailing zeros. */
static void format(long double v, char *out, size_t size) {
    if (v != v) {
        snprintf(out, size, "Error");
        return;
    }
    bool negative = v < 0;
    if (negative) {
        v = -v;
    }
    if (v >= 1e15L) {
        /* Big: digits and a power of ten. */
        int exponent = 0;
        while (v >= 10) {
            v /= 10;
            exponent++;
        }
        long long digits = (long long)(v * 1e8L + 0.5L);
        snprintf(out, size, "%s%lld.%08llde%d", negative ? "-" : "", digits / 100000000,
                 digits % 100000000, exponent);
        char *e = strchr(out, 'e');
        char *z = e;
        while (z[-1] == '0') {
            z--;
        }
        if (z[-1] == '.') {
            z--;
        }
        memmove(z, e, strlen(e) + 1);
        return;
    }
    long long whole = (long long)v;
    int decimals = 12;
    for (long long w = whole; w >= 10 && decimals > 0; w /= 10) {
        decimals--;
    }
    long double scale = 1;
    for (int i = 0; i < decimals; i++) {
        scale *= 10;
    }
    long long fraction = (long long)((v - (long double)whole) * scale + 0.5L);
    if ((long double)fraction >= scale) {
        whole++;
        fraction = 0;
    }
    char digits[24];
    snprintf(digits, sizeof(digits), "%0*lld", decimals, fraction);
    int end = decimals;
    while (end > 0 && digits[end - 1] == '0') {
        end--;
    }
    digits[end] = '\0';
    snprintf(out, size, "%s%lld%s%s", negative && (whole || end) ? "-" : "", whole, end ? "." : "",
             digits);
}

static long double parse(const char *s) {
    long double v = 0, scale = 1;
    bool negative = *s == '-', point = false;
    for (s += negative; *s; s++) {
        if (*s == '.') {
            point = true;
        } else if (*s >= '0' && *s <= '9') {
            if (point) {
                scale /= 10;
                v += (*s - '0') * scale;
            } else {
                v = v * 10 + (*s - '0');
            }
        }
    }
    return negative ? -v : v;
}

static const char *op_text(char o) {
    return o == '/' ? "\xc3\xb7" : o == '*' ? "\xc3\x97" : o == '-' ? "\xe2\x88\x92" : "+";
}

static long double apply(long double a, char o, long double b) {
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? (long double)(0.0 / 0.0) : a / b;
    }
    return b;
}

static void equals(void) {
    if (!op) {
        return;
    }
    char a[32], b[32];
    format(value, a, sizeof(a));
    snprintf(b, sizeof(b), "%s", entry);
    value = apply(value, op, parse(entry));
    snprintf(history, sizeof(history), "%s %s %s =", a, op_text(op), b);
    format(value, entry, sizeof(entry));
    op = 0;
    fresh = true;
    printf("calc: %s %s\n", history, entry);
    fflush(stdout);
}

static void input(const char *key) {
    if (!strcmp(entry, "Error")) {
        snprintf(entry, sizeof(entry), "0");
        fresh = true;
    }
    if ((key[0] >= '0' && key[0] <= '9') || key[0] == '.') {
        if (fresh) {
            snprintf(entry, sizeof(entry), "%s", key[0] == '.' ? "0." : key);
            fresh = false;
        } else if (strlen(entry) < 16 && !(key[0] == '.' && strchr(entry, '.'))) {
            if (!strcmp(entry, "0") && key[0] != '.') {
                entry[0] = '\0';
            }
            strcat(entry, key);
        }
        return;
    }
    if (!strcmp(key, "C")) {
        snprintf(entry, sizeof(entry), "0");
        value = 0;
        op = 0;
        history[0] = '\0';
        fresh = true;
    } else if (!strcmp(key, "+/-")) {
        if (entry[0] == '-') {
            memmove(entry, entry + 1, strlen(entry));
        } else if (strcmp(entry, "0") && strlen(entry) < 30) {
            memmove(entry + 1, entry, strlen(entry) + 1);
            entry[0] = '-';
        }
    } else if (!strcmp(key, "%")) {
        long double v = parse(entry) / 100;
        if (op == '+' || op == '-') {
            v *= value; /* 50 + 10% is 55. */
        }
        format(v, entry, sizeof(entry));
        fresh = true;
    } else if (!strcmp(key, "=")) {
        equals();
    } else if (!strcmp(key, "back")) {
        if (!fresh && strlen(entry) > 1) {
            entry[strlen(entry) - 1] = '\0';
        } else {
            snprintf(entry, sizeof(entry), "0");
            fresh = true;
        }
    } else {
        char o = !strcmp(key, "+") ? '+' : !strcmp(key, "\xe2\x88\x92") ? '-'
                 : !strcmp(key, "\xc3\x97") ? '*' : '/';
        if (op && !fresh) {
            equals();
        } else {
            value = parse(entry);
        }
        op = o;
        char a[32];
        format(value, a, sizeof(a));
        snprintf(history, sizeof(history), "%s %s", a, op_text(o));
        fresh = true;
    }
}

static void key_rect(int row, int col, int *x, int *y, int *w, int *h) {
    int cw = (WIDTH - PAD * (COLS + 1)) / COLS, ch = (HEIGHT - DISPLAY - PAD * (ROWS + 1)) / ROWS;
    *x = PAD + col * (cw + PAD);
    *y = DISPLAY + PAD + row * (ch + PAD);
    *w = row == 4 && col == 0 ? 2 * cw + PAD : cw;
    *h = ch;
}

static int key_at(int px, int py) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x, y, w, h;
            key_rect(r, c, &x, &y, &w, &h);
            if (keys[r][c][0] && vx_inside(px, py, x, y, w, h)) {
                return r * COLS + c;
            }
        }
    }
    return -1;
}

static void rounded(struct vx_surface *s, int x, int y, int w, int h, uint32_t color) {
    vx_fill(s, x + 3, y, w - 6, h, color);
    vx_fill(s, x, y + 3, w, h - 6, color);
    vx_fill(s, x + 1, y + 1, w - 2, h - 2, color);
}

static void draw(void) {
    struct vx_surface *s = &window->surface;
    bool dark = vx_theme.dark;
    vx_fill(s, 0, 0, WIDTH, HEIGHT, dark ? 0x1c1c22 : 0xf2f2f5);
    /* The display: what's been done above, the number big. */
    const struct vx_font *small = vx_font_ui();
    int hw = vx_text_width_font(small, history);
    vx_text(s, small, WIDTH - PAD - 4 - hw, 16, history, VX_COLOR_DIM, VX_TRANSPARENT);
    int size = 44;
    const struct vx_font *big = vx_font(VX_FACE_SANS, size);
    while (vx_text_width_font(big, entry) > WIDTH - 2 * PAD - 8 && size > 16) {
        big = vx_font(VX_FACE_SANS, size -= 4);
    }
    int ew = vx_text_width_font(big, entry);
    vx_text(s, big, WIDTH - PAD - 4 - ew, DISPLAY - 12 - vx_font_height(big), entry,
            VX_COLOR_TEXT, VX_TRANSPARENT);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (!keys[r][c][0]) {
                continue;
            }
            int x, y, w, h;
            key_rect(r, c, &x, &y, &w, &h);
            int i = r * COLS + c;
            bool operator = c == 3, top = r == 0 && c < 3;
            uint32_t color = operator ? 0xff9f0a : top ? (dark ? 0x5a5a64 : 0xd4d4dc)
                                                       : (dark ? 0x34343c : 0xffffff);
            if (operator && op && keys[r][c][0] == op_text(op)[0] && !strcmp(keys[r][c], op_text(op)) && fresh) {
                color = 0xffd08a;
            }
            if (i == pressed) {
                color = vx_mix(color, 0xffffff, 90);
            } else if (i == hot) {
                color = vx_mix(color, 0xffffff, 30);
            }
            rounded(s, x, y, w, h, color);
            const struct vx_font *f = vx_font(VX_FACE_SANS, 20);
            uint32_t text = operator ? 0xffffff : top && dark ? 0xffffff : dark ? 0xf0f0f4 : 0x1c1c22;
            int tw = vx_text_width_font(f, keys[r][c]);
            vx_text(s, f, x + (w - tw) / 2, y + (h - vx_font_height(f)) / 2, keys[r][c], text,
                    VX_TRANSPARENT);
        }
    }
    vx_window_present(window, 0, 0, WIDTH, HEIGHT);
}

static void key_event(const struct vx_gui_event *e) {
    if (e->key == VX_KEY_LEFTCTRL || e->key == VX_KEY_RIGHTCTRL) {
        ctrl = e->value != 0;
        return;
    }
    if (!e->value) {
        return;
    }
    if (ctrl && e->key == 46) { /* C: copy */
        vx_clipboard_set(entry, strlen(entry));
        return;
    }
    if (ctrl && e->key == 47) { /* V: paste a number */
        char *text = vx_clipboard_get();
        if (text) {
            long double v = parse(text);
            format(v, entry, sizeof(entry));
            fresh = true;
            free(text);
        }
        return;
    }
    int c = e->character;
    char key[4] = {(char)c, 0};
    if ((c >= '0' && c <= '9') || c == '.' || c == ',') {
        key[0] = c == ',' ? '.' : (char)c;
        input(key);
    } else if (c == '+') {
        input("+");
    } else if (c == '-') {
        input("\xe2\x88\x92");
    } else if (c == '*' || c == 'x') {
        input("\xc3\x97");
    } else if (c == '/') {
        input("\xc3\xb7");
    } else if (c == '%') {
        input("%");
    } else if (c == '=' || e->key == VX_KEY_ENTER) {
        input("=");
    } else if (e->key == VX_KEY_BACKSPACE) {
        input("back");
    } else if (e->key == VX_KEY_ESC || c == 'c' || c == 'C') {
        input("C");
    }
}

int main(void) {
    window = vx_window_create("Calculator", WIDTH, HEIGHT);
    if (!window) {
        fprintf(stderr, "calc: no desktop to open a window on\n");
        return 1;
    }
    int held = 0;
    for (;;) {
        draw();
        struct vx_gui_event e;
        if (vx_gui_wait(&e, -1) <= 0) {
            return 0;
        }
        switch (e.type) {
        case VX_GUI_CLOSE:
            vx_window_destroy(window);
            return 0;
        case VX_GUI_KEY: key_event(&e); break;
        case VX_GUI_FOCUS:
            if (!e.value) {
                ctrl = false;
            }
            break;
        case VX_GUI_POINTER: {
            hot = key_at(e.x, e.y);
            bool down = (e.buttons & 1) && !(held & 1);
            bool up = !(e.buttons & 1) && (held & 1);
            held = e.buttons;
            if (down) {
                pressed = hot;
            }
            if (up) {
                if (pressed >= 0 && pressed == hot) {
                    input(keys[pressed / COLS][pressed % COLS]);
                }
                pressed = -1;
            }
            break;
        }
        }
    }
}
