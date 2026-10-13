//! A tab: a WebView, and what its page has told the program (title, address, load state, status
//! text, cursor, dialogs, new windows).

use std::cell::{Cell, RefCell};
use std::os::raw::c_int;
use std::rc::Rc;

use servo::{
    ClipboardDelegate, Cursor, EmbedderControl, LoadStatus, SoftwareRenderingContext,
    StringRequest, WebView, WebViewDelegate,
};
use url::Url;

use crate::ffi;

/// What every tab shares: the rendering context, and what the pages ask the program to do.
pub struct Shared {
    pub context: Rc<SoftwareRenderingContext>,
    /// Tabs that pages opened (links with a target, `window.open`), to be added.
    pub new_tabs: RefCell<Vec<Tab>>,
    /// Dialogs and pickers the pages put up, with the address of the page that asked.
    pub controls: RefCell<Vec<(String, EmbedderControl)>>,
    /// A frame was finished, in any tab.
    pub frame_ready: Cell<bool>,
}

/// How far a page's load is.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Load {
    Started,
    HeadParsed,
    Complete,
}

pub struct TabState {
    shared: Rc<Shared>,
    pub title: RefCell<String>,
    pub title_changed: Cell<bool>,
    /// Every title the page set since the program last looked, in order.
    pub titles: RefCell<Vec<String>>,
    pub url: RefCell<Option<Url>>,
    pub url_changed: Cell<bool>,
    pub load: Cell<Load>,
    pub load_changed: Cell<bool>,
    pub status: RefCell<Option<String>>,
    pub cursor: Cell<c_int>,
    /// The page's icon, as 16 by 16 pixels (0xAARRGGBB).
    pub favicon: RefCell<Option<Vec<u32>>>,
    /// The page closed itself (`window.close()`).
    pub closed: Cell<bool>,
}

impl TabState {
    pub fn new(shared: Rc<Shared>) -> Rc<TabState> {
        Rc::new(TabState {
            shared,
            title: RefCell::new(String::new()),
            title_changed: Cell::new(false),
            titles: RefCell::new(Vec::new()),
            url: RefCell::new(None),
            url_changed: Cell::new(false),
            load: Cell::new(Load::Started),
            load_changed: Cell::new(false),
            status: RefCell::new(None),
            cursor: Cell::new(ffi::CURSOR_ARROW),
            favicon: RefCell::new(None),
            closed: Cell::new(false),
        })
    }

    pub fn loading(&self) -> bool {
        self.load.get() != Load::Complete
    }
}

fn cursor_shape(cursor: Cursor) -> c_int {
    match cursor {
        Cursor::Pointer | Cursor::Grab | Cursor::Grabbing | Cursor::Alias => ffi::CURSOR_HAND,
        Cursor::Text | Cursor::VerticalText => ffi::CURSOR_TEXT,
        Cursor::Wait | Cursor::Progress => ffi::CURSOR_WAIT,
        Cursor::Crosshair | Cursor::Cell => ffi::CURSOR_CROSS,
        Cursor::Move
        | Cursor::AllScroll
        | Cursor::EResize
        | Cursor::NResize
        | Cursor::NeResize
        | Cursor::NwResize
        | Cursor::SResize
        | Cursor::SeResize
        | Cursor::SwResize
        | Cursor::WResize
        | Cursor::EwResize
        | Cursor::NsResize
        | Cursor::NeswResize
        | Cursor::NwseResize
        | Cursor::ColResize
        | Cursor::RowResize => ffi::CURSOR_MOVE,
        _ => ffi::CURSOR_ARROW,
    }
}

impl WebViewDelegate for TabState {
    fn notify_load_status_changed(&self, _: WebView, status: LoadStatus) {
        self.load.set(match status {
            LoadStatus::Started => Load::Started,
            LoadStatus::HeadParsed => Load::HeadParsed,
            LoadStatus::Complete => Load::Complete,
        });
        self.load_changed.set(true);
    }

    fn notify_new_frame_ready(&self, _: WebView) {
        self.shared.frame_ready.set(true);
    }

    fn notify_page_title_changed(&self, _: WebView, title: Option<String>) {
        let title = title.unwrap_or_default();
        self.titles.borrow_mut().push(title.clone());
        *self.title.borrow_mut() = title;
        self.title_changed.set(true);
    }

    fn notify_url_changed(&self, _: WebView, url: Url) {
        *self.url.borrow_mut() = Some(url);
        self.url_changed.set(true);
    }

    fn notify_status_text_changed(&self, _: WebView, status: Option<String>) {
        *self.status.borrow_mut() = status.filter(|s| !s.is_empty());
    }

    fn notify_cursor_changed(&self, _: WebView, cursor: Cursor) {
        self.cursor.set(cursor_shape(cursor));
    }

    fn notify_favicon_changed(&self, webview: WebView) {
        let icon = webview.favicon().and_then(|image| favicon_pixels(&image));
        *self.favicon.borrow_mut() = icon;
    }

    fn notify_closed(&self, _: WebView) {
        self.closed.set(true);
    }

    /// A page opens another (a link with a target, `window.open`): it becomes a tab.
    fn request_create_new(&self, _: WebView, request: servo::CreateNewWebViewRequest) {
        let state = TabState::new(self.shared.clone());
        let webview = request
            .builder(self.shared.context.clone())
            .delegate(state.clone())
            .clipboard_delegate(Rc::new(Clipboard))
            .build();
        self.shared.new_tabs.borrow_mut().push(Tab { webview, state, zoom: 1.0 });
    }

    /// Dialogs and pickers wait for the main loop, which is not inside Servo's callback.
    fn show_embedder_control(&self, _: WebView, control: EmbedderControl) {
        let origin = self.url.borrow().as_ref().map(|u| u.host_str().unwrap_or("this page").to_string());
        self.shared
            .controls
            .borrow_mut()
            .push((origin.unwrap_or_else(|| "this page".to_string()), control));
    }
}

/// The size tabs show icons at.
pub const FAVICON_SIZE: i32 = 16;

/// A page's icon, made `FAVICON_SIZE` square (the nearest pixels), as 0xAARRGGBB.
fn favicon_pixels(image: &servo::Image) -> Option<Vec<u32>> {
    use servo::PixelFormat;
    let (width, height) = (image.width as usize, image.height as usize);
    let bytes_per_pixel = match image.format {
        PixelFormat::K8 => 1,
        PixelFormat::KA8 => 2,
        PixelFormat::RGB8 => 3,
        PixelFormat::RGBA8 | PixelFormat::BGRA8 => 4,
    };
    let data = image.data();
    if width == 0 || height == 0 || data.len() < width * height * bytes_per_pixel {
        return None;
    }
    let size = FAVICON_SIZE as usize;
    let mut pixels = Vec::with_capacity(size * size);
    for row in 0..size {
        for column in 0..size {
            let at = ((row * height / size) * width + column * width / size) * bytes_per_pixel;
            let p = &data[at..at + bytes_per_pixel];
            let (r, g, b, a) = match image.format {
                PixelFormat::K8 => (p[0], p[0], p[0], 255),
                PixelFormat::KA8 => (p[0], p[0], p[0], p[1]),
                PixelFormat::RGB8 => (p[0], p[1], p[2], 255),
                PixelFormat::RGBA8 => (p[0], p[1], p[2], p[3]),
                PixelFormat::BGRA8 => (p[2], p[1], p[0], p[3]),
            };
            pixels.push((a as u32) << 24 | (r as u32) << 16 | (g as u32) << 8 | b as u32);
        }
    }
    Some(pixels)
}

/// The desktop's clipboard, for the pages' copy and paste.
pub struct Clipboard;

impl ClipboardDelegate for Clipboard {
    fn get_text(&self, _: WebView, request: StringRequest) {
        request.success(ffi::clipboard_get());
    }

    fn set_text(&self, _: WebView, text: String) {
        ffi::clipboard_set(&text);
    }
}

pub struct Tab {
    pub webview: WebView,
    pub state: Rc<TabState>,
    pub zoom: f32,
}

/// The zoom steps, as browsers have them.
const ZOOMS: [f32; 13] = [0.3, 0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0];

impl Tab {
    pub fn title_or_address(&self) -> String {
        let title = self.state.title.borrow();
        if !title.is_empty() {
            return title.clone();
        }
        match self.state.url.borrow().as_ref() {
            Some(url) if url.scheme() != "about" => url.host_str().map_or_else(|| url.to_string(), str::to_string),
            _ if self.state.loading() => "Loading...".to_string(),
            _ => "New Tab".to_string(),
        }
    }

    /// Zooms to the next step in or out, or back to 100% (`direction` 0).
    pub fn zoom_step(&mut self, direction: i32) {
        let current = ZOOMS
            .iter()
            .position(|z| (*z - self.zoom).abs() < 0.001)
            .unwrap_or(5);
        let next = if direction == 0 {
            5
        } else {
            (current as i32 + direction).clamp(0, ZOOMS.len() as i32 - 1) as usize
        };
        self.zoom = ZOOMS[next];
        self.webview.set_page_zoom(self.zoom);
    }
}
