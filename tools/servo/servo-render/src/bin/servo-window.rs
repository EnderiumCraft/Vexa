//! servo-window: opens a window on the Vexa desktop and shows a page that Servo renders into
//! it. Mouse, wheel and keyboard go to the page, and resizing the window resizes the page.
//!
//!   servo-window URL
//!
//! It runs until the window is closed. Servo paints through the software rendering context,
//! and each frame it finishes is copied into the window's surface and presented. The page's
//! title becomes the window's title, and each change is printed, so a test can read it.

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
/// What one notch of the wheel scrolls the page by.
const PIXELS_PER_NOTCH: f64 = 40.0;

// libvexa's gui.h. Only the parts this program uses are declared.
const WINDOW_RESIZABLE: c_uint = 0x1;
const GUI_KEY: c_int = 1;
const GUI_POINTER: c_int = 2;
const GUI_CLOSE: c_int = 3;
const GUI_FOCUS: c_int = 4;
const GUI_RESIZE: c_int = 5;

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
}

/// What the page tells the program: loads, frames and titles.
#[derive(Default)]
struct Page {
    loaded: Cell<bool>,
    frame_ready: Cell<bool>,
    title: RefCell<Option<String>>,
    title_changed: Cell<bool>,
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
    unsafe { vx_window_present(window, 0, 0, WIDTH as c_int, HEIGHT as c_int) };

    let rendering_context = Rc::new(
        SoftwareRenderingContext::new(PhysicalSize::new(WIDTH, HEIGHT))
            .expect("Could not create the software rendering context"),
    );
    rendering_context
        .make_current()
        .expect("Could not make the rendering context current");

    let servo = ServoBuilder::default()
        .event_loop_waker(Box::new(NoWaker))
        .build();
    servo.setup_logging();

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
                GUI_KEY => key(&webview, &event, &mut modifiers),
                GUI_POINTER => pointer(&webview, &event, &mut buttons),
                GUI_FOCUS => webview.set_focused(event.value != 0),
                GUI_RESIZE => resize(window, &webview, event.width, event.height),
                _ => {},
            }
        }

        if page.title_changed.replace(false) {
            let title = page.title.borrow().clone().unwrap_or_else(|| "Servo".to_string());
            let c_title = CString::new(title.as_str()).unwrap_or_default();
            unsafe { vx_window_set_title(window, c_title.as_ptr()) };
            println!("servo-window: title {title}");
        }
        if page.loaded.get() && !reported_load {
            reported_load = true;
            println!("servo-window: page loaded");
        }
        if page.frame_ready.replace(false) {
            present(window, &rendering_context, &webview);
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

/// Copies the frame Servo has painted into the window's surface, and shows it.
fn present(window: *mut VxWindow, rendering_context: &SoftwareRenderingContext, webview: &WebView) {
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
    let height = (image.height() as i32).min(surface.height);
    for y in 0..height {
        for x in 0..width {
            let [r, g, b, _] = image.get_pixel(x as u32, y as u32).0;
            let color = (r as u32) << 16 | (g as u32) << 8 | b as u32;
            unsafe { *surface.pixels.offset((y * surface.stride + x) as isize) = color };
        }
    }
    unsafe { vx_window_present(window, 0, 0, width, height) };
}

/// The window was resized: the program gets a surface of the new size, and the page is resized
/// to match. `WebView::resize` resizes the rendering context too, on Servo's paint thread.
fn resize(window: *mut VxWindow, webview: &WebView, width: c_int, height: c_int) {
    // A failed resize keeps the old surface, so the page keeps its old size too.
    if unsafe { vx_window_resize(window, width, height) } != 0 {
        println!("servo-window: resize to {width}x{height} failed");
        return;
    }
    println!("servo-window: resized to {width}x{height}");
    webview.resize(PhysicalSize::new(width as u32, height as u32));
}

/// Pointer motion, buttons and wheel. The pointer is in the window's coordinates.
fn pointer(webview: &WebView, event: &VxGuiEvent, buttons: &mut c_int) {
    let point = WebViewPoint::Device(DevicePoint::new(event.x as f32, event.y as f32));
    webview.notify_input_event(InputEvent::MouseMove(MouseMoveEvent::new(point)));

    let changed = event.buttons ^ *buttons;
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
    *buttons = event.buttons;

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

/// A key goes to the page as Servo's keyboard event: a named key (Enter, the arrows...), or the
/// character it types.
fn key(webview: &WebView, event: &VxGuiEvent, modifiers: &mut Modifiers) {
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
