/*
 * SDL's video driver for Vexa: each SDL window is a window on the Vexa
 * desktop (<vexa/gui.h>). Drawing is in software: the window's surface is
 * SDL's framebuffer (XRGB8888), and updating it presents the rectangles.
 * Events come from vx_gui_wait: keys (Linux key codes, so SDL's Linux
 * scancode table), text, the pointer, closing, focus, resizing and dropped
 * files. The clipboard is Vexa's.
 *
 * Part of Vexa's SDK, under SDL's zlib license.
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_VEXA

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_timer.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_dropevents_c.h"
#include "../../events/scancodes_linux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vexa/abi.h>
#include <vexa/desktop.h>
#include <vexa/gui.h>

#define VEXA_DRIVER_NAME "vexa"

/* The screen's size, for SDL's display mode (the desktop's usual one). */
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 800

typedef struct
{
    struct vx_window *window; /* (Made once SDL has given it its title.) */
    unsigned flags;           /* VX_WINDOW_* */
    int buttons;              /* The pointer's buttons, last we heard. */
} VEXA_WindowData;

typedef struct
{
    int shape; /* VX_CURSOR_* */
} VEXA_CursorData;

static SDL_Cursor *current_cursor;
static SDL_bool cursor_shown = SDL_TRUE;

/* ---- Windows ----
 * SDL names a window after making it, so the desktop's window is made at
 * the first thing that needs it: the title, drawing, or events. */

static struct vx_window *vexa_window(SDL_Window *window)
{
    VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
    if (!data->window) {
        data->window = vx_window_create_flags(window->title ? window->title : "SDL", window->w,
                                              window->h, data->flags);
        if (!data->window) {
            SDL_SetError("Vexa: couldn't open a window on the desktop");
        }
    }
    return data->window;
}

static int VEXA_CreateWindow(_THIS, SDL_Window *window)
{
    VEXA_WindowData *data;
    (void)_this;
    if (access(DESKTOP_SOCKET, F_OK) != 0) {
        return SDL_SetError("Vexa: no desktop to open a window on");
    }
    data = (VEXA_WindowData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->flags = (window->flags & SDL_WINDOW_RESIZABLE) ? VX_WINDOW_RESIZABLE : 0;
    if (window->flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) {
        /* No full screen: a window the size asked for (or the screen's). */
        if ((window->flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP) {
            window->w = SCREEN_WIDTH;
            window->h = SCREEN_HEIGHT - 60;
        }
    }
    window->driverdata = data;
    window->flags &= ~SDL_WINDOW_HIDDEN;
    window->flags |= SDL_WINDOW_SHOWN;
    /* A new window has the keyboard (the desktop focuses it). */
    SDL_SetKeyboardFocus(window);
    return 0;
}

static void VEXA_DestroyWindow(_THIS, SDL_Window *window)
{
    VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
    if (data) {
        if (data->window) {
            vx_window_destroy(data->window);
        }
        SDL_free(data);
        window->driverdata = NULL;
    }
}

static void VEXA_SetWindowTitle(_THIS, SDL_Window *window)
{
    VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
    (void)_this;
    if (!data->window) {
        vexa_window(window); /* The first title: the window can be made now. */
    } else {
        vx_window_set_title(data->window, window->title ? window->title : "");
    }
}

/* (The size changes when the framebuffer is made again: see below.) */
static void VEXA_SetWindowSize(_THIS, SDL_Window *window)
{
    (void)_this;
    (void)window;
}

static SDL_Window *find_window(_THIS, int id)
{
    SDL_Window *window;
    for (window = _this->windows; window; window = window->next) {
        VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
        if (data && data->window && data->window->id == id) {
            return window;
        }
    }
    return NULL;
}

/* ---- The framebuffer: the window's own pixels ---- */

static int VEXA_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels,
                                        int *pitch)
{
    VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
    struct vx_surface *s;
    (void)_this;
    if (!vexa_window(window)) {
        return -1;
    }
    s = &data->window->surface;
    /* Resizing makes a new surface; SDL has let go of the old one by now. */
    if (s->width != window->w || s->height != window->h) {
        if (vx_window_resize(data->window, window->w, window->h) < 0) {
            return SDL_SetError("Vexa: couldn't resize the window to %dx%d", window->w, window->h);
        }
    }
    *format = SDL_PIXELFORMAT_RGB888;
    *pixels = s->pixels;
    *pitch = s->stride * 4;
    return 0;
}

static int VEXA_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects,
                                        int numrects)
{
    VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
    struct vx_surface *s = &data->window->surface;
    int i, x0 = s->width, y0 = s->height, x1 = 0, y1 = 0;
    (void)_this;
    /* One rectangle around them all: one message to the desktop. */
    for (i = 0; i < numrects; i++) {
        x0 = SDL_min(x0, rects[i].x);
        y0 = SDL_min(y0, rects[i].y);
        x1 = SDL_max(x1, rects[i].x + rects[i].w);
        y1 = SDL_max(y1, rects[i].y + rects[i].h);
    }
    x0 = SDL_max(x0, 0);
    y0 = SDL_max(y0, 0);
    x1 = SDL_min(x1, s->width);
    y1 = SDL_min(y1, s->height);
    if (x1 > x0 && y1 > y0) {
        vx_window_present(data->window, x0, y0, x1 - x0, y1 - y0);
    }
    return 0;
}

static void VEXA_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    (void)_this;
    (void)window; /* The pixels are the window's. */
}

/* ---- Events ---- */

static void key_event(SDL_Window *window, const struct vx_gui_event *e)
{
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    if (e->key >= 0 && e->key < (int)SDL_arraysize(linux_scancode_table)) {
        scancode = linux_scancode_table[e->key];
    }
    if (scancode != SDL_SCANCODE_UNKNOWN) {
        SDL_SendKeyboardKey(e->value ? SDL_PRESSED : SDL_RELEASED, scancode);
    }
    /* Text: what the key types, unless Ctrl or Alt is held. */
    if (e->value && e->character >= ' ' && e->character != 0x7f &&
        !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT))) {
        char text[5] = { 0 };
        vx_utf8_encode((uint32_t)e->character, text);
        SDL_SendKeyboardText(text);
    }
    (void)window;
}

static void pointer_event(SDL_Window *window, const struct vx_gui_event *e)
{
    static const Uint8 sdl_buttons[3] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE };
    VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
    int i, inside = e->x >= 0 && e->y >= 0 && e->x < window->w && e->y < window->h;

    if (inside && SDL_GetMouseFocus() != window) {
        SDL_SetMouseFocus(window);
    }
    SDL_SendMouseMotion(window, 0, 0, e->x, e->y);
    for (i = 0; i < 3; i++) {
        int bit = 1 << i;
        if ((e->buttons & bit) != (data->buttons & bit)) {
            SDL_SendMouseButton(window, 0, (e->buttons & bit) ? SDL_PRESSED : SDL_RELEASED,
                                sdl_buttons[i]);
        }
    }
    data->buttons = e->buttons;
    if (e->wheel) {
        SDL_SendMouseWheel(window, 0, 0.0f, (float)e->wheel, SDL_MOUSEWHEEL_NORMAL);
    }
    if (!inside && !e->buttons && SDL_GetMouseFocus() == window) {
        SDL_SetMouseFocus(NULL);
    }
}

static void drop_event(SDL_Window *window, const struct vx_gui_event *e)
{
    char *paths = vx_drop_paths(e);
    char *line, *next;
    if (!paths) {
        return;
    }
    for (line = paths; line && *line; line = next) {
        next = SDL_strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (*line) {
            SDL_SendDropFile(window, line);
        }
    }
    SDL_SendDropComplete(window);
    SDL_free(paths);
}

static void VEXA_PumpEvents(_THIS)
{
    struct vx_gui_event e;
    SDL_Window *window;
    int got;
    if (!_this->windows) {
        return;
    }
    for (window = _this->windows; window; window = window->next) {
        vexa_window(window); /* (Any not made yet.) */
    }
    while ((got = vx_gui_wait(&e, 0)) > 0) {
        window = find_window(_this, e.window);
        if (!window) {
            continue;
        }
        switch (e.type) {
        case VX_GUI_KEY:
            key_event(window, &e);
            break;
        case VX_GUI_POINTER:
            pointer_event(window, &e);
            break;
        case VX_GUI_CLOSE:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
            break;
        case VX_GUI_FOCUS:
            if (e.value) {
                SDL_SetKeyboardFocus(window);
            } else if (SDL_GetKeyboardFocus() == window) {
                SDL_SetKeyboardFocus(NULL);
            }
            break;
        case VX_GUI_RESIZE:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, e.width, e.height);
            break;
        case VX_GUI_THEME:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_EXPOSED, 0, 0);
            break;
        case VX_GUI_DROP:
            drop_event(window, &e);
            break;
        }
    }
    if (got < 0) { /* The desktop is gone: time to go. */
        SDL_SendQuit();
    }
}

/* ---- The pointer's shape ---- */

static SDL_Cursor *new_cursor(int shape)
{
    SDL_Cursor *cursor = (SDL_Cursor *)SDL_calloc(1, sizeof(*cursor));
    VEXA_CursorData *data = (VEXA_CursorData *)SDL_calloc(1, sizeof(*data));
    if (!cursor || !data) {
        SDL_free(cursor);
        SDL_free(data);
        SDL_OutOfMemory();
        return NULL;
    }
    data->shape = shape;
    cursor->driverdata = data;
    return cursor;
}

static SDL_Cursor *VEXA_CreateSystemCursor(SDL_SystemCursor id)
{
    switch (id) {
    case SDL_SYSTEM_CURSOR_IBEAM:
        return new_cursor(VX_CURSOR_TEXT);
    case SDL_SYSTEM_CURSOR_WAIT:
    case SDL_SYSTEM_CURSOR_WAITARROW:
        return new_cursor(VX_CURSOR_WAIT);
    case SDL_SYSTEM_CURSOR_CROSSHAIR:
        return new_cursor(VX_CURSOR_CROSS);
    case SDL_SYSTEM_CURSOR_SIZEALL:
    case SDL_SYSTEM_CURSOR_SIZENWSE:
    case SDL_SYSTEM_CURSOR_SIZENESW:
    case SDL_SYSTEM_CURSOR_SIZEWE:
    case SDL_SYSTEM_CURSOR_SIZENS:
        return new_cursor(VX_CURSOR_MOVE);
    case SDL_SYSTEM_CURSOR_HAND:
        return new_cursor(VX_CURSOR_HAND);
    default:
        return new_cursor(VX_CURSOR_ARROW);
    }
}

/* (Pictures of their own aren't possible: those are arrows.) */
static SDL_Cursor *VEXA_CreateCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    (void)surface;
    (void)hot_x;
    (void)hot_y;
    return new_cursor(VX_CURSOR_ARROW);
}

static void VEXA_FreeCursor(SDL_Cursor *cursor)
{
    if (cursor == current_cursor) {
        current_cursor = NULL;
    }
    SDL_free(cursor->driverdata);
    SDL_free(cursor);
}

static void apply_cursor(void)
{
    SDL_VideoDevice *video = SDL_GetVideoDevice();
    int shape = VX_CURSOR_ARROW;
    SDL_Window *window;
    if (current_cursor && cursor_shown) {
        shape = ((VEXA_CursorData *)current_cursor->driverdata)->shape;
    }
    for (window = video ? video->windows : NULL; window; window = window->next) {
        VEXA_WindowData *data = (VEXA_WindowData *)window->driverdata;
        if (data && data->window) {
            vx_window_set_cursor(data->window, shape);
        }
    }
}

/* Hiding it isn't possible either: it goes back to the arrow. */
static int VEXA_ShowCursor(SDL_Cursor *cursor)
{
    current_cursor = cursor;
    cursor_shown = cursor != NULL;
    apply_cursor();
    return 0;
}

static void init_mouse(void)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    mouse->CreateCursor = VEXA_CreateCursor;
    mouse->CreateSystemCursor = VEXA_CreateSystemCursor;
    mouse->ShowCursor = VEXA_ShowCursor;
    mouse->FreeCursor = VEXA_FreeCursor;
    SDL_SetDefaultCursor(VEXA_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
}

/* ---- The clipboard ---- */

static int VEXA_SetClipboardText(_THIS, const char *text)
{
    (void)_this;
    vx_clipboard_set(text, SDL_strlen(text));
    return 0;
}

static char *VEXA_GetClipboardText(_THIS)
{
    char *text = vx_clipboard_get();
    char *copy;
    (void)_this;
    copy = SDL_strdup(text ? text : "");
    free(text);
    return copy;
}

static SDL_bool VEXA_HasClipboardText(_THIS)
{
    char *text = vx_clipboard_get();
    SDL_bool has = (text && *text) ? SDL_TRUE : SDL_FALSE;
    (void)_this;
    free(text);
    return has;
}

/* ---- The driver ---- */

static int VEXA_VideoInit(_THIS)
{
    SDL_DisplayMode mode;
    (void)_this;
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = SCREEN_WIDTH;
    mode.h = SCREEN_HEIGHT;
    mode.refresh_rate = 60;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);
    init_mouse();
    return 0;
}

static void VEXA_VideoQuit(_THIS)
{
    (void)_this;
}

static void VEXA_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static int VEXA_DeviceShowMessageBox(_THIS, const SDL_MessageBoxData *data, int *buttonid);

static SDL_VideoDevice *VEXA_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }
    device->VideoInit = VEXA_VideoInit;
    device->VideoQuit = VEXA_VideoQuit;
    device->PumpEvents = VEXA_PumpEvents;
    device->CreateSDLWindow = VEXA_CreateWindow;
    device->DestroyWindow = VEXA_DestroyWindow;
    device->SetWindowTitle = VEXA_SetWindowTitle;
    device->SetWindowSize = VEXA_SetWindowSize;
    device->CreateWindowFramebuffer = VEXA_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = VEXA_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = VEXA_DestroyWindowFramebuffer;
    device->SetClipboardText = VEXA_SetClipboardText;
    device->GetClipboardText = VEXA_GetClipboardText;
    device->HasClipboardText = VEXA_HasClipboardText;
    device->ShowMessageBox = VEXA_DeviceShowMessageBox;
    device->free = VEXA_DeleteDevice;
    return device;
}

/* SDL_ShowMessageBox: a small window of its own, with the message (wrapped)
 * and the buttons; Enter and Escape choose the buttons marked for them. */

#define BOX_WIDTH 460
#define BOX_BUTTON_WIDTH 96
#define BOX_LINES 16

static int wrap_message(const char *message, char lines[BOX_LINES][128], int width)
{
    int count = 0;
    const char *p = message ? message : "";
    while (*p && count < BOX_LINES) {
        char line[128];
        int length = 0, last_space = -1;
        while (p[length] && p[length] != '\n' && length < 127) {
            line[length] = p[length];
            line[length + 1] = '\0';
            if (p[length] == ' ') {
                last_space = length;
            }
            if (vx_text_width(line) > width && last_space > 0) {
                length = last_space;
                break;
            }
            length++;
        }
        SDL_memcpy(lines[count], p, (size_t)length);
        lines[count][length] = '\0';
        count++;
        p += length;
        if (*p == ' ' || *p == '\n') {
            p++;
        }
    }
    return count;
}

static int VEXA_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid)
{
    char lines[BOX_LINES][128];
    int count = wrap_message(data->message, lines, BOX_WIDTH - 40);
    int buttons = data->numbuttons > 0 ? data->numbuttons : 0;
    int height = 24 + count * VX_LINE_HEIGHT + 24 + 30 + 16;
    int hot = -1, held = 0;
    struct vx_window *box;
    struct vx_gui_event e;

    printf("SDL message box: %s: %s\n", data->title ? data->title : "",
           data->message ? data->message : "");
    fflush(stdout);
    *buttonid = -1;
    box = vx_window_create(data->title ? data->title : "", BOX_WIDTH, height);
    if (!box) {
        return SDL_SetError("Vexa: no desktop to show a message on");
    }
    for (;;) {
        struct vx_surface *s = &box->surface;
        int i, x, by = height - 46;
        vx_fill(s, 0, 0, s->width, s->height, VX_COLOR_WINDOW);
        if (data->flags & SDL_MESSAGEBOX_ERROR) {
            vx_fill(s, 0, 0, 6, s->height, 0xd9534f);
        } else if (data->flags & SDL_MESSAGEBOX_WARNING) {
            vx_fill(s, 0, 0, 6, s->height, 0xf0ad4e);
        }
        for (i = 0; i < count; i++) {
            vx_draw_text(s, 22, 20 + i * VX_LINE_HEIGHT, lines[i], VX_COLOR_TEXT, VX_TRANSPARENT);
        }
        /* The buttons, right to left as SDL lists them by default. */
        for (i = 0, x = BOX_WIDTH - 20 - BOX_BUTTON_WIDTH; i < buttons; i++) {
            vx_draw_button(s, x, by, BOX_BUTTON_WIDTH, 30, data->buttons[i].text, hot == i);
            x -= BOX_BUTTON_WIDTH + 10;
        }
        vx_window_present(box, 0, 0, s->width, s->height);
        if (vx_gui_wait(&e, -1) <= 0) {
            break;
        }
        if (e.window != box->id) {
            continue; /* (The program's other windows wait.) */
        }
        if (e.type == VX_GUI_CLOSE) {
            for (i = 0; i < buttons; i++) {
                if (data->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) {
                    *buttonid = data->buttons[i].buttonid;
                }
            }
            break;
        }
        if (e.type == VX_GUI_KEY && e.value && (e.key == VX_KEY_ENTER || e.key == VX_KEY_ESC)) {
            Uint32 flag = e.key == VX_KEY_ENTER ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
                                                : SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
            SDL_bool found = SDL_FALSE;
            for (i = 0; i < buttons; i++) {
                if (data->buttons[i].flags & flag) {
                    *buttonid = data->buttons[i].buttonid;
                    found = SDL_TRUE;
                }
            }
            if (found || buttons <= 1) {
                if (!found && buttons == 1) {
                    *buttonid = data->buttons[0].buttonid;
                }
                break;
            }
        }
        if (e.type == VX_GUI_POINTER) {
            SDL_bool click = (e.buttons & 1) && !(held & 1);
            held = e.buttons;
            hot = -1;
            if (e.y >= by && e.y < by + 30) {
                for (i = 0, x = BOX_WIDTH - 20 - BOX_BUTTON_WIDTH; i < buttons; i++) {
                    if (e.x >= x && e.x < x + BOX_BUTTON_WIDTH) {
                        hot = i;
                    }
                    x -= BOX_BUTTON_WIDTH + 10;
                }
            }
            if (click && hot >= 0) {
                *buttonid = data->buttons[hot].buttonid;
                break;
            }
        }
    }
    vx_window_destroy(box);
    return 0;
}

static int VEXA_DeviceShowMessageBox(_THIS, const SDL_MessageBoxData *data, int *buttonid)
{
    (void)_this;
    return VEXA_ShowMessageBox(data, buttonid);
}

VideoBootStrap VEXA_bootstrap = {
    VEXA_DRIVER_NAME, "Windows on the Vexa desktop",
    VEXA_CreateDevice,
    VEXA_ShowMessageBox
};

#endif /* SDL_VIDEO_DRIVER_VEXA */
