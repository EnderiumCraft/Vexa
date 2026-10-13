//! Vane, a web browser for Vexa, on the Servo engine.
//!
//!   vane [URL...]
//!
//! A window with tabs, back, forward and reload buttons, an address bar, and the page below.
//! Servo paints through the software rendering context, and each frame it finishes is copied
//! into the window's surface and shown. The pages' titles become the tabs' and the window's.
//! Everything that happens is printed with a `vane: ` prefix, so a test can read it.
//!
//! Keys: Ctrl+T new tab, Ctrl+W close it, Ctrl+Tab and Ctrl+Shift+Tab (or Ctrl+PageDown and
//! Ctrl+PageUp) move between tabs, Ctrl+1 to Ctrl+9 pick one, Ctrl+L (or F6) edits the address,
//! Ctrl+R (or F5) reloads, Alt+Left and Alt+Right go back and forward, Ctrl+Plus, Ctrl+Minus and
//! Ctrl+0 zoom, Ctrl+Q quits.

mod address;
mod ffi;
mod nav;
mod tab;

use std::cell::{Cell, RefCell};
use std::error::Error;
use std::os::raw::c_int;
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use dpi::PhysicalSize;
use servo::resources::ResourceReaderMethods;
use servo::{
    Code, ContextMenu, ContextMenuAction, ContextMenuItem, DeviceIntPoint, DeviceIntRect,
    DevicePoint, EmbedderControl, EventLoopWaker,
    InputEvent, Key, KeyState, KeyboardEvent, Location, Modifiers, MouseButton,
    MouseButtonAction, MouseButtonEvent, MouseMoveEvent, NamedKey, RenderingContext, SelectElement,
    Servo, ServoBuilder, SimpleDialog, SoftwareRenderingContext, Theme, WebViewBuilder, WebViewPoint,
    WheelDelta, WheelEvent, WheelMode,
};
use servo_default_resources::DefaultResourceReader;
use url::Url;

use address::Address;
use ffi::{Event, Menu, Ui};
use tab::{Clipboard, Load, Shared, Tab, TabState};

const WIDTH: i32 = 960;
const HEIGHT: i32 = 680;
/// The row of tabs, and the toolbar under it with the buttons and the address bar.
const TAB_STRIP: i32 = 34;
const TOOLBAR: i32 = 40;
const CHROME: i32 = TAB_STRIP + TOOLBAR;
/// What one notch of the wheel scrolls the page by.
const PIXELS_PER_NOTCH: f64 = 40.0;
/// How often the page is copied into the window whether or not Servo reports a frame.
const PRESENT_INTERVAL: Duration = Duration::from_millis(250);
/// How often the toolbar is drawn while a page loads, for its moving bar.
const CHROME_INTERVAL: Duration = Duration::from_millis(80);
/// The page a new tab opens.
const START_PAGE: &str = "/apps/Vane.vxapp/Contents/Resources/start.html";

// Linux keyboard codes (Vexa's abi.h).
const K_ESC: c_int = 1;
const K_0: c_int = 11;
const K_MINUS: c_int = 12;
const K_EQUAL: c_int = 13;
const K_BACKSPACE: c_int = 14;
const K_TAB: c_int = 15;
const K_Q: c_int = 16;
const K_W: c_int = 17;
const K_R: c_int = 19;
const K_T: c_int = 20;
const K_ENTER: c_int = 28;
const K_A: c_int = 30;
const K_D: c_int = 32;
const K_L: c_int = 38;
const K_X: c_int = 45;
const K_C: c_int = 46;
const K_V: c_int = 47;
const K_KP_MINUS: c_int = 74;
const K_KP_PLUS: c_int = 78;
const K_F5: c_int = 63;
const K_F6: c_int = 64;
const K_HOME: c_int = 102;
const K_PAGE_UP: c_int = 104;
const K_UP: c_int = 103;
const K_LEFT: c_int = 105;
const K_DOWN: c_int = 108;
const K_RIGHT: c_int = 106;
const K_END: c_int = 107;
const K_PAGE_DOWN: c_int = 109;
const K_DELETE: c_int = 111;

struct NoWaker;

impl EventLoopWaker for NoWaker {
    fn clone_box(&self) -> Box<dyn EventLoopWaker> {
        Box::new(NoWaker)
    }

    fn wake(&self) {}
}

/// What the pointer is over.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Hit {
    None,
    Page,
    Tab(usize),
    TabClose(usize),
    NewTab,
    Back,
    Forward,
    Reload,
    Address,
    Menu,
}

/// What a pop-up menu is for.
enum PopupKind {
    /// The browser's menu.
    Browser,
    /// A page's `<select>`: the option ids, per menu item (None for a group's label).
    Select(SelectElement, Vec<Option<usize>>),
    /// A page's context menu (a right click): the action per menu item (None for a line).
    Context(ContextMenu, Vec<Option<ContextMenuAction>>),
}

struct Popup {
    menu: Menu,
    x: i32,
    y: i32,
    hot: i32,
    kind: PopupKind,
}

// The browser menu's items.
const MENU_NEW_TAB: i32 = 0;
const MENU_RELOAD: i32 = 1;
const MENU_ZOOM_IN: i32 = 3;
const MENU_ZOOM_OUT: i32 = 4;
const MENU_ZOOM_RESET: i32 = 5;
const MENU_COPY_ADDRESS: i32 = 7;
const MENU_ABOUT: i32 = 8;

fn tab_width(width: i32, count: usize) -> i32 {
    ((width - 16 - 40) / count.max(1) as i32).clamp(70, 220)
}

fn tab_rect(width: i32, count: usize, index: usize) -> (i32, i32, i32, i32) {
    (8 + index as i32 * tab_width(width, count), 5, tab_width(width, count) - 2, 28)
}

fn new_tab_rect(width: i32, count: usize) -> (i32, i32, i32, i32) {
    (8 + count as i32 * tab_width(width, count) + 4, 7, 28, 24)
}

const BACK: (i32, i32, i32, i32) = (8, TAB_STRIP + 6, 32, 28);
const FORWARD: (i32, i32, i32, i32) = (44, TAB_STRIP + 6, 32, 28);
const RELOAD: (i32, i32, i32, i32) = (80, TAB_STRIP + 6, 32, 28);

fn field_rect(width: i32) -> (i32, i32, i32, i32) {
    (120, TAB_STRIP + 7, width - 120 - 46, 26)
}

fn menu_button_rect(width: i32) -> (i32, i32, i32, i32) {
    (width - 38, TAB_STRIP + 6, 30, 28)
}

fn inside(x: i32, y: i32, rect: (i32, i32, i32, i32)) -> bool {
    x >= rect.0 && x < rect.0 + rect.2 && y >= rect.1 && y < rect.1 + rect.3
}

/// `text` cut to `width` pixels, ending in "..." when it was cut.
fn elide(text: &str, width: i32) -> String {
    if ffi::text_width(text) <= width {
        return text.to_string();
    }
    let end = ffi::fit_bytes(text, width - ffi::text_width("..."));
    format!("{}...", text[..end].trim_end())
}

fn start_url() -> Url {
    if std::path::Path::new(START_PAGE).exists() {
        Url::from_file_path(START_PAGE).unwrap_or_else(|_| Url::parse("about:blank").unwrap())
    } else {
        Url::parse("about:blank").unwrap()
    }
}

fn is_start_page(url: &Url) -> bool {
    url.scheme() == "file" && url.path() == START_PAGE || url.as_str() == "about:blank"
}

struct App {
    ui: Ui,
    servo: Servo,
    shared: Rc<Shared>,
    tabs: Vec<Tab>,
    active: usize,
    address: Address,
    hover: Hit,
    popup: Option<Popup>,
    modifiers: Modifiers,
    buttons: c_int,
    cursor: c_int,
    chrome_dirty: bool,
    quit: bool,
    started: Instant,
    reported_load: bool,
}

impl App {
    fn page_height(&self) -> u32 {
        (self.ui.height() - CHROME).max(1) as u32
    }

    fn theme(&self) -> Theme {
        if ffi::theme().dark {
            Theme::Dark
        } else {
            Theme::Light
        }
    }

    fn make_tab(&self, url: Url) -> Tab {
        let state = TabState::new(self.shared.clone());
        let webview = WebViewBuilder::new(&self.servo, self.shared.context.clone())
            .url(url)
            .delegate(state.clone())
            .clipboard_delegate(Rc::new(Clipboard))
            .build();
        webview.notify_theme_change(self.theme());
        Tab { webview, state, zoom: 1.0 }
    }

    /// Opens an address in a new tab, which becomes the one shown.
    fn open_tab(&mut self, url: Url, edit_address: bool) {
        let tab = self.make_tab(url);
        self.add_tab(tab);
        if edit_address {
            self.address.set("");
            self.address.focus();
            self.tabs[self.active].webview.set_focused(false);
        }
    }

    fn add_tab(&mut self, tab: Tab) {
        if let Some(old) = self.tabs.get(self.active) {
            old.webview.hide();
            old.webview.set_focused(false);
        }
        tab.webview.show();
        tab.webview.set_focused(!self.address.focused);
        self.tabs.push(tab);
        self.active = self.tabs.len() - 1;
        self.address.unfocus();
        self.sync_address();
        self.chrome_dirty = true;
        println!("vane: tab {} of {}", self.active + 1, self.tabs.len());
        self.update_window_title();
    }

    fn select_tab(&mut self, index: usize) {
        if index >= self.tabs.len() || index == self.active {
            return;
        }
        let old = &self.tabs[self.active];
        old.webview.hide();
        old.webview.set_focused(false);
        self.active = index;
        let tab = &self.tabs[index];
        tab.webview.show();
        tab.webview.set_focused(true);
        self.address.unfocus();
        self.sync_address();
        self.chrome_dirty = true;
        println!("vane: tab {} of {}", self.active + 1, self.tabs.len());
        self.update_window_title();
    }

    fn close_tab(&mut self, index: usize) {
        if index >= self.tabs.len() {
            return;
        }
        let was_active = index == self.active;
        self.tabs.remove(index);
        if self.tabs.is_empty() {
            self.quit = true;
            return;
        }
        if index < self.active {
            self.active -= 1;
        } else if was_active {
            self.active = index.min(self.tabs.len() - 1);
        }
        if was_active {
            let tab = &self.tabs[self.active];
            tab.webview.show();
            tab.webview.set_focused(true);
            self.address.unfocus();
        }
        self.sync_address();
        self.chrome_dirty = true;
        println!("vane: tab {} of {}", self.active + 1, self.tabs.len());
        self.update_window_title();
    }

    /// Makes the address bar show the active page's address, unless it is being edited.
    fn sync_address(&mut self) {
        if self.address.focused {
            return;
        }
        let text = match self.tabs[self.active].state.url.borrow().as_ref() {
            Some(url) if !is_start_page(url) => url.to_string(),
            _ => String::new(),
        };
        self.address.set(&text);
    }

    fn update_window_title(&self) {
        let title = self.tabs[self.active].title_or_address();
        self.ui.set_title(&format!("{title} - Vane"));
    }

    fn navigate(&mut self, text: &str) {
        let Some(url) = nav::resolve(text) else {
            println!("vane: bad address {text}");
            return;
        };
        println!("vane: navigate {url}");
        self.address.unfocus();
        let tab = &self.tabs[self.active];
        tab.webview.set_focused(true);
        tab.webview.load(url);
        self.chrome_dirty = true;
    }

    fn focus_address(&mut self) {
        self.address.focus();
        self.tabs[self.active].webview.set_focused(false);
        println!("vane: address focused");
        self.chrome_dirty = true;
    }

    fn leave_address(&mut self) {
        if self.address.focused {
            self.address.unfocus();
            self.tabs[self.active].webview.set_focused(true);
            self.sync_address();
            self.chrome_dirty = true;
        }
    }

    // ---- Drawing ----

    fn hit(&self, x: i32, y: i32) -> Hit {
        if y >= CHROME {
            return Hit::Page;
        }
        let width = self.ui.width();
        let count = self.tabs.len();
        for index in 0..count {
            let rect = tab_rect(width, count, index);
            if inside(x, y, rect) {
                return if x >= rect.0 + rect.2 - 24 { Hit::TabClose(index) } else { Hit::Tab(index) };
            }
        }
        if inside(x, y, new_tab_rect(width, count)) {
            return Hit::NewTab;
        }
        if inside(x, y, BACK) {
            return Hit::Back;
        }
        if inside(x, y, FORWARD) {
            return Hit::Forward;
        }
        if inside(x, y, RELOAD) {
            return Hit::Reload;
        }
        if inside(x, y, menu_button_rect(width)) {
            return Hit::Menu;
        }
        if inside(x, y, field_rect(width)) {
            return Hit::Address;
        }
        Hit::None
    }

    fn draw_chrome(&self) {
        let ui = &self.ui;
        let theme = ffi::theme();
        let width = ui.width();
        let count = self.tabs.len();
        let text_top = |top: i32, height: i32| top + (height - ffi::font_height()) / 2;
        ui.toolbar(0, 0, width, CHROME);

        for (index, tab) in self.tabs.iter().enumerate() {
            let (x, y, w, h) = tab_rect(width, count, index);
            let chosen = index == self.active;
            let favicon = tab.state.favicon.borrow();
            let loading = tab.state.loading();
            if let (Some(icon), false) = (favicon.as_ref(), loading) {
                // The page's icon, and the title after it.
                let label = elide(&tab.title_or_address(), w - 40 - tab::FAVICON_SIZE - 4);
                ui.tab(x, y, w, h, "", chosen);
                ui.blend_icon(x + 8, y + (h - tab::FAVICON_SIZE) / 2, icon, tab::FAVICON_SIZE);
                let top = y + (h - ffi::font_height()) / 2;
                let left = x + 8 + tab::FAVICON_SIZE + 4;
                if chosen {
                    ui.text(left, top + 1, &label, ffi::mix(theme.accent, 0x000000, 110));
                }
                ui.text(left, top, &label, if chosen { 0xffffff } else { theme.text });
            } else {
                let label = elide(&tab.title_or_address(), w - 40);
                ui.tab(x, y, w, h, &label, chosen);
                if loading {
                    // A small dot in the tab's corner while the page loads.
                    ui.rounded(x + 6, y + h / 2 - 2, 4, 4, 2, theme.accent);
                }
            }
            let hot = self.hover == Hit::TabClose(index);
            if chosen || hot || matches!(self.hover, Hit::Tab(i) if i == index) {
                let color = if hot { theme.text } else { theme.dim };
                ui.text(x + w - 18, text_top(y, h), "\u{d7}", color);
            }
        }
        let (x, y, w, h) = new_tab_rect(width, count);
        ui.button(x, y, w, h, "+", if self.hover == Hit::NewTab { ffi::BUTTON_HOT } else { 0 });

        let tab = &self.tabs[self.active];
        let flag = |hot: bool, disabled: bool| {
            (if hot { ffi::BUTTON_HOT } else { 0 }) | (if disabled { ffi::BUTTON_DISABLED } else { 0 })
        };
        let (x, y, w, h) = BACK;
        ui.button(x, y, w, h, "\u{2190}", flag(self.hover == Hit::Back, !tab.webview.can_go_back()));
        let (x, y, w, h) = FORWARD;
        ui.button(x, y, w, h, "\u{2192}", flag(self.hover == Hit::Forward, !tab.webview.can_go_forward()));
        let (x, y, w, h) = RELOAD;
        ui.button(x, y, w, h, "\u{21bb}", flag(self.hover == Hit::Reload, false));
        let (x, y, w, h) = menu_button_rect(width);
        let open = matches!(&self.popup, Some(p) if matches!(p.kind, PopupKind::Browser));
        ui.button(x, y, w, h, "\u{2261}", flag(self.hover == Hit::Menu || open, false));

        self.draw_field(tab);

        // The load's progress: a bar that moves while the page loads.
        if tab.state.loading() {
            let segment = width / 4;
            let travel = width + segment;
            let at = (self.started.elapsed().as_millis() as i32 / 5) % travel - segment;
            let from = at.max(0);
            let to = (at + segment).min(width);
            if to > from {
                ui.fill(from, CHROME - 3, to - from, 2, theme.accent);
            }
        }
    }

    fn draw_field(&self, tab: &Tab) {
        let ui = &self.ui;
        let theme = ffi::theme();
        let (fx, fy, fw, fh) = field_rect(ui.width());
        ui.rounded(fx, fy, fw, fh, 6, theme.view);
        ui.outline(fx, fy, fw, fh, if self.address.focused { theme.accent } else { theme.line });
        let top = fy + (fh - ffi::font_height()) / 2;

        let secure = tab.state.url.borrow().as_ref().map(|u| u.scheme().to_string());
        let mut text_x = fx + 10;
        if !self.address.focused {
            match secure.as_deref() {
                Some("https") => {
                    // A padlock.
                    let (lx, ly) = (fx + 9, fy + 7);
                    ui.fill(lx + 2, ly, 5, 1, 0x2e9d4f);
                    ui.fill(lx + 1, ly + 1, 1, 4, 0x2e9d4f);
                    ui.fill(lx + 7, ly + 1, 1, 4, 0x2e9d4f);
                    ui.rounded(lx - 1, ly + 5, 11, 8, 2, 0x2e9d4f);
                    text_x = fx + 28;
                },
                Some("http") => {
                    ui.text(fx + 9, top, "!", 0xd9822b);
                    text_x = fx + 24;
                },
                _ => {},
            }
        }
        let zoom = tab.zoom;
        let mut right = fx + fw - 8;
        if (zoom - 1.0).abs() > 0.001 {
            let label = format!("{}%", (zoom * 100.0).round() as i32);
            let label_width = ffi::text_width(&label);
            ui.text(right - label_width, top, &label, theme.dim);
            right -= label_width + 10;
        }
        let room = right - text_x;

        if self.address.focused {
            let start = self.address.visible_start(room);
            let visible = &self.address.text[start..];
            if let Some((from, to)) = self.address.selection() {
                let from = from.max(start);
                if to > from {
                    let before = ffi::text_width(&self.address.text[start..from]);
                    let inside = ffi::text_width(&self.address.text[from..to]);
                    ui.fill(text_x + before, fy + 3, inside.min(room - before).max(0), fh - 6, theme.selected);
                }
            }
            ui.text_fit(text_x, top, room, visible, theme.text);
            let caret = text_x + ffi::text_width(&self.address.text[start..self.address.cursor]);
            if caret < right {
                ui.fill(caret, fy + 5, 1, fh - 10, theme.text);
            }
        } else if self.address.text.is_empty() {
            ui.text_fit(text_x, top, room, "Search or enter address", theme.dim);
        } else {
            let (scheme, host, rest) = nav::split_for_display(&self.address.text);
            let mut x = text_x;
            for (part, color) in [(scheme, theme.dim), (host, theme.text), (rest, theme.dim)] {
                let left = right - x;
                if left <= 0 || part.is_empty() {
                    continue;
                }
                x = ui.text_fit(x, top, left, part, color);
            }
        }
    }

    /// The status text over the page's bottom left corner, and the pop-up menu.
    fn draw_overlays(&self) {
        let ui = &self.ui;
        let theme = ffi::theme();
        if let Some(status) = self.tabs[self.active].state.status.borrow().as_ref() {
            let width = (ffi::text_width(status) + 16).min(ui.width() * 2 / 3);
            let y = ui.height() - 24;
            ui.rounded(0, y, width, 24, 4, theme.window);
            ui.outline(0, y, width, 24, theme.line);
            ui.text_fit(8, y + (24 - ffi::font_height()) / 2, width - 16, status, theme.text);
        }
        if let Some(popup) = &self.popup {
            ui.menu(popup.x, popup.y, &popup.menu, popup.hot);
        }
    }

    /// Copies the frame Servo has painted into the window below the toolbar, and shows it.
    fn present_page(&mut self) {
        let tab = &self.tabs[self.active];
        tab.webview.paint();
        let context = &self.shared.context;
        let size = context.size();
        let rect = DeviceIntRect::new(
            DeviceIntPoint::new(0, 0),
            DeviceIntPoint::new(size.width as i32, size.height as i32),
        );
        let Some(image) = context.read_to_image(rect) else {
            return;
        };
        // Painting is slow, and a page's title comes before its first frame: the program says
        // when the colour of the page's top left corner changes, and when the top right
        // corner's does or the page's width does, which the tests wait for.
        static CORNER: AtomicU32 = AtomicU32::new(0xffffff);
        static EDGE: AtomicU64 = AtomicU64::new(0xffffff);
        let width = image.width() as usize;
        let raw = image.as_raw();
        let color = |x: usize| -> u32 {
            let p = &raw[x * 4..x * 4 + 3];
            (p[0] as u32) << 16 | (p[1] as u32) << 8 | p[2] as u32
        };
        let corner = color(0);
        if CORNER.swap(corner, Ordering::Relaxed) != corner {
            println!("vane: corner #{corner:06x}");
        }
        let edge = color(width - 1);
        let state = (width as u64) << 32 | edge as u64;
        if EDGE.swap(state, Ordering::Relaxed) != state {
            println!("vane: edge #{edge:06x} at {width}");
        }

        let mut row = vec![0u32; width];
        for y in 0..image.height() as usize {
            let line = &raw[y * width * 4..(y + 1) * width * 4];
            for (pixel, rgba) in row.iter_mut().zip(line.chunks_exact(4)) {
                *pixel = (rgba[0] as u32) << 16 | (rgba[1] as u32) << 8 | rgba[2] as u32;
            }
            self.ui.put_row(0, CHROME + y as i32, &row);
        }
        self.draw_chrome();
        self.draw_overlays();
        self.ui.present_all();
        self.chrome_dirty = false;
    }

    fn present_chrome(&mut self) {
        self.draw_chrome();
        self.draw_overlays();
        self.ui.present(0, 0, self.ui.width(), CHROME);
        self.chrome_dirty = false;
    }

    // ---- Events ----

    fn resize(&mut self, width: i32, height: i32) {
        // A failed resize keeps the old surface, so the page keeps its old size too.
        if !self.ui.resize(width, height) {
            println!("vane: resize to {width}x{height} failed");
            return;
        }
        println!("vane: resized to {width}x{height}");
        self.tabs[self.active]
            .webview
            .resize(PhysicalSize::new(width as u32, self.page_height()));
        self.popup = None;
        self.present_chrome();
        self.ui.present_all();
    }

    fn set_cursor(&mut self, shape: c_int) {
        if shape != self.cursor {
            self.cursor = shape;
            self.ui.set_cursor(shape);
        }
    }

    fn key(&mut self, event: &Event) {
        let down = event.value != 0;
        let modifier = match event.key {
            42 | 54 => Modifiers::SHIFT,
            29 | 97 => Modifiers::CONTROL,
            56 | 100 => Modifiers::ALT,
            125 => Modifiers::META,
            _ => Modifiers::empty(),
        };
        if down {
            self.modifiers.insert(modifier);
        } else {
            self.modifiers.remove(modifier);
        }
        if down && self.shortcut(event) {
            return;
        }
        if self.popup.is_some() {
            if down {
                self.popup_key(event.key);
            }
            return;
        }
        if self.address.focused {
            if down {
                self.address_key(event);
                self.chrome_dirty = true;
            }
            return;
        }

        let state = if down { KeyState::Down } else { KeyState::Up };
        let control = self.modifiers.contains(Modifiers::CONTROL);
        let key = named_key(event.key).map(Key::Named).or_else(|| {
            let c = event.character as u32;
            // Ctrl+letter arrives as the control character 1 to 26: the page wants the letter.
            let c = if control && (1..=26).contains(&c) { b'a' as u32 + c - 1 } else { c };
            char::from_u32(c)
                .filter(|c| !c.is_control())
                .map(|c| Key::Character(c.to_string()))
        });
        let Some(key) = key else {
            return;
        };
        self.tabs[self.active]
            .webview
            .notify_input_event(InputEvent::Keyboard(KeyboardEvent::new_without_event(
                state,
                key,
                Code::Unidentified,
                Location::Standard,
                self.modifiers,
                event.value == 2,
                false,
            )));
    }

    /// The browser's own keys. True if the key was one of them.
    fn shortcut(&mut self, event: &Event) -> bool {
        let control = self.modifiers.contains(Modifiers::CONTROL);
        let alt = self.modifiers.contains(Modifiers::ALT);
        let shift = self.modifiers.contains(Modifiers::SHIFT);
        let count = self.tabs.len();
        match event.key {
            K_T if control => self.open_tab(start_url(), true),
            K_W if control => self.close_tab(self.active),
            K_Q if control => self.quit = true,
            K_TAB if control => {
                let next = if shift { (self.active + count - 1) % count } else { (self.active + 1) % count };
                self.select_tab(next);
            },
            K_PAGE_DOWN if control => self.select_tab((self.active + 1) % count),
            K_PAGE_UP if control => self.select_tab((self.active + count - 1) % count),
            code @ 2..=10 if control => {
                let index = (code - 2) as usize;
                self.select_tab(if code == 10 { count - 1 } else { index.min(count - 1) });
            },
            K_L if control => self.focus_address(),
            K_D if alt => self.focus_address(),
            K_F6 => self.focus_address(),
            K_R if control => self.tabs[self.active].webview.reload(),
            K_F5 => self.tabs[self.active].webview.reload(),
            K_LEFT if alt => {
                self.tabs[self.active].webview.go_back(1);
            },
            K_RIGHT if alt => {
                self.tabs[self.active].webview.go_forward(1);
            },
            K_EQUAL | K_KP_PLUS if control => self.zoom(1),
            K_MINUS | K_KP_MINUS if control => self.zoom(-1),
            K_0 if control => self.zoom(0),
            _ => return false,
        }
        self.chrome_dirty = true;
        true
    }

    fn zoom(&mut self, direction: i32) {
        self.tabs[self.active].zoom_step(direction);
        println!("vane: zoom {}", (self.tabs[self.active].zoom * 100.0).round() as i32);
        self.chrome_dirty = true;
    }

    /// A key pressed while the address bar has the keyboard.
    fn address_key(&mut self, event: &Event) {
        let control = self.modifiers.contains(Modifiers::CONTROL);
        let shift = self.modifiers.contains(Modifiers::SHIFT);
        match event.key {
            K_ESC => self.leave_address(),
            K_ENTER => {
                let text = self.address.text.clone();
                self.navigate(&text);
            },
            K_BACKSPACE => self.address.key(address::Key::Backspace, false),
            K_DELETE => self.address.key(address::Key::Delete, false),
            K_LEFT => self.address.key(address::Key::Left, shift),
            K_RIGHT => self.address.key(address::Key::Right, shift),
            K_HOME => self.address.key(address::Key::Home, shift),
            K_END => self.address.key(address::Key::End, shift),
            K_A if control => self.address.key(address::Key::SelectAll, false),
            K_C if control => {
                let text = self.address.selected_text().unwrap_or(&self.address.text).to_string();
                ffi::clipboard_set(&text);
            },
            K_X if control => {
                let text = self.address.selected_text().unwrap_or(&self.address.text).to_string();
                ffi::clipboard_set(&text);
                if self.address.selection().is_none() {
                    self.address.key(address::Key::SelectAll, false);
                }
                self.address.key(address::Key::Backspace, false);
            },
            K_V if control => {
                let text = ffi::clipboard_get();
                self.address.key(address::Key::Paste(&text), false);
            },
            _ if !control => {
                if let Some(c) = char::from_u32(event.character as u32).filter(|c| !c.is_control()) {
                    self.address.key(address::Key::Char(c), false);
                }
            },
            _ => {},
        }
    }

    fn pointer(&mut self, event: &Event) {
        let changed = event.buttons ^ self.buttons;
        self.buttons = event.buttons;
        let pressed = changed & event.buttons != 0;
        let (x, y) = (event.x, event.y);

        if let Some(popup) = &mut self.popup {
            let hot = popup.menu.item_at(popup.x, popup.y, x, y);
            if hot != popup.hot {
                popup.hot = hot;
                self.chrome_dirty = true;
            }
            if pressed {
                let popup = self.popup.take().unwrap();
                self.chrome_dirty = true;
                // A press outside the menu only closes it.
                if hot >= 0 {
                    self.popup_chosen(popup, hot);
                }
            }
            return;
        }

        let hit = self.hit(x, y);
        if hit != self.hover {
            self.hover = hit;
            self.chrome_dirty = true;
        }
        let shape = match hit {
            Hit::Page => self.tabs[self.active].state.cursor.get(),
            Hit::Address => ffi::CURSOR_TEXT,
            Hit::None => ffi::CURSOR_ARROW,
            _ => ffi::CURSOR_ARROW,
        };
        self.set_cursor(shape);

        if hit != Hit::Page {
            if pressed {
                self.chrome_press(hit, event);
            }
            return;
        }

        if pressed && self.address.focused {
            self.leave_address();
        }
        let webview = &self.tabs[self.active].webview;
        let point = WebViewPoint::Device(DevicePoint::new(x as f32, (y - CHROME) as f32));
        webview.notify_input_event(InputEvent::MouseMove(MouseMoveEvent::new(point)));
        for (bit, button) in [
            (0, MouseButton::Primary),
            (1, MouseButton::Secondary),
            (2, MouseButton::Auxiliary),
        ] {
            if changed & (1 << bit) != 0 {
                let action = if event.buttons & (1 << bit) != 0 {
                    MouseButtonAction::Down
                } else {
                    MouseButtonAction::Up
                };
                webview.notify_input_event(InputEvent::MouseButton(MouseButtonEvent::new(
                    action, button, point,
                )));
            }
        }
        // Servo takes wheel deltas as winit does, where a positive y turns the page up (the
        // DOM's deltaY is the negative of it). The wheel is positive when it turns up, too.
        if event.wheel != 0 {
            println!("vane: wheel {}", event.wheel);
            let delta = WheelDelta {
                x: 0.0,
                y: (event.wheel as f64) * PIXELS_PER_NOTCH,
                z: 0.0,
                mode: WheelMode::DeltaPixel,
            };
            webview.notify_input_event(InputEvent::Wheel(WheelEvent::new(delta, point)));
        }
    }

    /// A press on the toolbar or the tabs.
    fn chrome_press(&mut self, hit: Hit, event: &Event) {
        let middle = event.buttons & 4 != 0;
        match hit {
            Hit::Tab(index) if middle => self.close_tab(index),
            Hit::Tab(index) => self.select_tab(index),
            Hit::TabClose(index) => self.close_tab(index),
            Hit::NewTab => self.open_tab(start_url(), true),
            Hit::Back => {
                self.tabs[self.active].webview.go_back(1);
            },
            Hit::Forward => {
                self.tabs[self.active].webview.go_forward(1);
            },
            Hit::Reload => self.tabs[self.active].webview.reload(),
            Hit::Menu => self.open_browser_menu(),
            Hit::Address => {
                let (fx, _, fw, _) = field_rect(self.ui.width());
                if self.address.focused {
                    // A click in the text moves the cursor there.
                    let text_x = fx + 10;
                    let start = self.address.visible_start(fw - 20);
                    self.address.cursor = self.address.byte_at(start, event.x - text_x);
                    self.address.anchor = None;
                } else {
                    self.focus_address();
                }
            },
            _ => {
                self.leave_address();
            },
        }
        self.chrome_dirty = true;
    }

    /// A key while a pop-up menu is open: Up and Down move through its items, Enter chooses
    /// the one marked, Escape closes it.
    fn popup_key(&mut self, key: c_int) {
        let Some(popup) = &mut self.popup else {
            return;
        };
        match key {
            K_ESC => {
                self.popup = None;
            },
            K_UP | K_DOWN => {
                let first = if popup.hot < 0 && key == K_UP { popup.menu.count() } else { popup.hot };
                popup.hot = popup.menu.step(first, if key == K_UP { -1 } else { 1 });
            },
            K_ENTER if popup.hot >= 0 => {
                let hot = popup.hot;
                let popup = self.popup.take().unwrap();
                self.popup_chosen(popup, hot);
            },
            _ => {},
        }
        self.chrome_dirty = true;
    }

    fn open_browser_menu(&mut self) {
        let has_address = !self.address.text.is_empty();
        let menu = Menu::new(&[
            ("New Tab", "Ctrl+T", false),
            ("Reload", "Ctrl+R", false),
            ("", "", false),
            ("Zoom In", "Ctrl++", false),
            ("Zoom Out", "Ctrl+-", false),
            ("Actual Size", "Ctrl+0", false),
            ("", "", false),
            ("Copy Address", "", !has_address),
            ("About Vane", "", false),
        ]);
        let (width, _) = menu.size();
        let x = (self.ui.width() - width - 6).max(0);
        self.popup = Some(Popup { menu, x, y: CHROME - 4, hot: -1, kind: PopupKind::Browser });
    }

    fn popup_chosen(&mut self, popup: Popup, item: i32) {
        match popup.kind {
            PopupKind::Browser => match item {
                MENU_NEW_TAB => self.open_tab(start_url(), true),
                MENU_RELOAD => self.tabs[self.active].webview.reload(),
                MENU_ZOOM_IN => self.zoom(1),
                MENU_ZOOM_OUT => self.zoom(-1),
                MENU_ZOOM_RESET => self.zoom(0),
                MENU_COPY_ADDRESS => {
                    if let Some(url) = self.tabs[self.active].state.url.borrow().as_ref() {
                        ffi::clipboard_set(url.as_str());
                    }
                },
                MENU_ABOUT => {
                    ffi::alert(
                        "About Vane",
                        "Vane",
                        "A web browser for Vexa. Pages are drawn by the Servo engine \
                         (servo.org), under the Mozilla Public License 2.0.",
                        &["OK"],
                        0,
                        0,
                    );
                },
                _ => {},
            },
            PopupKind::Select(mut select, ids) => {
                if let Some(Some(id)) = ids.get(item as usize) {
                    select.select(vec![*id]);
                    select.submit();
                }
            },
            PopupKind::Context(menu, actions) => {
                if let Some(Some(action)) = actions.get(item as usize) {
                    menu.select(*action);
                }
            },
        }
    }

    /// Dialogs, and pickers, that pages put up.
    fn handle_controls(&mut self) {
        let controls: Vec<_> = self.shared.controls.borrow_mut().drain(..).collect();
        for (origin, control) in controls {
            match control {
                EmbedderControl::SimpleDialog(dialog) => {
                    let message = dialog.message().to_string();
                    let title = format!("{origin} says");
                    match &dialog {
                        SimpleDialog::Alert(_) => {
                            ffi::alert(&title, &message, "", &["OK"], 0, 0);
                            dialog.confirm();
                        },
                        SimpleDialog::Confirm(_) => {
                            if ffi::alert(&title, &message, "", &["Cancel", "OK"], 1, 0) == 1 {
                                dialog.confirm();
                            } else {
                                dialog.dismiss();
                            }
                        },
                        // Vexa has no dialog with a text field yet: the question is cancelled.
                        SimpleDialog::Prompt(_) => dialog.dismiss(),
                    }
                },
                EmbedderControl::SelectElement(select) => self.open_select(select),
                EmbedderControl::ContextMenu(menu) => self.open_context_menu(menu),
                // Colour and file pickers and input methods are not shown yet: dropping them
                // cancels them.
                _ => {},
            }
        }
    }

    fn open_select(&mut self, select: SelectElement) {
        let mut items: Vec<(String, bool)> = Vec::new();
        let mut ids: Vec<Option<usize>> = Vec::new();
        let chosen = select.selected_options();
        for entry in select.options() {
            match entry {
                servo::SelectElementOptionOrOptgroup::Option(option) => {
                    let mark = if chosen.contains(&option.id) { "\u{2713} " } else { "    " };
                    items.push((format!("{mark}{}", option.label), option.is_disabled));
                    ids.push(Some(option.id));
                },
                servo::SelectElementOptionOrOptgroup::Optgroup { label, options } => {
                    items.push((label.clone(), true));
                    ids.push(None);
                    for option in options {
                        let mark = if chosen.contains(&option.id) { "\u{2713} " } else { "    " };
                        items.push((format!("  {mark}{}", option.label), option.is_disabled));
                        ids.push(Some(option.id));
                    }
                },
            }
        }
        let list: Vec<(&str, &str, bool)> = items.iter().map(|(l, d)| (l.as_str(), "", *d)).collect();
        let menu = Menu::new(&list);
        let position = select.position();
        let (width, height) = menu.size();
        let x = position.min.x.clamp(0, (self.ui.width() - width).max(0));
        let y = (CHROME + position.max.y).min((self.ui.height() - height).max(CHROME));
        self.popup = Some(Popup { menu, x, y, hot: -1, kind: PopupKind::Select(select, ids) });
        self.chrome_dirty = true;
    }

    fn open_context_menu(&mut self, menu: ContextMenu) {
        let mut entries: Vec<(&str, &str, bool)> = Vec::new();
        let mut actions: Vec<Option<ContextMenuAction>> = Vec::new();
        for item in menu.items() {
            match item {
                ContextMenuItem::Item { label, action, enabled } => {
                    entries.push((label.as_str(), "", !*enabled));
                    actions.push(Some(*action));
                },
                ContextMenuItem::Separator => {
                    entries.push(("", "", false));
                    actions.push(None);
                },
            }
        }
        if entries.is_empty() {
            return;
        }
        let labels: Vec<&str> = entries.iter().map(|e| e.0).filter(|l| !l.is_empty()).collect();
        println!("vane: context menu: {}", labels.join(" | "));
        let list = Menu::new(&entries);
        let position = menu.position();
        let (width, height) = list.size();
        let x = position.min.x.clamp(0, (self.ui.width() - width).max(0));
        let y = (CHROME + position.min.y).min((self.ui.height() - height).max(CHROME));
        self.popup = Some(Popup { menu: list, x, y, hot: -1, kind: PopupKind::Context(menu, actions) });
        self.chrome_dirty = true;
    }

    /// What the tabs' pages have told the program since the last turn.
    fn sync_tabs(&mut self) {
        let new_tabs: Vec<Tab> = self.shared.new_tabs.borrow_mut().drain(..).collect();
        for tab in new_tabs {
            self.add_tab(tab);
        }
        for index in (0..self.tabs.len()).rev() {
            if self.tabs[index].state.closed.get() {
                self.close_tab(index);
            }
        }
        if self.tabs.is_empty() {
            return;
        }
        for index in 0..self.tabs.len() {
            let state = self.tabs[index].state.clone();
            let active = index == self.active;
            let titles: Vec<String> = state.titles.borrow_mut().drain(..).collect();
            if state.title_changed.replace(false) {
                self.chrome_dirty = true;
                if active {
                    self.update_window_title();
                    // Every title is printed, in order: a page can set two between two looks.
                    for title in &titles {
                        println!("vane: title {title}");
                    }
                }
            }
            if state.url_changed.replace(false) && active {
                self.sync_address();
                self.chrome_dirty = true;
            }
            if state.load_changed.replace(false) {
                self.chrome_dirty = true;
                if state.load.get() == Load::Complete && active {
                    if !self.reported_load {
                        self.reported_load = true;
                        println!("vane: page loaded");
                    }
                    println!("vane: load complete");
                }
            }
        }
        let shape = if self.hover == Hit::Page { self.tabs[self.active].state.cursor.get() } else { self.cursor };
        self.set_cursor(shape);
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let urls: Vec<Url> = std::env::args()
        .skip(1)
        .filter_map(|argument| nav::resolve(&argument))
        .collect();
    // The default resources register through inventory. Taking the reader as a trait object
    // keeps the linker from dropping them.
    let _resources: &dyn ResourceReaderMethods = &DefaultResourceReader;

    let Some(ui) = Ui::open("Vane", WIDTH, HEIGHT) else {
        eprintln!("vane: no desktop to open a window on");
        std::process::exit(1);
    };
    ui.fill(0, 0, WIDTH, HEIGHT, 0xffffff);
    ui.present_all();

    let context = Rc::new(
        SoftwareRenderingContext::new(PhysicalSize::new(WIDTH as u32, (HEIGHT - CHROME) as u32))
            .expect("Could not create the software rendering context"),
    );
    context
        .make_current()
        .expect("Could not make the rendering context current");

    let servo = ServoBuilder::default()
        .event_loop_waker(Box::new(NoWaker))
        .build();
    servo.setup_logging();

    let shared = Rc::new(Shared {
        context,
        new_tabs: RefCell::new(Vec::new()),
        controls: RefCell::new(Vec::new()),
        frame_ready: Cell::new(false),
    });
    let mut app = App {
        ui,
        servo,
        shared,
        tabs: Vec::new(),
        active: 0,
        address: Address::default(),
        hover: Hit::None,
        popup: None,
        modifiers: Modifiers::empty(),
        buttons: 0,
        cursor: ffi::CURSOR_ARROW,
        chrome_dirty: true,
        quit: false,
        started: Instant::now(),
        reported_load: false,
    };
    let mut first = urls;
    if first.is_empty() {
        first.push(start_url());
    }
    for url in first {
        app.open_tab(url, false);
    }
    // The first tab opened is the one shown first.
    app.select_tab(0);
    app.present_chrome();

    let mut last_present = Instant::now();
    let mut last_chrome = Instant::now();
    while !app.quit {
        app.servo.spin_event_loop();

        // Events come in every few milliseconds, and Servo gets a turn between them.
        match app.ui.wait(10) {
            None => break,
            Some(Some(event)) => match event.kind {
                ffi::GUI_CLOSE => break,
                ffi::GUI_KEY => app.key(&event),
                ffi::GUI_POINTER => app.pointer(&event),
                ffi::GUI_FOCUS => {
                    let focused = event.value != 0 && !app.address.focused && app.popup.is_none();
                    app.tabs[app.active].webview.set_focused(focused);
                },
                ffi::GUI_RESIZE => app.resize(event.width, event.height),
                ffi::GUI_THEME => {
                    for tab in &app.tabs {
                        tab.webview.notify_theme_change(app.theme());
                    }
                    app.chrome_dirty = true;
                },
                _ => {},
            },
            Some(None) => {},
        }
        if app.quit {
            break;
        }

        app.sync_tabs();
        app.handle_controls();
        if app.quit || app.tabs.is_empty() {
            break;
        }

        let loading = app.tabs[app.active].state.loading();
        if app.shared.frame_ready.replace(false) || last_present.elapsed() >= PRESENT_INTERVAL {
            app.present_page();
            last_present = Instant::now();
            last_chrome = Instant::now();
        } else if app.chrome_dirty || (loading && last_chrome.elapsed() >= CHROME_INTERVAL) {
            app.present_chrome();
            last_chrome = Instant::now();
        }
    }
    Ok(())
}

/// The named keys, by the codes in Vexa's abi.h (the Linux keyboard codes).
fn named_key(code: c_int) -> Option<NamedKey> {
    const FUNCTION_KEYS: [NamedKey; 10] = [
        NamedKey::F1,
        NamedKey::F2,
        NamedKey::F3,
        NamedKey::F4,
        NamedKey::F5,
        NamedKey::F6,
        NamedKey::F7,
        NamedKey::F8,
        NamedKey::F9,
        NamedKey::F10,
    ];
    Some(match code {
        1 => NamedKey::Escape,
        14 => NamedKey::Backspace,
        15 => NamedKey::Tab,
        28 => NamedKey::Enter,
        29 | 97 => NamedKey::Control,
        42 | 54 => NamedKey::Shift,
        56 | 100 => NamedKey::Alt,
        58 => NamedKey::CapsLock,
        59..=68 => FUNCTION_KEYS[(code - 59) as usize],
        102 => NamedKey::Home,
        103 => NamedKey::ArrowUp,
        104 => NamedKey::PageUp,
        105 => NamedKey::ArrowLeft,
        106 => NamedKey::ArrowRight,
        107 => NamedKey::End,
        108 => NamedKey::ArrowDown,
        109 => NamedKey::PageDown,
        110 => NamedKey::Insert,
        111 => NamedKey::Delete,
        125 => NamedKey::Meta,
        _ => return None,
    })
}
