#include <vexa/abi.h>
#include <vexa/console.h>
#include <vexa/fb.h>
#include <vexa/font.h>
#include <vexa/keyboard.h>
#include <vexa/kprintf.h>
#include <vexa/sched.h>
#include <vexa/signal.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/tty.h>

#define INPUT_SIZE 4096
#define LINE_MAX 1024

static struct spinlock lock = SPINLOCK_INIT;
static struct tty_settings settings;
static char input[INPUT_SIZE]; /* Bytes ready for readers (ring buffer). */
static size_t input_head, input_tail;
static size_t lines_ready;      /* Canonical mode: newline-ended lines in `input`. */
static size_t partial_lines;    /* Lines ended by Ctrl+D instead of Enter. */
static size_t end_of_input;     /* Pending Ctrl+D "end of file" markers. */
static char line[LINE_MAX];     /* The line being edited. */
static size_t line_length;
static uint32_t foreground;
static struct wait_queue readers = WAIT_QUEUE_INIT;

static void echo(const char *text, size_t length) {
    kwrite(text, length);
}

static bool input_full(void) {
    return (input_head + 1) % INPUT_SIZE == input_tail;
}

static void input_push(char c) {
    if (!input_full()) {
        input[input_head] = c;
        input_head = (input_head + 1) % INPUT_SIZE;
    }
}

static void finish_line(bool with_newline) {
    for (size_t i = 0; i < line_length; i++) {
        input_push(line[i]);
    }
    if (with_newline) {
        input_push('\n');
    }
    line_length = 0;
    if (with_newline) {
        lines_ready++;
    } else {
        partial_lines++;
    }
}

/* Called for every byte from the keyboard, in the keyboard's interrupt. */
static void receive(char c) {
    uint32_t signal_group = 0;
    int signal = 0;
    uint64_t flags = spin_lock_irqsave(&lock);
    if ((settings.iflag & TTY_ICRNL) && c == '\r') {
        c = '\n';
    }
    uint32_t lflag = settings.lflag;
    if ((lflag & TTY_ISIG) && (c == settings.cc[TTY_VINTR] || c == settings.cc[TTY_VQUIT])) {
        signal = c == settings.cc[TTY_VINTR] ? VX_SIGINT : VX_SIGQUIT;
        signal_group = foreground;
        line_length = 0;
        if (lflag & TTY_ECHO) {
            echo(c == settings.cc[TTY_VINTR] ? "^C\n" : "^\\\n", 3);
        }
    } else if (lflag & TTY_ICANON) {
        if (c == settings.cc[TTY_VERASE] || c == '\b') {
            if (line_length > 0) {
                line_length--;
                if (lflag & TTY_ECHO) {
                    echo("\b \b", 3);
                }
            }
        } else if (c == settings.cc[TTY_VKILL]) {
            while (line_length > 0 && (lflag & TTY_ECHO)) {
                echo("\b \b", 3);
                line_length--;
            }
            line_length = 0;
        } else if (c == settings.cc[TTY_VEOF]) {
            if (line_length == 0) {
                end_of_input++;
            } else {
                finish_line(false);
            }
        } else if (c == '\n') {
            if (lflag & (TTY_ECHO | TTY_ECHONL)) {
                echo("\n", 1);
            }
            finish_line(true);
        } else if (line_length < LINE_MAX - 1 && (uint8_t)c >= ' ') {
            line[line_length++] = c;
            if (lflag & TTY_ECHO) {
                echo(&c, 1);
            }
        }
    } else {
        input_push(c);
        if (lflag & TTY_ECHO) {
            echo(&c, 1);
        }
    }
    spin_unlock_irqrestore(&lock, flags);
    wait_queue_wake_all(&readers);
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
        if (!(settings.lflag & TTY_ICANON)) {
            for (; *sequence; sequence++) {
                receive(*sequence);
            }
        }
        return;
    }
    receive(key == '\b' ? 0x7f : (char)key); /* Backspace sends DEL, like other terminals. */
}

void tty_init(void) {
    settings.iflag = TTY_ICRNL;
    settings.oflag = TTY_OPOST | TTY_ONLCR;
    settings.cflag = 0x00bf; /* CS8 | CREAD | B38400: nothing reads it, but programs look. */
    settings.lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK | TTY_IEXTEN;
    settings.cc[TTY_VINTR] = 0x03;  /* Ctrl+C */
    settings.cc[TTY_VQUIT] = 0x1c;  /* Ctrl+\ */
    settings.cc[TTY_VERASE] = 0x7f; /* Backspace */
    settings.cc[TTY_VKILL] = 0x15;  /* Ctrl+U */
    settings.cc[TTY_VEOF] = 0x04;   /* Ctrl+D */
    settings.cc[TTY_VMIN] = 1;
    settings.cc[TTY_VSUSP] = 0x1a;  /* Ctrl+Z */
}

void tty_attach_keyboard(void) {
    keyboard_set_consumer(keyboard_input);
}

static bool can_read(void *unused) {
    (void)unused;
    if (end_of_input) {
        return true;
    }
    if (settings.lflag & TTY_ICANON) {
        return lines_ready > 0 || partial_lines > 0;
    }
    return input_head != input_tail || settings.cc[TTY_VMIN] == 0;
}

int64_t tty_read(void *buffer, size_t size) {
    if (size == 0) {
        return 0;
    }
    int error = wait_queue_wait_interruptible(&readers, can_read, NULL);
    if (error) {
        return error;
    }
    uint64_t flags = spin_lock_irqsave(&lock);
    size_t n = 0;
    char *out = buffer;
    if (input_head == input_tail && end_of_input) {
        end_of_input--; /* Ctrl+D on an empty line: this read returns 0 ("end of file"). */
    } else {
        bool canonical = settings.lflag & TTY_ICANON;
        while (n < size && input_head != input_tail) {
            char c = input[input_tail];
            input_tail = (input_tail + 1) % INPUT_SIZE;
            out[n++] = c;
            if (canonical && c == '\n') {
                break;
            }
        }
        if (canonical && n > 0 && out[n - 1] == '\n') {
            lines_ready--;
        } else if (canonical && input_head == input_tail) {
            partial_lines = 0;
        }
    }
    spin_unlock_irqrestore(&lock, flags);
    return (int64_t)n;
}

int64_t tty_write(const void *buffer, size_t size) {
    kwrite(buffer, size);
    return (int64_t)size;
}

void tty_get_settings(struct tty_settings *out) {
    uint64_t flags = spin_lock_irqsave(&lock);
    *out = settings;
    spin_unlock_irqrestore(&lock, flags);
}

void tty_set_settings(const struct tty_settings *in) {
    uint64_t flags = spin_lock_irqsave(&lock);
    bool was_canonical = settings.lflag & TTY_ICANON;
    settings = *in;
    if (was_canonical && !(settings.lflag & TTY_ICANON) && line_length) {
        finish_line(false); /* Switching to raw: hand over what was typed. */
    }
    spin_unlock_irqrestore(&lock, flags);
    wait_queue_wake_all(&readers);
}

uint32_t tty_foreground(void) {
    return foreground;
}

void tty_set_foreground(uint32_t group) {
    foreground = group;
}

void tty_window_size(uint16_t *rows, uint16_t *columns) {
    *rows = (uint16_t)(fb_height() / FONT_HEIGHT);
    *columns = (uint16_t)(fb_width() / FONT_WIDTH);
    if (!*rows || !*columns) {
        *rows = 25;
        *columns = 80;
    }
}

size_t tty_bytes_ready(void) {
    uint64_t flags = spin_lock_irqsave(&lock);
    size_t n = (input_head + INPUT_SIZE - input_tail) % INPUT_SIZE;
    spin_unlock_irqrestore(&lock, flags);
    return n;
}
