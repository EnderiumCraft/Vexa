//! libvexa's GUI, as far as Vane uses it (libvexa's gui.h), and a small safe layer to draw with.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_long, c_uint, c_void};

pub const WINDOW_RESIZABLE: c_uint = 0x1;
pub const GUI_KEY: c_int = 1;
pub const GUI_POINTER: c_int = 2;
pub const GUI_CLOSE: c_int = 3;
pub const GUI_FOCUS: c_int = 4;
pub const GUI_RESIZE: c_int = 5;
pub const GUI_THEME: c_int = 6;

pub const CURSOR_ARROW: c_int = 0;
pub const CURSOR_TEXT: c_int = 1;
pub const CURSOR_HAND: c_int = 2;
pub const CURSOR_WAIT: c_int = 3;
pub const CURSOR_CROSS: c_int = 4;
pub const CURSOR_MOVE: c_int = 5;

pub const BUTTON_HOT: c_uint = 1;
pub const BUTTON_DISABLED: c_uint = 2;

#[repr(C)]
pub struct Surface {
    pub pixels: *mut u32,
    pub width: c_int,
    pub height: c_int,
    pub stride: c_int,
}

/// The start of libvexa's struct vx_window. The program draws on `surface`; the rest of the
/// struct (the buffers and the desktop's handles) is libvexa's, and this never reads it.
#[repr(C)]
pub struct Window {
    pub id: c_int,
    pub surface: Surface,
}

/// libvexa's struct vx_gui_event, field for field.
#[repr(C)]
pub struct Event {
    pub kind: c_int,
    pub window: c_int,
    pub x: c_int,
    pub y: c_int,
    pub buttons: c_int,
    pub wheel: c_int,
    pub key: c_int,
    pub value: c_int,
    pub character: c_int,
    pub width: c_int,
    pub height: c_int,
    pub text: [c_char; 104],
}

/// libvexa's struct vx_theme.
#[repr(C)]
pub struct Theme {
    pub dark: bool,
    pub window: u32,
    pub view: u32,
    pub text: u32,
    pub dim: u32,
    pub accent: u32,
    pub selected: u32,
    pub button: u32,
    pub button_hot: u32,
    pub line: u32,
    pub sidebar: u32,
    pub stripe: u32,
    pub shadow: u32,
    pub panel: u32,
    pub menu: u32,
    pub title: u32,
    pub title_focused: u32,
    pub title_text: u32,
}

#[repr(C)]
pub struct MenuItem {
    pub label: *const c_char,
    pub keys: *const c_char,
    pub disabled: bool,
}

#[repr(C)]
pub struct Alert {
    pub title: *const c_char,
    pub icon: *const c_char,
    pub message: *const c_char,
    pub detail: *const c_char,
    pub buttons: *const *const c_char,
    pub button_count: c_int,
    pub default_button: c_int,
    pub cancel_button: c_int,
}

extern "C" {
    static vx_theme: Theme;

    fn vx_window_create_flags(
        title: *const c_char,
        width: c_int,
        height: c_int,
        flags: c_uint,
    ) -> *mut Window;
    fn vx_window_center(window: *mut Window);
    fn vx_window_resize(window: *mut Window, width: c_int, height: c_int) -> c_int;
    fn vx_window_present(window: *mut Window, x: c_int, y: c_int, width: c_int, height: c_int);
    fn vx_window_set_title(window: *mut Window, title: *const c_char);
    fn vx_window_set_cursor(window: *mut Window, shape: c_int);
    fn vx_window_destroy(window: *mut Window);
    fn vx_gui_wait(event: *mut Event, timeout_ms: c_long) -> c_int;

    fn vx_fill(s: *mut Surface, x: c_int, y: c_int, w: c_int, h: c_int, color: u32);
    fn vx_fill_rounded(
        s: *mut Surface,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
        radius: c_int,
        color: u32,
        alpha: c_int,
    );
    fn vx_draw_outline(s: *mut Surface, x: c_int, y: c_int, w: c_int, h: c_int, color: u32);
    fn vx_draw_toolbar(s: *mut Surface, x: c_int, y: c_int, w: c_int, h: c_int);
    fn vx_draw_tab(
        s: *mut Surface,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
        label: *const c_char,
        chosen: bool,
    );
    fn vx_draw_button_flags(
        s: *mut Surface,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
        label: *const c_char,
        flags: c_uint,
    );
    fn vx_mix(a: u32, b: u32, amount: c_int) -> u32;

    fn vx_font_ui() -> *const c_void;
    fn vx_font_height(font: *const c_void) -> c_int;
    fn vx_text(
        s: *mut Surface,
        font: *const c_void,
        x: c_int,
        y: c_int,
        text: *const c_char,
        fg: u32,
        bg: u32,
    ) -> c_int;
    fn vx_text_width_font(font: *const c_void, text: *const c_char) -> c_int;
    fn vx_text_fit_bytes(font: *const c_void, text: *const c_char, width: c_int) -> usize;

    fn vx_menu_size(items: *const MenuItem, count: c_int, width: *mut c_int, height: *mut c_int);
    fn vx_draw_menu(
        s: *mut Surface,
        x: c_int,
        y: c_int,
        items: *const MenuItem,
        count: c_int,
        hot: c_int,
    );
    fn vx_menu_item_at(
        items: *const MenuItem,
        count: c_int,
        x: c_int,
        y: c_int,
        px: c_int,
        py: c_int,
    ) -> c_int;

    fn vx_clipboard_set(text: *const c_char, length: usize);
    fn vx_clipboard_get() -> *mut c_char;
    fn vx_alert(alert: *const Alert) -> c_int;
    fn free(pointer: *mut c_void);
}

pub const TRANSPARENT: u32 = 0xff000000;

/// The theme's colors, as the desktop has them now.
pub fn theme() -> &'static Theme {
    unsafe { &vx_theme }
}

pub fn mix(a: u32, b: u32, amount: i32) -> u32 {
    unsafe { vx_mix(a, b, amount) }
}

fn c_string(text: &str) -> CString {
    CString::new(text.replace('\0', "")).unwrap_or_default()
}

/// The window's text, in the desktop's UI font, in pixels.
pub fn text_width(text: &str) -> i32 {
    let text = c_string(text);
    unsafe { vx_text_width_font(vx_font_ui(), text.as_ptr()) }
}

/// How many bytes of `text` (whole characters) fit in `width` pixels.
pub fn fit_bytes(text: &str, width: i32) -> usize {
    if width <= 0 {
        return 0;
    }
    let c = c_string(text);
    let bytes = unsafe { vx_text_fit_bytes(vx_font_ui(), c.as_ptr(), width) };
    // The C string was cut at any NUL, which text from outside could have: stay inside `text`.
    let mut end = bytes.min(text.len());
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    end
}

pub fn font_height() -> i32 {
    unsafe { vx_font_height(vx_font_ui()) }
}

pub fn clipboard_set(text: &str) {
    let c = c_string(text);
    unsafe { vx_clipboard_set(c.as_ptr(), c.as_bytes().len()) };
}

pub fn clipboard_get() -> String {
    unsafe {
        let pointer = vx_clipboard_get();
        if pointer.is_null() {
            return String::new();
        }
        let text = CStr::from_ptr(pointer).to_string_lossy().into_owned();
        free(pointer as *mut c_void);
        text
    }
}

/// A question with buttons, in a window of its own; the button chosen (or the cancel one).
pub fn alert(title: &str, message: &str, detail: &str, buttons: &[&str], default: i32, cancel: i32) -> i32 {
    let title = c_string(title);
    let message = c_string(message);
    let detail = c_string(detail);
    let labels: Vec<CString> = buttons.iter().map(|b| c_string(b)).collect();
    let pointers: Vec<*const c_char> = labels.iter().map(|l| l.as_ptr()).collect();
    let alert = Alert {
        title: title.as_ptr(),
        icon: std::ptr::null(),
        message: message.as_ptr(),
        detail: if detail.as_bytes().is_empty() { std::ptr::null() } else { detail.as_ptr() },
        buttons: pointers.as_ptr(),
        button_count: pointers.len() as c_int,
        default_button: default,
        cancel_button: cancel,
    };
    unsafe { vx_alert(&alert) }
}

/// A pop-up menu: its items, each with a label and an optional shortcut, or a line.
pub struct Menu {
    items: Vec<MenuItem>,
    selectable: Vec<bool>,
    _strings: Vec<CString>,
}

impl Menu {
    /// `items`: (label, shortcut, disabled); an empty label is a line between groups.
    pub fn new(items: &[(&str, &str, bool)]) -> Menu {
        let mut strings = Vec::new();
        let mut list = Vec::new();
        let mut selectable = Vec::new();
        for (label, keys, disabled) in items {
            let label_pointer = if label.is_empty() {
                std::ptr::null()
            } else {
                strings.push(c_string(label));
                strings.last().unwrap().as_ptr()
            };
            let keys_pointer = if keys.is_empty() {
                std::ptr::null()
            } else {
                strings.push(c_string(keys));
                strings.last().unwrap().as_ptr()
            };
            list.push(MenuItem { label: label_pointer, keys: keys_pointer, disabled: *disabled });
            selectable.push(!label.is_empty() && !*disabled);
        }
        // The CStrings' buffers do not move when the Vec of them does.
        Menu { items: list, selectable, _strings: strings }
    }

    /// The next item that can be chosen, from `from` (-1: none yet) in `direction` (1 or -1),
    /// not going past the ends; `from` if there is none.
    pub fn step(&self, from: i32, direction: i32) -> i32 {
        let mut at = from;
        loop {
            at += direction;
            if at < 0 || at >= self.count() {
                return from;
            }
            if self.selectable[at as usize] {
                return at;
            }
        }
    }

    pub fn count(&self) -> i32 {
        self.items.len() as i32
    }

    pub fn size(&self) -> (i32, i32) {
        let (mut width, mut height) = (0, 0);
        unsafe { vx_menu_size(self.items.as_ptr(), self.count(), &mut width, &mut height) };
        (width, height)
    }

    pub fn item_at(&self, x: i32, y: i32, px: i32, py: i32) -> i32 {
        unsafe { vx_menu_item_at(self.items.as_ptr(), self.count(), x, y, px, py) }
    }
}

/// The window, and drawing on its surface.
pub struct Ui {
    window: *mut Window,
}

impl Ui {
    pub fn open(title: &str, width: i32, height: i32) -> Option<Ui> {
        let title = c_string(title);
        let window = unsafe { vx_window_create_flags(title.as_ptr(), width, height, WINDOW_RESIZABLE) };
        if window.is_null() {
            return None;
        }
        unsafe { vx_window_center(window) };
        Some(Ui { window })
    }

    pub fn width(&self) -> i32 {
        unsafe { (*self.window).surface.width }
    }

    pub fn height(&self) -> i32 {
        unsafe { (*self.window).surface.height }
    }

    fn surface(&self) -> *mut Surface {
        unsafe { &mut (*self.window).surface as *mut Surface }
    }

    /// Gives the window a surface of a new size (blank: draw all of it again).
    pub fn resize(&self, width: i32, height: i32) -> bool {
        unsafe { vx_window_resize(self.window, width, height) == 0 }
    }

    pub fn present(&self, x: i32, y: i32, width: i32, height: i32) {
        unsafe { vx_window_present(self.window, x, y, width, height) };
    }

    pub fn present_all(&self) {
        self.present(0, 0, self.width(), self.height());
    }

    pub fn set_title(&self, title: &str) {
        let title = c_string(title);
        unsafe { vx_window_set_title(self.window, title.as_ptr()) };
    }

    pub fn set_cursor(&self, shape: c_int) {
        unsafe { vx_window_set_cursor(self.window, shape) };
    }

    pub fn wait(&self, timeout_ms: i64) -> Option<Option<Event>> {
        let mut event: Event = unsafe { std::mem::zeroed() };
        let waited = unsafe { vx_gui_wait(&mut event, timeout_ms as c_long) };
        if waited < 0 {
            None
        } else if waited == 0 {
            Some(None)
        } else {
            Some(Some(event))
        }
    }

    pub fn fill(&self, x: i32, y: i32, w: i32, h: i32, color: u32) {
        unsafe { vx_fill(self.surface(), x, y, w, h, color) };
    }

    pub fn rounded(&self, x: i32, y: i32, w: i32, h: i32, radius: i32, color: u32) {
        unsafe { vx_fill_rounded(self.surface(), x, y, w, h, radius, color, 255) };
    }

    pub fn outline(&self, x: i32, y: i32, w: i32, h: i32, color: u32) {
        unsafe { vx_draw_outline(self.surface(), x, y, w, h, color) };
    }

    pub fn toolbar(&self, x: i32, y: i32, w: i32, h: i32) {
        unsafe { vx_draw_toolbar(self.surface(), x, y, w, h) };
    }

    pub fn tab(&self, x: i32, y: i32, w: i32, h: i32, label: &str, chosen: bool) {
        let label = c_string(label);
        unsafe { vx_draw_tab(self.surface(), x, y, w, h, label.as_ptr(), chosen) };
    }

    pub fn button(&self, x: i32, y: i32, w: i32, h: i32, label: &str, flags: c_uint) {
        let label = c_string(label);
        unsafe { vx_draw_button_flags(self.surface(), x, y, w, h, label.as_ptr(), flags) };
    }

    /// Text with its top at `y`, in the UI font; returns the x after it.
    pub fn text(&self, x: i32, y: i32, text: &str, color: u32) -> i32 {
        let text = c_string(text);
        unsafe { vx_text(self.surface(), vx_font_ui(), x, y, text.as_ptr(), color, TRANSPARENT) }
    }

    /// Text cut to `width` pixels.
    pub fn text_fit(&self, x: i32, y: i32, width: i32, text: &str, color: u32) -> i32 {
        let end = fit_bytes(text, width);
        self.text(x, y, &text[..end], color)
    }

    pub fn menu(&self, x: i32, y: i32, menu: &Menu, hot: i32) {
        unsafe { vx_draw_menu(self.surface(), x, y, menu.items.as_ptr(), menu.count(), hot) };
    }

    /// Draws a square icon of `pixels` (0xAARRGGBB, `size` by `size`) over what is on the
    /// surface, blended by their alpha.
    pub fn blend_icon(&self, x: i32, y: i32, pixels: &[u32], size: i32) {
        let surface = unsafe { &*self.surface() };
        for row in 0..size {
            for column in 0..size {
                let pixel = pixels[(row * size + column) as usize];
                let alpha = (pixel >> 24) as i32;
                let (px, py) = (x + column, y + row);
                if alpha == 0 || px < 0 || py < 0 || px >= surface.width || py >= surface.height {
                    continue;
                }
                unsafe {
                    let at = surface.pixels.offset((py * surface.stride + px) as isize);
                    *at = mix(*at & 0xffffff, pixel & 0xffffff, alpha);
                }
            }
        }
    }

    /// Writes a row of pixels (0xRRGGBB) into the surface.
    pub fn put_row(&self, x: i32, y: i32, pixels: &[u32]) {
        let surface = unsafe { &*self.surface() };
        if y < 0 || y >= surface.height || x < 0 {
            return;
        }
        let count = pixels.len().min((surface.width - x).max(0) as usize);
        unsafe {
            let row = surface.pixels.offset((y * surface.stride + x) as isize);
            std::ptr::copy_nonoverlapping(pixels.as_ptr(), row, count);
        }
    }
}

impl Drop for Ui {
    fn drop(&mut self) {
        unsafe { vx_window_destroy(self.window) };
    }
}
