/*
 * Xvexa: an X server on the Vexa desktop.
 *
 * A kdrive server, like Xephyr, whose "host" is Vexa's compositor. It works
 * two ways:
 *
 *  - Windowed (the default): the X screen is one desktop window. Its frame
 *    buffer is the window's pixel buffer (a file in /run/shm that the
 *    desktop maps too), and what X draws is presented as damage.
 *
 *  - Rootless (-rootless): every top-level X window is a desktop window of
 *    its own, with the desktop's title bar, and menus and tooltips
 *    (override-redirect windows) are frameless popups. The Composite
 *    extension draws each top-level window into a pixmap of its own; what
 *    changes there is copied into its desktop window's buffer. The X screen
 *    is as big as the real one and X windows are kept where the desktop
 *    shows them, so X coordinates are screen coordinates.
 *
 * Either way, the keys and pointer events the desktop sends become X input.
 * Key codes are Linux's, so with the "evdev" XKB rules X's key code is the
 * key code plus 8, as on Linux.
 *
 * It talks to the desktop with the protocol in libvexa/include/vexa/desktop.h
 * (the message layout is repeated below), over the local socket
 * /run/desktop. This file is part of Vexa (MIT license); it is built inside
 * xorg-server's source tree (hw/kdrive/vexa) by tools/build-x11.sh.
 *
 *     Xvexa :0 -rootless -noreset &
 *     DISPLAY=:0 xterm
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include "kdrive.h"
#include "compint.h"
#include "damage.h"
#include "dixstruct.h"
#include "inputstr.h"
#include "propertyst.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "xkbsrv.h"

/* ---- The desktop protocol (libvexa/include/vexa/desktop.h) ---- */

#define DESKTOP_SOCKET "/run/desktop"

enum desktop_message_type {
    DESKTOP_CREATE = 1,
    DESKTOP_PRESENT = 2,
    DESKTOP_TITLE = 3,
    DESKTOP_DESTROY = 4,
    DESKTOP_BUFFER = 5,
    DESKTOP_MOVE = 6,
    DESKTOP_INFO = 7,
    DESKTOP_CREATED = 16,
    DESKTOP_KEY = 17,
    DESKTOP_POINTER = 18,
    DESKTOP_CLOSE = 19,
    DESKTOP_FOCUS = 20,
    DESKTOP_CONFIGURE = 21,
    DESKTOP_RESIZED = 22,
    DESKTOP_MOVED = 23,
    DESKTOP_INFO_REPLY = 24,
};

#define DESKTOP_RESIZABLE 0x1
#define DESKTOP_POPUP 0x2

struct desktop_message {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    char text[104];
};

/* ---- State ---- */

typedef struct {
    DamagePtr damage;           /* Windowed: the whole screen's. */
    ScreenBlockHandlerProcPtr block_handler;
    RealizeWindowProcPtr realize_window;
    UnrealizeWindowProcPtr unrealize_window;
} VexaScreenPrivate;

/* Rootless: a top-level X window and its desktop window. */
struct vexa_window {
    WindowPtr window;
    uint32_t id;                /* The desktop's number for it. */
    uint32_t *pixels;           /* Its buffer (a file in /run/shm). */
    size_t size;
    int width, height;
    int x, y;                   /* Where the desktop shows it (= X's drawable x, y). */
    Bool popup;
    DamagePtr damage;
    char title[64];
    struct vexa_window *next;
};

static int desktop = -1;       /* The connection. */
static Bool rootless;
static uint32_t window_id;     /* Windowed: the one window. */
static void *pixels;           /* Windowed: its buffer, the X frame buffer. */
static size_t pixels_size;
static int width = 1024, height = 640;
static char title[64] = "X";
static KdKeyboardInfo *vexa_keyboard;
static KdPointerInfo *vexa_pointer;
static struct vexa_window *windows;
static int buffers_made;
static ScreenPtr vexa_screen;

/* Messages read while waiting for an answer, handled later. */
#define PENDING_MAX 64
static struct desktop_message pending[PENDING_MAX];
static int pending_count;

static int
send_message(struct desktop_message *m)
{
    return send(desktop, m, sizeof(*m), MSG_NOSIGNAL) == sizeof(*m) ? 0 : -1;
}

/* Reads one message, waiting for it. */
static Bool
receive_message(struct desktop_message *m)
{
    for (;;) {
        ssize_t n = recv(desktop, m, sizeof(*m), 0);

        if (n == sizeof(*m))
            return TRUE;
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = { desktop, POLLIN, 0 };

            poll(&p, 1, -1);
            continue;
        }
        return FALSE;
    }
}

/* Waits for the desktop's answer of this type (for this window, if not 0);
 * other messages wait in `pending`. */
static Bool
wait_reply(uint32_t type, uint32_t window, struct desktop_message *reply)
{
    for (;;) {
        if (!receive_message(reply))
            return FALSE;
        if (reply->type == type && (!window || reply->window == window))
            return TRUE;
        if (pending_count < PENDING_MAX)
            pending[pending_count++] = *reply;
    }
}

static void
connect_desktop(void)
{
    struct sockaddr_un address;

    if (desktop >= 0)
        return;
    desktop = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (desktop < 0)
        FatalError("Xvexa: can't make a socket: %s\n", strerror(errno));
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, DESKTOP_SOCKET);
    if (connect(desktop, (struct sockaddr *) &address, sizeof(address)) < 0)
        FatalError("Xvexa: no Vexa desktop at %s (start `desktop` first): %s\n",
                   DESKTOP_SOCKET, strerror(errno));
    fcntl(desktop, F_SETFL, O_NONBLOCK);
}

/* Makes a pixel buffer the desktop can map: returns its mapping and puts
 * its path in `path`. */
static void *
make_buffer(int w, int h, size_t *size, char *path, size_t path_size)
{
    void *memory;
    int fd;

    snprintf(path, path_size, "/run/shm/xvexa-%d-%d", (int) getpid(), ++buffers_made);
    *size = ((size_t) w * h * 4 + 4095) & ~(size_t) 4095;
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, *size) < 0) {
        close(fd);
        unlink(path);
        return NULL;
    }
    memory = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) {
        unlink(path);
        return NULL;
    }
    return memory;
}

/* Opens a desktop window on a buffer; returns its number (0 if refused). */
static uint32_t
create_window(const char *path, const char *name, int w, int h, int flags)
{
    struct desktop_message m;
    size_t length = strlen(path);

    memset(&m, 0, sizeof(m));
    m.type = DESKTOP_CREATE;
    m.a = w;
    m.b = h;
    m.c = flags;
    memcpy(m.text, path, length + 1);
    strncpy(m.text + length + 1, name, sizeof(m.text) - length - 2);
    if (send_message(&m) < 0 || !wait_reply(DESKTOP_CREATED, 0, &m))
        FatalError("Xvexa: the desktop went away\n");
    unlink(path); /* Both sides have it mapped. */
    return m.window;
}

/* Windowed: connects, makes the buffer and opens the window. */
static void
open_window(void)
{
    char path[64];

    connect_desktop();
    pixels = make_buffer(width, height, &pixels_size, path, sizeof(path));
    if (!pixels)
        FatalError("Xvexa: can't make %s: %s\n", path, strerror(errno));
    window_id = create_window(path, title, width, height, 0);
    if (!window_id)
        FatalError("Xvexa: the desktop refused the window\n");
}

/* Rootless: the screen's size. */
static void
ask_screen_size(void)
{
    struct desktop_message m;

    connect_desktop();
    memset(&m, 0, sizeof(m));
    m.type = DESKTOP_INFO;
    if (send_message(&m) < 0 || !wait_reply(DESKTOP_INFO_REPLY, 0, &m))
        FatalError("Xvexa: the desktop went away\n");
    width = m.a;
    height = m.b;
}

/* ---- Rootless windows ---- */

static struct vexa_window *
find_by_id(uint32_t id)
{
    struct vexa_window *vw;

    for (vw = windows; vw; vw = vw->next)
        if (vw->id == id)
            return vw;
    return NULL;
}

static struct vexa_window *
find_by_window(WindowPtr window)
{
    struct vexa_window *vw;

    for (vw = windows; vw; vw = vw->next)
        if (vw->window == window)
            return vw;
    return NULL;
}

/* The window's name: _NET_WM_NAME (UTF-8) or WM_NAME, in ASCII. */
static void
window_name(WindowPtr window, char *out, size_t size)
{
    static Atom net_wm_name;
    PropertyPtr property = NULL;
    size_t i, n;

    if (!net_wm_name)
        net_wm_name = MakeAtom("_NET_WM_NAME", 12, TRUE);
    if (dixLookupProperty(&property, window, net_wm_name, serverClient,
                          DixReadAccess) != Success &&
        dixLookupProperty(&property, window, XA_WM_NAME, serverClient,
                          DixReadAccess) != Success)
        property = NULL;
    if (!property || property->format != 8) {
        snprintf(out, size, "X window");
        return;
    }
    /* Other characters become '?' (one per UTF-8 sequence). */
    for (i = 0, n = 0; i < property->size && n < size - 1; i++) {
        unsigned char c = ((unsigned char *) property->data)[i];

        if (c >= 0x80 && c < 0xc0)
            continue;
        out[n++] = c >= 0x20 && c < 0x7f ? c : c >= 0xc0 ? '?' : ' ';
    }
    out[n] = '\0';
}

/* Copies part of the window (window coordinates) into its buffer. */
static void
copy_window(struct vexa_window *vw, int x, int y, int w, int h)
{
    ScreenPtr screen = vw->window->drawable.pScreen;
    PixmapPtr pixmap = (*screen->GetWindowPixmap) (vw->window);
    int ox = vw->window->drawable.x, oy = vw->window->drawable.y;
    int row;
    uint8_t *base;

#ifdef COMPOSITE
    ox -= pixmap->screen_x;
    oy -= pixmap->screen_y;
#endif
    if (x < 0)
        w += x, x = 0;
    if (y < 0)
        h += y, y = 0;
    if (x + w > vw->width)
        w = vw->width - x;
    if (y + h > vw->height)
        h = vw->height - y;
    if (w <= 0 || h <= 0 || !pixmap->devPrivate.ptr || pixmap->drawable.bitsPerPixel != 32)
        return;
    base = pixmap->devPrivate.ptr;
    for (row = y; row < y + h; row++) {
        const uint8_t *from = base + (long) (oy + row) * pixmap->devKind + (ox + x) * 4;

        if (oy + row < 0 || oy + row >= pixmap->drawable.height || ox + x < 0 ||
            ox + x + w > pixmap->drawable.width)
            continue;
        memcpy(vw->pixels + (long) row * vw->width + x, from, (size_t) w * 4);
    }
}

static void
present(struct vexa_window *vw, int x, int y, int w, int h)
{
    struct desktop_message m;

    memset(&m, 0, sizeof(m));
    m.type = DESKTOP_PRESENT;
    m.window = vw->id;
    m.a = x;
    m.b = y;
    m.c = w;
    m.d = h;
    send_message(&m);
}

/* A top-level window was mapped: give it a desktop window. */
static void
add_window(WindowPtr window)
{
    struct vexa_window *vw = calloc(1, sizeof(*vw));
    char path[64];
    ScreenPtr screen = window->drawable.pScreen;

    if (!vw)
        return;
    vw->window = window;
    vw->width = window->drawable.width;
    vw->height = window->drawable.height;
    vw->x = window->drawable.x;
    vw->y = window->drawable.y;
    vw->popup = window->overrideRedirect;
    vw->pixels = make_buffer(vw->width, vw->height, &vw->size, path, sizeof(path));
    if (!vw->pixels) {
        free(vw);
        return;
    }
    window_name(window, vw->title, sizeof(vw->title));
    vw->id = create_window(path, vw->title, vw->width, vw->height,
                           vw->popup ? DESKTOP_POPUP : DESKTOP_RESIZABLE);
    if (!vw->id) {
        munmap(vw->pixels, vw->size);
        free(vw);
        return;
    }
    if (vw->popup) {
        struct desktop_message m;

        memset(&m, 0, sizeof(m));
        m.type = DESKTOP_MOVE;
        m.window = vw->id;
        m.a = vw->x;
        m.b = vw->y;
        send_message(&m);
    }
    vw->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, screen, NULL);
    if (vw->damage)
        DamageRegister(&window->drawable, vw->damage);
    copy_window(vw, 0, 0, vw->width, vw->height);
    present(vw, 0, 0, vw->width, vw->height);
    vw->next = windows;
    windows = vw;
}

static void
remove_window(struct vexa_window *vw)
{
    struct vexa_window **link = &windows;
    struct desktop_message m;

    while (*link != vw)
        link = &(*link)->next;
    *link = vw->next;
    if (vw->damage) {
        DamageUnregister(vw->damage);
        DamageDestroy(vw->damage);
    }
    memset(&m, 0, sizeof(m));
    m.type = DESKTOP_DESTROY;
    m.window = vw->id;
    send_message(&m);
    munmap(vw->pixels, vw->size);
    free(vw);
}

/* The X window changed size: a new buffer of that size. */
static void
resize_buffer(struct vexa_window *vw)
{
    int w = vw->window->drawable.width, h = vw->window->drawable.height;
    char path[64];
    size_t size;
    void *memory = make_buffer(w, h, &size, path, sizeof(path));
    struct desktop_message m;

    if (!memory)
        return;
    memset(&m, 0, sizeof(m));
    m.type = DESKTOP_BUFFER;
    m.window = vw->id;
    m.a = w;
    m.b = h;
    strncpy(m.text, path, sizeof(m.text) - 1);
    if (send_message(&m) < 0 || !wait_reply(DESKTOP_RESIZED, vw->id, &m) || !m.a) {
        unlink(path);
        munmap(memory, size);
        return;
    }
    unlink(path);
    munmap(vw->pixels, vw->size);
    vw->pixels = memory;
    vw->size = size;
    vw->width = w;
    vw->height = h;
    copy_window(vw, 0, 0, w, h);
    present(vw, 0, 0, w, h);
}

/* After each round of requests: pass on what changed. */
static void
update_windows(void)
{
    struct vexa_window *vw;

    for (vw = windows; vw; vw = vw->next) {
        WindowPtr window = vw->window;
        char name[64];

        if (window->drawable.width != vw->width || window->drawable.height != vw->height) {
            resize_buffer(vw);
            if (vw->damage)
                DamageEmpty(vw->damage);
        }
        if (window->drawable.x != vw->x || window->drawable.y != vw->y) {
            /* The program moved it (a menu, say): the desktop follows. */
            struct desktop_message m;

            vw->x = window->drawable.x;
            vw->y = window->drawable.y;
            memset(&m, 0, sizeof(m));
            m.type = DESKTOP_MOVE;
            m.window = vw->id;
            m.a = vw->x;
            m.b = vw->y;
            send_message(&m);
        }
        if (vw->damage) {
            RegionPtr region = DamageRegion(vw->damage);

            if (RegionNotEmpty(region)) {
                /* Damage on a window is kept in the window's coordinates. */
                BoxPtr box = RegionExtents(region);
                int x = box->x1, y = box->y1;
                int w = box->x2 - box->x1, h = box->y2 - box->y1;

                copy_window(vw, x, y, w, h);
                present(vw, x, y, w, h);
                DamageEmpty(vw->damage);
            }
        }
        if (!vw->popup) {
            window_name(window, name, sizeof(name));
            if (strcmp(name, vw->title)) {
                struct desktop_message m;

                strcpy(vw->title, name);
                memset(&m, 0, sizeof(m));
                m.type = DESKTOP_TITLE;
                m.window = vw->id;
                strncpy(m.text, name, sizeof(m.text) - 1);
                send_message(&m);
            }
        }
    }
}

/* Moves or resizes an X window as the desktop asks (x, y: its content). */
static void
configure_window(WindowPtr window, Mask mask, int a, int b)
{
    XID values[2];
    int border = window->borderWidth;

    if (mask & CWX) {
        values[0] = a - border;
        values[1] = b - border;
    }
    else {
        values[0] = a;
        values[1] = b;
    }
    ConfigureWindow(window, mask, values, serverClient);
}

static void
raise_window(WindowPtr window)
{
    XID above = Above;

    ConfigureWindow(window, CWStackMode, &above, serverClient);
}

/* The desktop's close button: WM_DELETE_WINDOW if the program understands
 * it, otherwise the program is disconnected (like xkill). */
static void
close_window(WindowPtr window)
{
    static Atom wm_protocols, wm_delete_window;
    PropertyPtr property = NULL;
    Bool polite = FALSE;
    uint32_t i;

    if (!wm_protocols) {
        wm_protocols = MakeAtom("WM_PROTOCOLS", 12, TRUE);
        wm_delete_window = MakeAtom("WM_DELETE_WINDOW", 16, TRUE);
    }
    if (dixLookupProperty(&property, window, wm_protocols, serverClient,
                          DixReadAccess) == Success && property->format == 32) {
        for (i = 0; i < property->size; i++)
            if (((CARD32 *) property->data)[i] == wm_delete_window)
                polite = TRUE;
    }
    if (polite) {
        xEvent event;

        memset(&event, 0, sizeof(event));
        event.u.u.type = ClientMessage;
        event.u.u.detail = 32;
        event.u.clientMessage.window = window->drawable.id;
        event.u.clientMessage.u.l.type = wm_protocols;
        event.u.clientMessage.u.l.longs0 = wm_delete_window;
        event.u.clientMessage.u.l.longs1 = CurrentTime;
        DeliverEventsToWindow(inputInfo.pointer, window, &event, 1, NoEventMask, NullGrab);
    }
    else if (wClient(window) != serverClient) {
        CloseDownClient(wClient(window));
    }
}

/* ---- Input from the desktop ---- */

static void
pointer_event(const struct desktop_message *m)
{
    unsigned long buttons = 0;
    int x = m->a, y = m->b;

    if (!vexa_pointer)
        return;
    if (rootless) {
        struct vexa_window *vw = find_by_id(m->window);

        if (!vw)
            return;
        x += vw->window->drawable.x;
        y += vw->window->drawable.y;
    }
    /* Desktop: bit 0 left, 1 right, 2 middle. X: 1 left, 2 middle, 3 right. */
    if (m->c & 1)
        buttons |= KD_BUTTON_1;
    if (m->c & 4)
        buttons |= KD_BUTTON_2;
    if (m->c & 2)
        buttons |= KD_BUTTON_3;
    if (m->d) {
        /* The wheel: buttons 4 (up) and 5 (down), pressed and let go. */
        unsigned long wheel = m->d > 0 ? KD_BUTTON_4 : KD_BUTTON_5;
        int clicks = m->d > 0 ? m->d : -m->d;

        while (clicks--) {
            KdEnqueuePointerEvent(vexa_pointer, buttons | wheel | KD_POINTER_DESKTOP, x, y, 0);
            KdEnqueuePointerEvent(vexa_pointer, buttons | KD_POINTER_DESKTOP, x, y, 0);
        }
        return;
    }
    KdEnqueuePointerEvent(vexa_pointer, buttons | KD_POINTER_DESKTOP, x, y, 0);
}

static void
handle_message(const struct desktop_message *m)
{
    struct vexa_window *vw = rootless ? find_by_id(m->window) : NULL;

    switch (m->type) {
    case DESKTOP_KEY:
        /* X repeats held keys itself: the desktop's repeats (2) aren't needed. */
        if (vexa_keyboard && m->b != 2 && m->a > 0 && m->a < 248)
            KdEnqueueKeyboardEvent(vexa_keyboard, (unsigned char) m->a, m->b == 0);
        break;
    case DESKTOP_POINTER:
        pointer_event(m);
        break;
    case DESKTOP_CLOSE:
        if (!rootless) {
            ErrorF("Xvexa: the window was closed\n");
            GiveUp(0);
        }
        else if (vw) {
            close_window(vw->window);
        }
        break;
    case DESKTOP_FOCUS:
        /* The desktop raised it and gave it the keyboard: so does X. */
        if (vw && m->a && !vw->popup) {
            raise_window(vw->window);
            SetInputFocus(serverClient, inputInfo.keyboard, vw->window->drawable.id,
                          RevertToPointerRoot, CurrentTime, FALSE);
        }
        break;
    case DESKTOP_MOVED:
        if (vw && !vw->popup && (m->a != vw->x || m->b != vw->y)) {
            vw->x = m->a;
            vw->y = m->b;
            configure_window(vw->window, CWX | CWY, m->a, m->b);
        }
        break;
    case DESKTOP_CONFIGURE:
        if (vw && m->a > 0 && m->b > 0)
            configure_window(vw->window, CWWidth | CWHeight, m->a, m->b);
        break;
    default:
        break;
    }
}

static void
desktop_notify(int fd, int ready, void *data)
{
    struct desktop_message m;
    ssize_t n;

    while (pending_count) {
        m = pending[0];
        memmove(pending, pending + 1, (size_t) --pending_count * sizeof(pending[0]));
        handle_message(&m);
    }
    for (;;) {
        n = recv(desktop, &m, sizeof(m), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        if (n != sizeof(m)) {
            ErrorF("Xvexa: the desktop went away\n");
            RemoveNotifyFd(desktop);
            GiveUp(0);
            return;
        }
        handle_message(&m);
    }
}

/* ---- The screen ---- */

static Bool
vexaCardInit(KdCardInfo * card)
{
    card->driver = NULL;
    return TRUE;
}

static Bool
vexaScreenInitialize(KdScreenInfo * screen)
{
    if (rootless) {
        /* X's screen is the real one; the frame buffer is only for the root
         * window, which nobody sees. */
        if (!pixels) {
            ask_screen_size();
            pixels_size = (size_t) width * height * 4;
            pixels = calloc(1, pixels_size);
            if (!pixels)
                return FALSE;
        }
    }
    else if (screen->width && screen->height && !pixels) {
        width = screen->width;
        height = screen->height;
    }
    screen->width = width;
    screen->height = height;
    screen->rate = 60;
    screen->fb.depth = 24;
    screen->fb.bitsPerPixel = 32;
    screen->fb.visuals = (1 << TrueColor);
    screen->fb.redMask = 0xff0000;
    screen->fb.greenMask = 0x00ff00;
    screen->fb.blueMask = 0x0000ff;
    screen->driver = calloc(1, sizeof(VexaScreenPrivate));
    if (!screen->driver)
        return FALSE;

    /* When the server resets (its last client left), the window stays. */
    if (!pixels)
        open_window();
    screen->fb.frameBuffer = pixels;
    screen->fb.byteStride = width * 4;
    screen->fb.pixelStride = width;
    return TRUE;
}

static Bool
vexaInitScreen(ScreenPtr pScreen)
{
    pScreen->CreateColormap = fbInitializeColormap;
    return TRUE;
}

static VexaScreenPrivate *
screen_private(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    return pScreenPriv->screen->driver;
}

/* After each round of requests: show what X drew. */
static void
vexaBlockHandler(ScreenPtr pScreen, void *timeout)
{
    VexaScreenPrivate *priv = screen_private(pScreen);
    RegionPtr region;

    pScreen->BlockHandler = priv->block_handler;
    (*pScreen->BlockHandler) (pScreen, timeout);
    priv->block_handler = pScreen->BlockHandler;
    pScreen->BlockHandler = vexaBlockHandler;

    if (rootless) {
        update_windows();
        /* Messages that came while waiting for an answer: now, and without
         * sleeping first (they may be input). */
        if (pending_count) {
            desktop_notify(desktop, X_NOTIFY_READ, NULL);
            AdjustWaitForDelay(timeout, 0);
        }
        return;
    }
    if (!priv->damage)
        return;
    region = DamageRegion(priv->damage);
    if (RegionNotEmpty(region)) {
        BoxPtr box = RegionExtents(region);
        struct desktop_message m;

        memset(&m, 0, sizeof(m));
        m.type = DESKTOP_PRESENT;
        m.window = window_id;
        m.a = box->x1;
        m.b = box->y1;
        m.c = box->x2 - box->x1;
        m.d = box->y2 - box->y1;
        send_message(&m);
        DamageEmpty(priv->damage);
    }
}

static Bool
vexaRealizeWindow(WindowPtr window)
{
    ScreenPtr pScreen = window->drawable.pScreen;
    VexaScreenPrivate *priv = screen_private(pScreen);
    Bool result;

    pScreen->RealizeWindow = priv->realize_window;
    result = (*pScreen->RealizeWindow) (window);
    priv->realize_window = pScreen->RealizeWindow;
    pScreen->RealizeWindow = vexaRealizeWindow;

    if (!window->parent) {
        /* The root: every window on it is drawn off the screen, into a
         * pixmap of its own. */
        compRedirectSubwindows(serverClient, window, CompositeRedirectManual);
    }
    else if (window->parent == pScreen->root && window->drawable.class == InputOutput &&
             !find_by_window(window)) {
        add_window(window);
    }
    return result;
}

static Bool
vexaUnrealizeWindow(WindowPtr window)
{
    ScreenPtr pScreen = window->drawable.pScreen;
    VexaScreenPrivate *priv = screen_private(pScreen);
    struct vexa_window *vw = find_by_window(window);
    Bool result;

    if (vw)
        remove_window(vw);
    pScreen->UnrealizeWindow = priv->unrealize_window;
    result = (*pScreen->UnrealizeWindow) (window);
    priv->unrealize_window = pScreen->UnrealizeWindow;
    pScreen->UnrealizeWindow = vexaUnrealizeWindow;
    return result;
}

static Bool
vexaFinishInitScreen(ScreenPtr pScreen)
{
    VexaScreenPrivate *priv = screen_private(pScreen);

    vexa_screen = pScreen;
    priv->block_handler = pScreen->BlockHandler;
    pScreen->BlockHandler = vexaBlockHandler;
    if (rootless) {
        priv->realize_window = pScreen->RealizeWindow;
        pScreen->RealizeWindow = vexaRealizeWindow;
        priv->unrealize_window = pScreen->UnrealizeWindow;
        pScreen->UnrealizeWindow = vexaUnrealizeWindow;
    }
    return TRUE;
}

static Bool
vexaCreateResources(ScreenPtr pScreen)
{
    VexaScreenPrivate *priv = screen_private(pScreen);
    PixmapPtr pixmap = (*pScreen->GetScreenPixmap) (pScreen);

    if (rootless)
        return TRUE;
    priv->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, pScreen);
    DamageRegister(&pixmap->drawable, priv->damage);
    return TRUE;
}

static void
vexaCloseScreen(ScreenPtr pScreen)
{
    VexaScreenPrivate *priv = screen_private(pScreen);

    while (windows)
        remove_window(windows);
    if (priv->damage) {
        DamageDestroy(priv->damage);
        priv->damage = NULL;
    }
}

static void
vexaScreenFini(KdScreenInfo * screen)
{
}

static void
vexaCardFini(KdCardInfo * card)
{
}

static void
vexaGetColors(ScreenPtr pScreen, int n, xColorItem * pdefs)
{
    while (n--) {
        pdefs->red = pdefs->green = pdefs->blue = 0;
        pdefs++;
    }
}

static void
vexaPutColors(ScreenPtr pScreen, int n, xColorItem * pdefs)
{
}

static KdCardFuncs vexaFuncs = {
    vexaCardInit,
    vexaScreenInitialize,
    vexaInitScreen,
    vexaFinishInitScreen,
    vexaCreateResources,
    vexaScreenFini,
    vexaCardFini,
    0,                          /* initCursor: X draws the cursor itself */
    0, 0, 0, 0,                 /* no acceleration */
    vexaGetColors,
    vexaPutColors,
    vexaCloseScreen,
};

/* ---- Input devices ---- */

static Status
vexaPointerInit(KdPointerInfo * pi)
{
    pi->nAxes = 3;
    pi->nButtons = 7;
    free(pi->name);
    pi->name = strdup("Vexa desktop pointer");
    vexa_pointer = pi;
    return Success;
}

static Status
vexaPointerEnable(KdPointerInfo * pi)
{
    SetNotifyFd(desktop, desktop_notify, X_NOTIFY_READ, NULL);
    return Success;
}

static void
vexaPointerDisable(KdPointerInfo * pi)
{
    RemoveNotifyFd(desktop);
}

static void
vexaPointerFini(KdPointerInfo * pi)
{
    vexa_pointer = NULL;
}

static KdPointerDriver vexaPointerDriver = {
    "vexa",
    vexaPointerInit,
    vexaPointerEnable,
    vexaPointerDisable,
    vexaPointerFini,
    NULL,
};

static Status
vexaKeyboardInit(KdKeyboardInfo * ki)
{
    /* Linux key codes: X adds 8 (KD_MIN_KEYCODE), which is what the evdev
     * XKB rules expect. */
    ki->minScanCode = 0;
    ki->maxScanCode = 247;
    free(ki->name);
    ki->name = strdup("Vexa desktop keyboard");
    vexa_keyboard = ki;
    return Success;
}

static Status
vexaKeyboardEnable(KdKeyboardInfo * ki)
{
    return Success;
}

static void
vexaKeyboardLeds(KdKeyboardInfo * ki, int leds)
{
}

static void
vexaKeyboardBell(KdKeyboardInfo * ki, int volume, int frequency, int duration)
{
}

static void
vexaKeyboardDisable(KdKeyboardInfo * ki)
{
}

static void
vexaKeyboardFini(KdKeyboardInfo * ki)
{
    vexa_keyboard = NULL;
}

static KdKeyboardDriver vexaKeyboardDriver = {
    "vexa",
    vexaKeyboardInit,
    vexaKeyboardEnable,
    vexaKeyboardLeds,
    vexaKeyboardBell,
    vexaKeyboardDisable,
    vexaKeyboardFini,
    NULL,
};

/* ---- What every X server provides ---- */

int
main(int argc, char *argv[], char *envp[])
{
    return dix_main(argc, argv, envp);
}

void
InitOutput(ScreenInfo * pScreenInfo, int argc, char **argv)
{
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    KdKeyboardInfo *ki;
    KdPointerInfo *pi;

    KdAddKeyboardDriver(&vexaKeyboardDriver);
    KdAddPointerDriver(&vexaPointerDriver);

    ki = KdNewKeyboard();
    if (!ki)
        FatalError("Xvexa: couldn't make the keyboard\n");
    ki->driver = &vexaKeyboardDriver;
    KdAddKeyboard(ki);

    pi = KdNewPointer();
    if (!pi)
        FatalError("Xvexa: couldn't make the pointer\n");
    pi->driver = &vexaPointerDriver;
    KdAddPointer(pi);

    KdInitInput();
}

void
CloseInput(void)
{
    KdCloseInput();
}

#if INPUTTHREAD
void
ddxInputThreadInit(void)
{
}
#endif

#ifdef DDXBEFORERESET
void
ddxBeforeReset(void)
{
}
#endif

void
ddxUseMsg(void)
{
    KdUseMsg();
    ErrorF("\nXvexa options:\n");
    ErrorF("-title <title>       the window's title (windowed)\n");
    ErrorF("-rootless            every X window is a desktop window\n");
    ErrorF("\n");
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
    if (!strcmp(argv[i], "-title")) {
        if (i + 1 < argc) {
            strncpy(title, argv[i + 1], sizeof(title) - 1);
            return 2;
        }
        UseMsg();
        exit(1);
    }
    if (!strcmp(argv[i], "-rootless")) {
        rootless = TRUE;
        return 1;
    }
    return KdProcessArgument(argc, argv, i);
}

void
OsVendorInit(void)
{
    if (serverGeneration == 1 && !KdCardInfoLast()) {
        KdCardInfo *card;
        KdScreenInfo *screen;

        KdCardInfoAdd(&vexaFuncs, 0);
        card = KdCardInfoLast();
        screen = KdScreenInfoAdd(card);
        KdParseScreen(screen, "1024x640");
    }
}

void
InitCard(char *name)
{
    KdCardInfoAdd(&vexaFuncs, 0);
}
