//! servo-window: opens a window on the Vexa desktop with an address bar, and a page that Servo
//! renders below the bar. Mouse, wheel and keyboard go to the page, and resizing the window
//! resizes the page.
//!
//!   servo-window URL
//!
//! The address bar shows the page's address. Click it, or press Ctrl+L, and type another: Enter
//! loads it (an address without a scheme is https), and Escape or a click on the page gives the
//! keyboard back to the page. It runs until the window is closed. Servo paints through the
//! software rendering context, and each frame it finishes is copied into the window's surface
//! and presented. The page's title becomes the window's title, and each change is printed, so a
//! test can read it.

use std::cell::{Cell, RefCell};
use std::error::Error;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_long, c_uint};
use std::rc::Rc;

use dpi::PhysicalSize;
use servo::resources::ResourceReaderMethods;
use servo::{
    Code, DeviceIntPoint, DeviceIntRect, DevicePoint, EventLoopWaker, InputEvent, Key, KeyState,
    KeyboardEvent, LoadStatus, Location, Modifiers, MouseButton, MouseButtonAction,
    MouseButtonEvent, MouseMoveEvent, NamedKey, RenderingContext, ServoBuilder,
    SoftwareRenderingContext, WebView, WebViewBuilder, WebViewDelegate, WebViewPoint, WheelDelta,
    WheelEvent, WheelMode,
};
use servo_default_resources::DefaultResourceReader;
use url::Url;

const WIDTH: u32 = 800;
const HEIGHT: u32 = 600;
/// The address bar, above the page: libvexa's toolbar, with its field 8 pixels down.
const BAR_HEIGHT: u32 = 40;
/// What one notch of the wheel scrolls the page by.
const PIXELS_PER_NOTCH: f64 = 40.0;

// libvexa's gui.h. Only the parts this program uses are declared.
const WINDOW_RESIZABLE: c_uint = 0x1;
const GUI_KEY: c_int = 1;
const GUI_POINTER: c_int = 2;
const GUI_CLOSE: c_int = 3;
const GUI_FOCUS: c_int = 4;
const GUI_RESIZE: c_int = 5;

// Linux keyboard codes (Vexa's abi.h).
const KEY_ESCAPE: c_int = 1;
const KEY_BACKSPACE: c_int = 14;
const KEY_ENTER: c_int = 28;
const KEY_L: c_int = 38;

#[repr(C)]
struct VxSurface {
    pixels: *mut u32,
    width: c_int,
    height: c_int,
    stride: c_int,
}

/// The start of libvexa's struct vx_window. The program draws on `surface`; the rest of the
/// struct (the buffers and the desktop's handles) is libvexa's, and this never reads it.
#[repr(C)]
struct VxWindow {
    id: c_int,
    surface: VxSurface,
}

/// libvexa's struct vx_gui_event, field for field.
#[repr(C)]
struct VxGuiEvent {
    kind: c_int,
    window: c_int,
    x: c_int,
    y: c_int,
    buttons: c_int,
    wheel: c_int,
    key: c_int,
    value: c_int,
    character: c_int,
    width: c_int,
    height: c_int,
    text: [c_char; 104],
}

extern "C" {
    fn vx_window_create_flags(
        title: *const c_char,
        width: c_int,
        height: c_int,
        flags: c_uint,
    ) -> *mut VxWindow;
    fn vx_window_center(window: *mut VxWindow);
    fn vx_window_resize(window: *mut VxWindow, width: c_int, height: c_int) -> c_int;
    fn vx_window_present(window: *mut VxWindow, x: c_int, y: c_int, width: c_int, height: c_int);
    fn vx_window_set_title(window: *mut VxWindow, title: *const c_char);
    fn vx_window_destroy(window: *mut VxWindow);
    fn vx_gui_wait(event: *mut VxGuiEvent, timeout_ms: c_long) -> c_int;
    fn vx_draw_toolbar(surface: *mut VxSurface, x: c_int, y: c_int, width: c_int, height: c_int);
    fn vx_draw_field(
        surface: *mut VxSurface,
        x: c_int,
        y: c_int,
        width: c_int,
        text: *const c_char,
        focused: bool,
    );
}

/// What the page tells the program: loads, frames, titles and addresses.
#[derive(Default)]
struct Page {
    loaded: Cell<bool>,
    frame_ready: Cell<bool>,
    title: RefCell<Option<String>>,
    title_changed: Cell<bool>,
    url: RefCell<Option<Url>>,
    url_changed: Cell<bool>,
}

impl WebViewDelegate for Page {
    fn notify_load_status_changed(&self, _: WebView, status: LoadStatus) {
        if matches!(status, LoadStatus::Complete) {
            self.loaded.set(true);
        }
    }

    fn notify_new_frame_ready(&self, _: WebView) {
        self.frame_ready.set(true);
    }

    fn notify_page_title_changed(&self, _: WebView, title: Option<String>) {
        *self.title.borrow_mut() = title;
        self.title_changed.set(true);
    }

    fn notify_url_changed(&self, _: WebView, url: Url) {
        *self.url.borrow_mut() = Some(url);
        self.url_changed.set(true);
    }
}

/// The address bar: the address the page shows, or the one being typed.
#[derive(Default)]
struct Bar {
    text: String,
    focused: bool,
    /// The first character typed after the bar takes the focus replaces the address.
    replace_on_type: bool,
}

impl Bar {
    fn focus(&mut self, webview: &WebView) {
        self.focused = true;
        self.replace_on_type = true;
        webview.set_focused(false);
        println!("servo-window: address focused");
    }

    fn unfocus(&mut self, webview: &WebView) {
        self.focused = false;
        self.replace_on_type = false;
        webview.set_focused(true);
    }

    /// Makes the first typed character replace the address, once.
    fn clear_for_typing(&mut self) {
        if self.replace_on_type {
            self.text.clear();
            self.replace_on_type = false;
        }
    }
}

struct NoWaker;

impl EventLoopWaker for NoWaker {
    fn clone_box(&self) -> Box<dyn EventLoopWaker> {
        Box::new(NoWaker)
    }

    fn wake(&self) {}
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: servo-window URL");
        std::process::exit(2);
    }
    let url = Url::parse(&args[1])?;
    // The default resources register through inventory. Taking the reader as a trait object
    // keeps the linker from dropping them.
    let _resources: &dyn ResourceReaderMethods = &DefaultResourceReader;

    let title = CString::new("Servo")?;
    let window = unsafe {
        vx_window_create_flags(title.as_ptr(), WIDTH as c_int, HEIGHT as c_int, WINDOW_RESIZABLE)
    };
    if window.is_null() {
        eprintln!("servo-window: no desktop to open a window on");
        std::process::exit(1);
    }
    unsafe { vx_window_center(window) };
    fill(window, 0xffffff);

    let rendering_context = Rc::new(
        SoftwareRenderingContext::new(PhysicalSize::new(WIDTH, HEIGHT - BAR_HEIGHT))
            .expect("Could not create the software rendering context"),
    );
    rendering_context
        .make_current()
        .expect("Could not make the rendering context current");

    let servo = ServoBuilder::default()
        .event_loop_waker(Box::new(NoWaker))
        .build();
    servo.setup_logging();

    let mut bar = Bar {
        text: url.to_string(),
        ..Bar::default()
    };
    paint_bar(window, &bar);
    unsafe { vx_window_present(window, 0, 0, WIDTH as c_int, HEIGHT as c_int) };

    let page = Rc::new(Page::default());
    let webview = WebViewBuilder::new(&servo, rendering_context.clone())
        .url(url)
        .delegate(page.clone())
        .build();
    webview.set_focused(true);

    let mut buttons = 0;
    let mut modifiers = Modifiers::empty();
    let mut reported_load = false;
    loop {
        servo.spin_event_loop();

        // Events come in every few milliseconds, and Servo gets a turn between them.
        let mut event: VxGuiEvent = unsafe { std::mem::zeroed() };
        let waited = unsafe { vx_gui_wait(&mut event, 10) };
        if waited < 0 {
            // The desktop is gone.
            break;
        }
        if waited > 0 {
            match event.kind {
                GUI_CLOSE => break,
                GUI_KEY => key(window, &webview, &mut bar, &event, &mut modifiers),
                GUI_POINTER => pointer(window, &webview, &mut bar, &event, &mut buttons),
                GUI_FOCUS => webview.set_focused(event.value != 0 && !bar.focused),
                GUI_RESIZE => resize(window, &webview, &bar, event.width, event.height),
                _ => {},
            }
        }

        if page.title_changed.replace(false) {
            let title = page.title.borrow().clone().unwrap_or_else(|| "Servo".to_string());
            let c_title = CString::new(title.as_str()).unwrap_or_default();
            unsafe { vx_window_set_title(window, c_title.as_ptr()) };
            println!("servo-window: title {title}");
        }
        if page.url_changed.replace(false) && !bar.focused {
            if let Some(url) = page.url.borrow().clone() {
                bar.text = url.to_string();
                show_bar(window, &bar);
            }
        }
        if page.loaded.get() && !reported_load {
            reported_load = true;
            println!("servo-window: page loaded");
        }
        if page.frame_ready.replace(false) {
            present(window, &rendering_context, &webview, &bar);
        }
    }

    unsafe { vx_window_destroy(window) };
    Ok(())
}

/// Fills the window's surface with a color (0xRRGGBB).
fn fill(window: *mut VxWindow, color: u32) {
    let surface = unsafe { &(*window).surface };
    for y in 0..surface.height {
        for x in 0..surface.width {
            unsafe { *surface.pixels.offset((y * surface.stride + x) as isize) = color };
        }
    }
}

/// Draws the address bar into the top rows of the window's surface.
fn paint_bar(window: *mut VxWindow, bar: &Bar) {
    let surface = unsafe { &mut (*window).surface as *mut VxSurface };
    let width = unsafe { (*surface).width };
    let text = CString::new(bar.text.replace('\0', "")).unwrap_or_default();
    unsafe {
        vx_draw_toolbar(surface, 0, 0, width, BAR_HEIGHT as c_int);
        vx_draw_field(surface, 8, 8, width - 16, text.as_ptr(), bar.focused);
    }
}

/// Redraws and shows just the address bar, when it changes.
fn show_bar(window: *mut VxWindow, bar: &Bar) {
    paint_bar(window, bar);
    let width = unsafe { (*window).surface.width };
    unsafe { vx_window_present(window, 0, 0, width, BAR_HEIGHT as c_int) };
}

/// Copies the frame Servo has painted into the window's surface below the bar, and shows it.
fn present(
    window: *mut VxWindow,
    rendering_context: &SoftwareRenderingContext,
    webview: &WebView,
    bar: &Bar,
) {
    webview.paint();
    let size = rendering_context.size();
    let rect = DeviceIntRect::new(
        DeviceIntPoint::new(0, 0),
        DeviceIntPoint::new(size.width as i32, size.height as i32),
    );
    let Some(image) = rendering_context.read_to_image(rect) else {
        return;
    };
    let surface = unsafe { &(*window).surface };
    let width = (image.width() as i32).min(surface.width);
    let height = (image.height() as i32).min(surface.height - BAR_HEIGHT as i32);
    for y in 0..height {
        for x in 0..width {
            let [r, g, b, _] = image.get_pixel(x as u32, y as u32).0;
            let color = (r as u32) << 16 | (g as u32) << 8 | b as u32;
            let row = y + BAR_HEIGHT as i32;
            unsafe { *surface.pixels.offset((row * surface.stride + x) as isize) = color };
        }
    }
    paint_bar(window, bar);
    unsafe { vx_window_present(window, 0, 0, surface.width, surface.height) };
}

/// The window was resized: the program gets a surface of the new size, and the page is resized
/// to the rest of it, below the bar. `WebView::resize` resizes the rendering context too, on
/// Servo's paint thread.
fn resize(window: *mut VxWindow, webview: &WebView, bar: &Bar, width: c_int, height: c_int) {
    // A failed resize keeps the old surface, so the page keeps its old size too.
    if unsafe { vx_window_resize(window, width, height) } != 0 {
        println!("servo-window: resize to {width}x{height} failed");
        return;
    }
    println!("servo-window: resized to {width}x{height}");
    show_bar(window, bar);
    let page_height = (height as u32).saturating_sub(BAR_HEIGHT).max(1);
    webview.resize(PhysicalSize::new(width as u32, page_height));
}

/// Pointer motion, buttons and wheel. The pointer is in the window's coordinates: the bar is at
/// the top, and the page's coordinates start below it. A press in the bar takes the keyboard for
/// the bar, and a press on the page gives it back to the page.
fn pointer(
    window: *mut VxWindow,
    webview: &WebView,
    bar: &mut Bar,
    event: &VxGuiEvent,
    buttons: &mut c_int,
) {
    let changed = event.buttons ^ *buttons;
    *buttons = event.buttons;
    let pressed = changed & event.buttons != 0;
    if event.y < BAR_HEIGHT as c_int {
        if pressed {
            bar.focus(webview);
            show_bar(window, bar);
        }
        return;
    }
    if pressed && bar.focused {
        bar.unfocus(webview);
        show_bar(window, bar);
    }

    let point = WebViewPoint::Device(DevicePoint::new(
        event.x as f32,
        (event.y - BAR_HEIGHT as c_int) as f32,
    ));
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

    // Servo takes wheel deltas as winit does, where a positive y turns the page up (the DOM's
    // deltaY is the negative of it). The wheel is positive when it turns up, too.
    if event.wheel != 0 {
        println!("servo-window: wheel {}", event.wheel);
        let delta = WheelDelta {
            x: 0.0,
            y: (event.wheel as f64) * PIXELS_PER_NOTCH,
            z: 0.0,
            mode: WheelMode::DeltaPixel,
        };
        webview.notify_input_event(InputEvent::Wheel(WheelEvent::new(delta, point)));
    }
}

/// A key goes to the address bar while it has the focus, and to the page otherwise. Ctrl+L
/// gives the bar the focus.
fn key(
    window: *mut VxWindow,
    webview: &WebView,
    bar: &mut Bar,
    event: &VxGuiEvent,
    modifiers: &mut Modifiers,
) {
    let state = if event.value == 0 {
        KeyState::Up
    } else {
        KeyState::Down
    };
    let modifier = match event.key {
        42 | 54 => Modifiers::SHIFT,
        29 | 97 => Modifiers::CONTROL,
        56 | 100 => Modifiers::ALT,
        125 => Modifiers::META,
        _ => Modifiers::empty(),
    };
    if matches!(state, KeyState::Down) {
        modifiers.insert(modifier);
    } else {
        modifiers.remove(modifier);
    }
    let pressed = matches!(state, KeyState::Down);

    if pressed && event.key == KEY_L && modifiers.contains(Modifiers::CONTROL) {
        bar.focus(webview);
        show_bar(window, bar);
        return;
    }
    if bar.focused {
        if pressed {
            bar_key(webview, bar, event);
            show_bar(window, bar);
        }
        return;
    }

    let key = named_key(event.key)
        .map(Key::Named)
        .or_else(|| {
            char::from_u32(event.character as u32)
                .filter(|c| !c.is_control())
                .map(|c| Key::Character(c.to_string()))
        });
    let Some(key) = key else {
        return;
    };
    webview.notify_input_event(InputEvent::Keyboard(KeyboardEvent::new_without_event(
        state,
        key,
        Code::Unidentified,
        Location::Standard,
        *modifiers,
        event.value == 2,
        false,
    )));
}

/// A key pressed while the address bar has the focus: typing edits the address, Enter loads it,
/// and Escape gives the keyboard back to the page.
fn bar_key(webview: &WebView, bar: &mut Bar, event: &VxGuiEvent) {
    match event.key {
        KEY_ESCAPE => bar.unfocus(webview),
        KEY_BACKSPACE => {
            if bar.replace_on_type {
                bar.clear_for_typing();
            } else {
                bar.text.pop();
            }
        },
        KEY_ENTER => navigate(webview, bar),
        _ => {
            if let Some(c) = char::from_u32(event.character as u32).filter(|c| !c.is_control()) {
                bar.clear_for_typing();
                bar.text.push(c);
            }
        },
    }
}

/// Loads the address the bar holds. An address without a scheme is https.
fn navigate(webview: &WebView, bar: &mut Bar) {
    let text = bar.text.trim().to_string();
    let parsed = Url::parse(&text).or_else(|_| Url::parse(&format!("https://{text}")));
    match parsed {
        Ok(url) => {
            println!("servo-window: navigate {url}");
            webview.load(url);
            bar.unfocus(webview);
        },
        Err(error) => println!("servo-window: bad address {text}: {error}"),
    }
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
