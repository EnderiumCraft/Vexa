#include <vexa/abi.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/font.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/sched.h>
#include <vexa/signal.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/tty.h>

#define INPUT_SIZE 4096
#define LINE_MAX 1024

struct tty {
    struct spinlock lock;
    struct tty_settings settings;
    char input[INPUT_SIZE]; /* Bytes ready for readers (ring buffer). */
    size_t input_head, input_tail;
    size_t lines_ready;     /* Canonical mode: newline-ended lines in `input`. */
    size_t partial_lines;   /* Lines ended by Ctrl+D instead of Enter. */
    size_t end_of_input;    /* Pending Ctrl+D "end of file" markers. */
    char line[LINE_MAX];    /* The line being edited. */
    size_t line_length;
    uint32_t foreground;
    uint16_t rows, columns; /* 0: the console's screen size. */
    bool hung_up;
    struct wait_queue readers;
    tty_output_fn output;
    void *owner;
};

static struct tty console;
struct tty *console_tty = &console;

/* Echo collects under the lock and goes out after it. */
struct echo {
    char text[64];
    size_t length;
};

static void echo(struct echo *e, const char *text, size_t length) {
    for (size_t i = 0; i < length && e->length < sizeof(e->text); i++) {
        e->text[e->length++] = text[i];
    }
}

static bool input_full(struct tty *tty) {
    return (tty->input_head + 1) % INPUT_SIZE == tty->input_tail;
}

static void input_push(struct tty *tty, char c) {
    if (!input_full(tty)) {
        tty->input[tty->input_head] = c;
        tty->input_head = (tty->input_head + 1) % INPUT_SIZE;
    }
}

static void finish_line(struct tty *tty, bool with_newline) {
    for (size_t i = 0; i < tty->line_length; i++) {
        input_push(tty, tty->line[i]);
    }
    if (with_newline) {
        input_push(tty, '\n');
    }
    tty->line_length = 0;
    if (with_newline) {
        tty->lines_ready++;
    } else {
        tty->partial_lines++;
    }
}

/* Output processing (ONLCR) on the way to the screen or the master. */
static void output(struct tty *tty, const char *text, size_t length) {
    if (!(tty->settings.oflag & TTY_OPOST) || !(tty->settings.oflag & TTY_ONLCR) ||
        tty == &console) {
        tty->output(tty, text, length); /* The console does its own newlines. */
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n') {
            if (i > start) {
                tty->output(tty, text + start, i - start);
            }
            tty->output(tty, "\r\n", 2);
            start = i + 1;
        }
    }
    if (start < length) {
        tty->output(tty, text + start, length - start);
    }
}

void tty_receive(struct tty *tty, char c) {
    uint32_t signal_group = 0;
    int signal = 0;
    struct echo e = {.length = 0};
    uint64_t flags = spin_lock_irqsave(&tty->lock);
    struct tty_settings *settings = &tty->settings;
    if ((settings->iflag & TTY_ICRNL) && c == '\r') {
        c = '\n';
    }
    uint32_t lflag = settings->lflag;
    if ((lflag & TTY_ISIG) &&
        (c == settings->cc[TTY_VINTR] || c == settings->cc[TTY_VQUIT])) {
        signal = c == settings->cc[TTY_VINTR] ? VX_SIGINT : VX_SIGQUIT;
        signal_group = tty->foreground;
        tty->line_length = 0;
        if (lflag & TTY_ECHO) {
            echo(&e, c == settings->cc[TTY_VINTR] ? "^C\n" : "^\\\n", 3);
        }
    } else if (lflag & TTY_ICANON) {
        if (c == settings->cc[TTY_VERASE] || c == '\b') {
            if (tty->line_length > 0) {
                tty->line_length--;
                if (lflag & TTY_ECHO) {
                    echo(&e, "\b \b", 3);
                }
            }
        } else if (c == settings->cc[TTY_VKILL]) {
            while (tty->line_length > 0 && (lflag & TTY_ECHO)) {
                echo(&e, "\b \b", 3);
                tty->line_length--;
            }
            tty->line_length = 0;
        } else if (c == settings->cc[TTY_VEOF]) {
            if (tty->line_length == 0) {
                tty->end_of_input++;
            } else {
                finish_line(tty, false);
            }
        } else if (c == '\n') {
            if (lflag & (TTY_ECHO | TTY_ECHONL)) {
                echo(&e, "\n", 1);
            }
            finish_line(tty, true);
        } else if (tty->line_length < LINE_MAX - 1 && (uint8_t)c >= ' ') {
            tty->line[tty->line_length++] = c;
            if (lflag & TTY_ECHO) {
                echo(&e, &c, 1);
            }
        }
    } else {
        input_push(tty, c);
        if (lflag & TTY_ECHO) {
            echo(&e, &c, 1);
        }
    }
    spin_unlock_irqrestore(&tty->lock, flags);
    if (e.length) {
        output(tty, e.text, e.length);
    }
    wait_queue_wake_all(&tty->readers);
    if (signal && signal_group) {
        signal_send_group(signal_group, signal);
    }
}

static void keyboard_input(int key) {
    const char *sequence = NULL;
    switch (key) {
    case KEY_UP: sequence = "\x1b[A"; break;
    case KEY_DOWN: sequence = "\x1b[B"; break;
    case KEY_RIGHT: sequence = "\x1b[C"; break;
    case KEY_LEFT: sequence = "\x1b[D"; break;
    }
    if (sequence) {
        /* Arrow keys only mean something to programs that read raw keys. */
        if (!(console.settings.lflag & TTY_ICANON)) {
            for (; *sequence; sequence++) {
                tty_receive(&console, *sequence);
            }
        }
        return;
    }
    /* Backspace sends DEL, like other terminals. */
    tty_receive(&console, key == '\b' ? 0x7f : (char)key);
}

static void default_settings(struct tty_settings *settings) {
    memset(settings, 0, sizeof(*settings));
    settings->iflag = TTY_ICRNL;
    settings->oflag = TTY_OPOST | TTY_ONLCR;
    settings->cflag = 0x00bf; /* CS8 | CREAD | B38400: nothing reads it, but programs look. */
    settings->lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK | TTY_IEXTEN;
    settings->cc[TTY_VINTR] = 0x03;  /* Ctrl+C */
    settings->cc[TTY_VQUIT] = 0x1c;  /* Ctrl+\ */
    settings->cc[TTY_VERASE] = 0x7f; /* Backspace */
    settings->cc[TTY_VKILL] = 0x15;  /* Ctrl+U */
    settings->cc[TTY_VEOF] = 0x04;   /* Ctrl+D */
    settings->cc[TTY_VMIN] = 1;
    settings->cc[TTY_VSUSP] = 0x1a;  /* Ctrl+Z */
}

static void console_output(struct tty *tty, const char *text, size_t length) {
    (void)tty;
    kwrite(text, length);
}

void tty_init(void) {
    default_settings(&console.settings);
    console.output = console_output;
}

void tty_attach_keyboard(void) {
    keyboard_set_consumer(keyboard_input);
}

struct tty *tty_create(tty_output_fn output_fn, void *owner, uint16_t rows, uint16_t columns) {
    struct tty *tty = kzalloc(sizeof(*tty));
    if (!tty) {
        return NULL;
    }
    default_settings(&tty->settings);
    tty->output = output_fn;
    tty->owner = owner;
    tty->rows = rows;
    tty->columns = columns;
    return tty;
}

void tty_destroy(struct tty *tty) {
    kfree(tty);
}

void *tty_owner(struct tty *tty) {
    return tty->owner;
}

void tty_hang_up(struct tty *tty) {
    uint64_t flags = spin_lock_irqsave(&tty->lock);
    tty->hung_up = true;
    uint32_t group = tty->foreground;
    spin_unlock_irqrestore(&tty->lock, flags);
    wait_queue_wake_all(&tty->readers);
    if (group) {
        signal_send_group(group, VX_SIGHUP);
    }
}

static bool can_read(void *arg) {
    struct tty *tty = arg;
    if (tty->end_of_input || tty->hung_up) {
        return true;
    }
    if (tty->settings.lflag & TTY_ICANON) {
        return tty->lines_ready > 0 || tty->partial_lines > 0;
    }
    return tty->input_head != tty->input_tail || tty->settings.cc[TTY_VMIN] == 0;
}

int64_t tty_read(struct tty *tty, void *buffer, size_t size, bool nonblocking) {
    if (size == 0) {
        return 0;
    }
    if (nonblocking && !can_read(tty)) {
        return -VX_EAGAIN;
    }
    int error = wait_queue_wait_interruptible(&tty->readers, can_read, tty);
    if (error) {
        return error;
    }
    uint64_t flags = spin_lock_irqsave(&tty->lock);
    size_t n = 0;
    char *out = buffer;
    if (tty->input_head == tty->input_tail && (tty->end_of_input || tty->hung_up)) {
        if (tty->end_of_input) {
            tty->end_of_input--; /* Ctrl+D on an empty line: this read returns 0. */
        }
    } else {
        bool canonical = tty->settings.lflag & TTY_ICANON;
        while (n < size && tty->input_head != tty->input_tail) {
            char c = tty->input[tty->input_tail];
            tty->input_tail = (tty->input_tail + 1) % INPUT_SIZE;
            out[n++] = c;
            if (canonical && c == '\n') {
                break;
            }
        }
        if (canonical && n > 0 && out[n - 1] == '\n') {
            tty->lines_ready--;
        } else if (canonical && tty->input_head == tty->input_tail) {
            tty->partial_lines = 0;
        }
    }
    spin_unlock_irqrestore(&tty->lock, flags);
    return (int64_t)n;
}

int64_t tty_write(struct tty *tty, const void *buffer, size_t size) {
    if (tty->hung_up) {
        return -VX_EIO;
    }
    output(tty, buffer, size);
    return (int64_t)size;
}

void tty_get_settings(struct tty *tty, struct tty_settings *out) {
    uint64_t flags = spin_lock_irqsave(&tty->lock);
    *out = tty->settings;
    spin_unlock_irqrestore(&tty->lock, flags);
}

void tty_set_settings(struct tty *tty, const struct tty_settings *in) {
    uint64_t flags = spin_lock_irqsave(&tty->lock);
    bool was_canonical = tty->settings.lflag & TTY_ICANON;
    tty->settings = *in;
    if (was_canonical && !(tty->settings.lflag & TTY_ICANON) && tty->line_length) {
        finish_line(tty, false); /* Switching to raw: hand over what was typed. */
    }
    spin_unlock_irqrestore(&tty->lock, flags);
    wait_queue_wake_all(&tty->readers);
}

uint32_t tty_foreground(struct tty *tty) {
    return tty->foreground;
}

void tty_set_foreground(struct tty *tty, uint32_t group) {
    tty->foreground = group;
}

void tty_window_size(struct tty *tty, uint16_t *rows, uint16_t *columns) {
    if (tty->rows && tty->columns) {
        *rows = tty->rows;
        *columns = tty->columns;
        return;
    }
    *rows = (uint16_t)(fb_height() / FONT_HEIGHT);
    *columns = (uint16_t)(fb_width() / FONT_WIDTH);
    if (!*rows || !*columns) {
        *rows = 25;
        *columns = 80;
    }
}

void tty_set_window_size(struct tty *tty, uint16_t rows, uint16_t columns) {
    bool changed = tty->rows != rows || tty->columns != columns;
    tty->rows = rows;
    tty->columns = columns;
    if (changed && tty->foreground) {
        signal_send_group(tty->foreground, VX_SIGWINCH);
    }
}

size_t tty_bytes_ready(struct tty *tty) {
    uint64_t flags = spin_lock_irqsave(&tty->lock);
    size_t n = (tty->input_head + INPUT_SIZE - tty->input_tail) % INPUT_SIZE;
    spin_unlock_irqrestore(&tty->lock, flags);
    return n;
}

uint32_t tty_poll(struct tty *tty) {
    uint32_t ready = OBJECT_WRITABLE;
    if (can_read(tty) && (tty->input_head != tty->input_tail || tty->end_of_input ||
                          tty->hung_up)) {
        ready |= OBJECT_READABLE;
    }
    if (tty->hung_up) {
        ready |= OBJECT_HANGUP;
    }
    return ready;
}
