/*
 * Xvexa: an X server whose screen is a window on the Vexa desktop.
 *
 * A kdrive server, like Xephyr, but its "host" is Vexa's compositor: the X
 * screen's frame buffer is the window's pixel buffer (a file in /run/shm that
 * the desktop maps too), what X draws is presented as damage, and the keys
 * and pointer events the desktop sends to the window become X input. Key
 * codes are Linux's, so with the "evdev" XKB rules X's key code is the key
 * code plus 8, as on Linux.
 *
 * It talks to the desktop with the protocol in libvexa/include/vexa/desktop.h
 * (the message layout is repeated below), over the local socket
 * /run/desktop. This file is part of Vexa (MIT license); it is built inside
 * xorg-server's source tree (hw/kdrive/vexa) by tools/build-x11.sh.
 *
 *     Xvexa :0 -screen 1024x640 &
 *     DISPLAY=:0 xterm
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "kdrive.h"
#include "damage.h"
#include "inputstr.h"
#include "scrnintstr.h"
#include "xkbsrv.h"

/* ---- The desktop protocol (libvexa/include/vexa/desktop.h) ---- */

#define DESKTOP_SOCKET "/run/desktop"

enum desktop_message_type {
    DESKTOP_CREATE = 1,
    DESKTOP_PRESENT = 2,
    DESKTOP_TITLE = 3,
    DESKTOP_DESTROY = 4,
    DESKTOP_CREATED = 16,
    DESKTOP_KEY = 17,
    DESKTOP_POINTER = 18,
    DESKTOP_CLOSE = 19,
    DESKTOP_FOCUS = 20,
};

struct desktop_message {
    uint32_t type;
    uint32_t window;
    int32_t a, b, c, d;
    char text[104];
};

/* ---- State ---- */

typedef struct {
    DamagePtr damage;
    ScreenBlockHandlerProcPtr block_handler;
} VexaScreenPrivate;

static int desktop = -1;       /* The connection. */
static uint32_t window_id;
static void *pixels;           /* The window's buffer: the X frame buffer. */
static size_t pixels_size;
static int width = 1024, height = 640;
static char title[64] = "X";
static KdKeyboardInfo *vexa_keyboard;
static KdPointerInfo *vexa_pointer;
static int pointer_x, pointer_y;
static unsigned long pointer_buttons; /* KD_BUTTON_* */

static int
send_message(struct desktop_message *m)
{
    return send(desktop, m, sizeof(*m), MSG_NOSIGNAL) == sizeof(*m) ? 0 : -1;
}

/* Connects, makes the buffer and opens the window. */
static void
open_window(void)
{
    char path[64];
    struct sockaddr_un address;
    struct desktop_message m;
    int fd;

    desktop = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (desktop < 0)
        FatalError("Xvexa: can't make a socket: %s\n", strerror(errno));
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, DESKTOP_SOCKET);
    if (connect(desktop, (struct sockaddr *) &address, sizeof(address)) < 0)
        FatalError("Xvexa: no Vexa desktop at %s (start `desktop` first): %s\n",
                   DESKTOP_SOCKET, strerror(errno));

    snprintf(path, sizeof(path), "/run/shm/xvexa-%d", (int) getpid());
    pixels_size = ((size_t) width * height * 4 + 4095) & ~(size_t) 4095;
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, pixels_size) < 0)
        FatalError("Xvexa: can't make %s: %s\n", path, strerror(errno));
    pixels = mmap(NULL, pixels_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        FatalError("Xvexa: can't map %s: %s\n", path, strerror(errno));
    close(fd);

    memset(&m, 0, sizeof(m));
    m.type = DESKTOP_CREATE;
    m.a = width;
    m.b = height;
    strcpy(m.text, path);
    strncpy(m.text + strlen(path) + 1, title, sizeof(m.text) - strlen(path) - 2);
    if (send_message(&m) < 0)
        FatalError("Xvexa: the desktop went away\n");
    do {
        if (recv(desktop, &m, sizeof(m), 0) != sizeof(m))
            FatalError("Xvexa: the desktop went away\n");
    } while (m.type != DESKTOP_CREATED);
    unlink(path);
    if (!m.window)
        FatalError("Xvexa: the desktop refused the window\n");
    window_id = m.window;
    fcntl(desktop, F_SETFL, O_NONBLOCK);
}

/* ---- Input from the desktop ---- */

static void
pointer_event(const struct desktop_message *m)
{
    unsigned long buttons = 0;

    if (!vexa_pointer)
        return;
    /* Desktop: bit 0 left, 1 right, 2 middle. X: 1 left, 2 middle, 3 right. */
    if (m->c & 1)
        buttons |= KD_BUTTON_1;
    if (m->c & 4)
        buttons |= KD_BUTTON_2;
    if (m->c & 2)
        buttons |= KD_BUTTON_3;
    pointer_x = m->a;
    pointer_y = m->b;
    pointer_buttons = buttons;
    if (m->d) {
        /* The wheel: buttons 4 (up) and 5 (down), pressed and let go. */
        unsigned long wheel = m->d > 0 ? KD_BUTTON_4 : KD_BUTTON_5;
        int clicks = m->d > 0 ? m->d : -m->d;

        while (clicks--) {
            KdEnqueuePointerEvent(vexa_pointer, buttons | wheel | KD_POINTER_DESKTOP,
                                  pointer_x, pointer_y, 0);
            KdEnqueuePointerEvent(vexa_pointer, buttons | KD_POINTER_DESKTOP,
                                  pointer_x, pointer_y, 0);
        }
        return;
    }
    KdEnqueuePointerEvent(vexa_pointer, buttons | KD_POINTER_DESKTOP, pointer_x, pointer_y, 0);
}

static void
desktop_notify(int fd, int ready, void *data)
{
    struct desktop_message m;
    ssize_t n;

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
        switch (m.type) {
        case DESKTOP_KEY:
            /* X repeats held keys itself: the desktop's repeats (2) aren't needed. */
            if (vexa_keyboard && m.b != 2 && m.a > 0 && m.a < 248)
                KdEnqueueKeyboardEvent(vexa_keyboard, (unsigned char) m.a, m.b == 0);
            break;
        case DESKTOP_POINTER:
            pointer_event(&m);
            break;
        case DESKTOP_CLOSE:
            ErrorF("Xvexa: the window was closed\n");
            GiveUp(0);
            return;
        default:
            break;
        }
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
    if (screen->width && screen->height && !pixels) {
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

/* After each round of requests: show what X drew. */
static void
vexaBlockHandler(ScreenPtr pScreen, void *timeout)
{
    KdScreenPriv(pScreen);
    VexaScreenPrivate *priv = pScreenPriv->screen->driver;
    RegionPtr region;

    pScreen->BlockHandler = priv->block_handler;
    (*pScreen->BlockHandler) (pScreen, timeout);
    priv->block_handler = pScreen->BlockHandler;
    pScreen->BlockHandler = vexaBlockHandler;

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
vexaFinishInitScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    VexaScreenPrivate *priv = pScreenPriv->screen->driver;

    priv->block_handler = pScreen->BlockHandler;
    pScreen->BlockHandler = vexaBlockHandler;
    return TRUE;
}

static Bool
vexaCreateResources(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    VexaScreenPrivate *priv = pScreenPriv->screen->driver;
    PixmapPtr pixmap = (*pScreen->GetScreenPixmap) (pScreen);

    priv->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, pScreen);
    DamageRegister(&pixmap->drawable, priv->damage);
    return TRUE;
}

static void
vexaCloseScreen(ScreenPtr pScreen)
{
    KdScreenPriv(pScreen);
    VexaScreenPrivate *priv = pScreenPriv->screen->driver;

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
    ErrorF("-title <title>       the window's title\n");
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
