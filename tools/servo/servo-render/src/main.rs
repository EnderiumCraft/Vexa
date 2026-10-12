//! servo-render: loads a local HTML page in Servo on Vexa, paints one frame through the
//! software rendering context, writes it to a PNG and checks the pixels.
//!
//!   servo-render URL OUT.png [--render-test]
//!
//! It prints the page's title and writes the frame to OUT.png. With --render-test, the page is
//! tests/servo/render-test.html (orange, with a blue square in the top-left corner), and the
//! program checks the pixels and prints "servo-render: passed" when they are right.

use std::cell::{Cell, RefCell};
use std::error::Error;
use std::rc::Rc;
use std::time::{Duration, Instant};

use dpi::PhysicalSize;
use servo::{
    DeviceIntPoint, DeviceIntRect, EventLoopWaker, LoadStatus, RenderingContext,
    ServoBuilder, SoftwareRenderingContext, WebView, WebViewBuilder, WebViewDelegate,
};
use servo::resources::ResourceReaderMethods;
use servo_default_resources::DefaultResourceReader;
use url::Url;

const WIDTH: u32 = 800;
const HEIGHT: u32 = 600;
const ORANGE: [u8; 3] = [255, 128, 0];
const BLUE: [u8; 3] = [0, 0, 255];

struct NoWaker;

impl EventLoopWaker for NoWaker {
    fn clone_box(&self) -> Box<dyn EventLoopWaker> {
        Box::new(NoWaker)
    }

    fn wake(&self) {}
}

/// Tracks the page's load. A frame counts only once it is painted after the load completes.
#[derive(Default)]
struct Page {
    loaded: Cell<bool>,
    frame_ready: Cell<bool>,
    title: RefCell<Option<String>>,
}

impl WebViewDelegate for Page {
    fn notify_load_status_changed(&self, _: WebView, status: LoadStatus) {
        if matches!(status, LoadStatus::Complete) {
            self.loaded.set(true);
            self.frame_ready.set(false);
        }
    }

    fn notify_new_frame_ready(&self, _: WebView) {
        self.frame_ready.set(true);
    }

    fn notify_page_title_changed(&self, _: WebView, title: Option<String>) {
        *self.title.borrow_mut() = title;
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().collect();
    let render_test = args.len() == 4 && args[3] == "--render-test";
    if !(args.len() == 3 || render_test) {
        eprintln!("usage: servo-render URL OUT.png [--render-test]");
        std::process::exit(2);
    }
    let url = Url::parse(&args[1])?;

    // The default resources (the user-agent stylesheets and the like) register through
    // inventory. Taking the reader as a trait object keeps the linker from dropping them.
    let _resources: &dyn ResourceReaderMethods = &DefaultResourceReader;

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
        .url(url.clone())
        .delegate(page.clone())
        .build();

    // Spin Servo until the page has loaded and a frame has been painted after that.
    let deadline = Instant::now() + Duration::from_secs(60);
    while !(page.loaded.get() && page.frame_ready.get()) {
        servo.spin_event_loop();
        if Instant::now() > deadline {
            eprintln!(
                "servo-render: timed out (loaded: {}, frame ready: {})",
                page.loaded.get(),
                page.frame_ready.get()
            );
            std::process::exit(1);
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    // Let the compositor finish the frame before reading it back.
    let settle = Instant::now() + Duration::from_millis(500);
    while Instant::now() < settle {
        servo.spin_event_loop();
        std::thread::sleep(Duration::from_millis(5));
    }

    println!(
        "servo-render: title {}",
        page.title.borrow().as_deref().unwrap_or("")
    );
    webview.paint();
    let rect = DeviceIntRect::new(
        DeviceIntPoint::new(0, 0),
        DeviceIntPoint::new(WIDTH as i32, HEIGHT as i32),
    );
    let image = rendering_context
        .read_to_image(rect)
        .ok_or("could not read the frame back")?;
    image.save(&args[2])?;

    let centre = image.get_pixel(WIDTH / 2, HEIGHT / 2).0;
    let corner = image.get_pixel(10, 10).0;
    let centre = [centre[0], centre[1], centre[2]];
    let corner = [corner[0], corner[1], corner[2]];
    println!(
        "servo-render: {url} rendered {WIDTH}x{HEIGHT}, centre {centre:?}, corner {corner:?}, written to {}",
        args[2]
    );
    if !render_test {
        return Ok(());
    }
    if centre != ORANGE || corner != BLUE {
        println!("servo-render: pixels are wrong, expected centre {ORANGE:?} and corner {BLUE:?}");
        std::process::exit(1);
    }
    println!("servo-render: passed");
    Ok(())
}
