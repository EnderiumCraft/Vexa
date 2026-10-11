//! WebRender on Vexa: a renderer on a surfman context from OSMesa (Mesa's softpipe). It draws
//! one rectangle into the surface's pixels, and the pixels are checked.

use gleam::gl::{self, Gl};
use std::process::ExitCode;
use std::rc::Rc;
use surfman::{
    Connection, ContextAttributeFlags, ContextAttributes, GLVersion, SurfaceAccess, SurfaceType,
};
use webrender::webrender_api::units::{
    DeviceIntRect, DeviceIntSize, FramebufferIntRect, FramebufferIntSize, LayoutRect, LayoutSize,
};
use webrender::webrender_api::{
    ColorF, CommonItemProperties, DisplayListBuilder, DocumentId, Epoch, FramePublishId,
    FrameReadyParams, PipelineId, RenderNotifier, SpaceAndClipInfo,
};
use webrender::webrender_api::RenderReasons;
use webrender::{Transaction, WebRenderOptions};

const WIDTH: i32 = 128;
const HEIGHT: i32 = 128;

struct Notifier;

impl RenderNotifier for Notifier {
    fn clone(&self) -> Box<dyn RenderNotifier> {
        Box::new(Notifier)
    }

    fn wake_up(&self, _composite_needed: bool) {}

    fn new_frame_ready(&self, _: DocumentId, _: FramePublishId, _: &FrameReadyParams) {}
}

fn main() -> ExitCode {
    let connection = Connection::new().expect("connect");
    let adapter = connection.create_adapter().expect("adapter");
    let device = connection.create_device(&adapter).expect("device");
    let attributes = ContextAttributes {
        version: GLVersion::new(3, 2),
        flags: ContextAttributeFlags::DEPTH,
    };
    let descriptor = device.create_context_descriptor(&attributes).expect("descriptor");
    let mut context = device.create_context(&descriptor, None).expect("context");
    let size = euclid::default::Size2D::new(WIDTH, HEIGHT);
    let surface = device
        .create_surface(&context, SurfaceAccess::GPUCPU, SurfaceType::Generic { size })
        .expect("surface");
    device
        .bind_surface_to_context(&mut context, surface)
        .map_err(|(error, _)| error)
        .expect("bind");
    device.make_context_current(&context).expect("make current");

    let gl: Rc<dyn Gl> = unsafe { gl::GlFns::load_with(|name| device.get_proc_address(&context, name)) };
    let version = gl.get_string(gl::VERSION);
    println!("webrender-test: OpenGL {version}");

    let (mut renderer, sender) = webrender::create_webrender_instance(
        gl.clone(),
        Box::new(Notifier),
        WebRenderOptions::default(),
        None,
    )
    .expect("create WebRender");
    println!("webrender-test: renderer created");

    let mut api = sender.create_api();
    let frame_size = DeviceIntSize::new(WIDTH, HEIGHT);
    let document_id = api.add_document(frame_size);
    let pipeline_id = PipelineId(0, 0);
    let layout_size = LayoutSize::new(WIDTH as f32, HEIGHT as f32);
    let bounds = LayoutRect::from_size(layout_size);

    let mut builder = DisplayListBuilder::new(pipeline_id);
    builder.begin();
    let space_and_clip = SpaceAndClipInfo::root_scroll(pipeline_id);
    let common = CommonItemProperties::new(bounds, space_and_clip);
    builder.push_rect(&common, bounds, ColorF::new(1.0, 0.5, 0.0, 1.0));
    let display_list = builder.end();

    let mut transaction = Transaction::new();
    transaction.set_root_pipeline(pipeline_id);
    transaction.set_display_list(Epoch(0), display_list);
    transaction.set_document_view(DeviceIntRect::from_size(frame_size));
    transaction.generate_frame(0, true, false, RenderReasons::empty());
    api.send_transaction(document_id, transaction);
    api.flush_scene_builder();
    renderer.update();

    match renderer.render(frame_size, 0) {
        Ok(_) => println!("webrender-test: frame rendered"),
        Err(errors) => {
            println!("webrender-test: render failed: {errors:?}");
            return ExitCode::from(1);
        }
    }
    let pixels = renderer.read_pixels_rgba8(FramebufferIntRect::from_size(FramebufferIntSize::new(WIDTH, HEIGHT)));
    let middle = ((HEIGHT as usize / 2) * WIDTH as usize + WIDTH as usize / 2) * 4;
    let (r, g, b) = (pixels[middle], pixels[middle + 1], pixels[middle + 2]);
    println!("webrender-test: middle {r},{g},{b}");
    let right = (r as i32 - 255).abs() <= 2 && (g as i32 - 128).abs() <= 2 && b <= 2;

    renderer.deinit();
    if right {
        println!("webrender-test: passed");
        ExitCode::SUCCESS
    } else {
        println!("webrender-test: FAIL");
        ExitCode::from(1)
    }
}
