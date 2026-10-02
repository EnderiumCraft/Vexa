/* xclipboard: the clipboard between X programs and Vexa's own.
 *
 * Vexa keeps its clipboard's text in /run/clipboard (see <vexa/desktop.h>).
 * This X program, which xrun starts with the X server, watches both sides:
 * when an X program copies (it becomes the CLIPBOARD selection's owner, which
 * XFixes reports), it asks for the text and writes the file, and tells the
 * desktop; when the file changes, it takes the selection itself and hands
 * the text to X programs that paste.
 *
 * Built with musl against the X libraries (see the Makefile).
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIPBOARD_FILE "/run/clipboard"
#define DESKTOP_SOCKET "/run/desktop"
#define DESKTOP_CLIPBOARD_SET 14

/* <vexa/desktop.h>'s message. */
struct desktop_message {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    char text[104];
};

static Display *display;
static Window window;
static Atom clipboard, utf8, targets, property, text_atom;
static char *owned_text;  /* What we hand out while we own the selection. */
static size_t owned_length;
static struct timespec file_seen;
static off_t file_size_seen = -1;

static void tell_desktop(void) {
    int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s < 0) {
        return;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, DESKTOP_SOCKET);
    if (connect(s, (struct sockaddr *)&address, sizeof(address)) == 0) {
        struct desktop_message m = {.type = DESKTOP_CLIPBOARD_SET, .a = 1};
        write(s, &m, sizeof(m));
    }
    close(s);
}

static void remember_file(void) {
    struct stat st;
    if (stat(CLIPBOARD_FILE, &st) == 0) {
        file_seen = st.st_mtim;
        file_size_seen = st.st_size;
    }
}

/* X has new text: into the file. */
static void write_file(const char *text, size_t length) {
    char temporary[64];
    snprintf(temporary, sizeof(temporary), "%s.x%d", CLIPBOARD_FILE, (int)getpid());
    FILE *f = fopen(temporary, "w");
    if (!f) {
        return;
    }
    fwrite(text, 1, length, f);
    fclose(f);
    rename(temporary, CLIPBOARD_FILE);
    remember_file();
    tell_desktop();
}

/* The file changed: take the selection, with its text. */
static void own_file_text(void) {
    FILE *f = fopen(CLIPBOARD_FILE, "r");
    if (!f) {
        return;
    }
    free(owned_text);
    owned_text = NULL;
    owned_length = 0;
    size_t capacity = 0;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (owned_length + n > capacity) {
            capacity = (owned_length + n) * 2;
            char *more = realloc(owned_text, capacity);
            if (!more) {
                break;
            }
            owned_text = more;
        }
        memcpy(owned_text + owned_length, buffer, n);
        owned_length += n;
    }
    fclose(f);
    XSetSelectionOwner(display, clipboard, window, CurrentTime);
    XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
    XFlush(display);
}

static void file_check(void) {
    struct stat st;
    if (stat(CLIPBOARD_FILE, &st) != 0) {
        return;
    }
    if (st.st_mtim.tv_sec != file_seen.tv_sec || st.st_mtim.tv_nsec != file_seen.tv_nsec ||
        st.st_size != file_size_seen) {
        file_seen = st.st_mtim;
        file_size_seen = st.st_size;
        own_file_text();
    }
}

static void answer_request(XSelectionRequestEvent *r) {
    XSelectionEvent reply = {.type = SelectionNotify, .display = r->display,
                             .requestor = r->requestor, .selection = r->selection,
                             .target = r->target, .property = None, .time = r->time};
    Atom prop = r->property != None ? r->property : r->target;
    if (r->target == targets) {
        Atom offered[] = {targets, utf8, XA_STRING, text_atom};
        XChangeProperty(display, r->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)offered, 4);
        reply.property = prop;
    } else if (r->target == utf8 || r->target == XA_STRING || r->target == text_atom) {
        XChangeProperty(display, r->requestor, prop, r->target == XA_STRING ? XA_STRING : utf8, 8,
                        PropModeReplace, (unsigned char *)(owned_text ? owned_text : ""),
                        (int)owned_length);
        reply.property = prop;
    }
    XSendEvent(display, r->requestor, False, 0, (XEvent *)&reply);
    XFlush(display);
}

static void take_text(void) {
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, window, property, 0, 1 << 24, True, AnyPropertyType, &type,
                           &format, &count, &after, &data) == Success && data && format == 8) {
        write_file((const char *)data, count);
    }
    if (data) {
        XFree(data);
    }
}

int main(void) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "xclipboard: no X display\n");
        return 1;
    }
    int event_base, error_base;
    if (!XFixesQueryExtension(display, &event_base, &error_base)) {
        fprintf(stderr, "xclipboard: no XFixes\n");
        return 1;
    }
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    clipboard = XInternAtom(display, "CLIPBOARD", False);
    utf8 = XInternAtom(display, "UTF8_STRING", False);
    targets = XInternAtom(display, "TARGETS", False);
    text_atom = XInternAtom(display, "TEXT", False);
    property = XInternAtom(display, "VEXA_CLIPBOARD", False);
    XFixesSelectSelectionInput(display, DefaultRootWindow(display), clipboard,
                               XFixesSetSelectionOwnerNotifyMask);
    remember_file();
    if (file_size_seen > 0) {
        own_file_text(); /* What Vexa had before X started. */
    }
    int fd = ConnectionNumber(display);
    for (;;) {
        while (XPending(display)) {
            XEvent e;
            XNextEvent(display, &e);
            if (e.type == SelectionRequest) {
                answer_request(&e.xselectionrequest);
            } else if (e.type == SelectionNotify) {
                if (e.xselection.property != None) {
                    take_text();
                }
            } else if (e.type == event_base + XFixesSelectionNotify) {
                XFixesSelectionNotifyEvent *n = (XFixesSelectionNotifyEvent *)&e;
                if (n->owner != None && n->owner != window) {
                    /* An X program copied: ask it for the text. */
                    XConvertSelection(display, clipboard, utf8, property, window, n->timestamp);
                    XFlush(display);
                }
            }
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);
        struct timeval timeout = {0, 400000}; /* The file, every 0.4 s. */
        if (select(fd + 1, &read_set, NULL, NULL, &timeout) == 0) {
            file_check();
        }
    }
}
