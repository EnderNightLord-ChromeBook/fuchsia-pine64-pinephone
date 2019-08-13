// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use carnelian::{
    measure_text, Canvas, Color, FontDescription, FontFace, IntSize, MappingPixelSink, Paint,
    PixelSink, Point, Rect, Size,
};
use failure::{bail, Error, ResultExt};
use fuchsia_async as fasync;
use fuchsia_framebuffer::{Config, FrameBuffer, PixelFormat};
use futures::StreamExt;

mod setup;

static FONT_DATA: &'static [u8] =
    include_bytes!("../../../../prebuilt/third_party/fonts/robotoslab/RobotoSlab-Regular.ttf");

struct RecoveryUI<'a, T: PixelSink> {
    face: FontFace<'a>,
    canvas: Canvas<T>,
    config: Config,
    text_size: u32,
}

impl<'a, T: PixelSink> RecoveryUI<'a, T> {
    fn draw(&mut self, heading: &str, body: &str) {
        let r = Rect::new(
            Point::new(0.0, 0.0),
            Size::new(self.config.width as f32, self.config.height as f32),
        );

        let bg = Color { r: 255, g: 0, b: 255, a: 255 };
        self.canvas.fill_rect(&r, bg);

        let mut font_description =
            FontDescription { baseline: 0, face: &mut self.face, size: self.text_size };
        let size = measure_text(heading, &mut font_description);
        let paint = Paint { fg: Color { r: 255, g: 255, b: 255, a: 255 }, bg: bg };
        self.canvas.fill_text(
            heading,
            Point::new(
                (self.config.width / 2) as f32 - (size.width / 2.0),
                (self.config.height / 4) as f32 - (size.height / 2.0),
            ),
            &mut font_description,
            &paint,
        );

        let size = measure_text(body, &mut font_description);

        self.canvas.fill_text(
            body,
            Point::new(
                (self.config.width / 2) as f32 - (size.width / 2.0),
                (self.config.height / 2) as f32 + (self.config.height / 4) as f32
                    - (size.height / 2.0),
            ),
            &mut font_description,
            &paint,
        );
    }
}

async fn run<'a>(ui: &'a mut RecoveryUI<'a, MappingPixelSink>) -> Result<(), Error> {
    ui.draw("Fuchsia System Recovery", "Waiting...");

    let mut receiver = setup::start_server()?;
    while let Some(_event) = receiver.next().await {
        println!("recovery: received request");
        ui.draw("Fuchsia System Recovery", "Got event");
    }

    Ok(())
}

fn main() -> Result<(), Error> {
    println!("recovery: started");
    let mut executor = fasync::Executor::new().context("Failed to create executor")?;
    let fb = FrameBuffer::new(None, &mut executor).context("Failed to create framebuffer")?;
    let config = fb.get_config();
    if config.format != PixelFormat::Argb8888
        && config.format != PixelFormat::Rgb565
        && config.format != PixelFormat::RgbX888
    {
        bail!("Unsupported pixel format {:#?}", config.format);
    }

    let display_size = IntSize::new(config.width as i32, config.height as i32);

    let frame = fb.new_frame(&mut executor)?;
    frame.present(&fb)?;

    let face = FontFace::new(FONT_DATA).unwrap();

    let canvas = Canvas::new(
        display_size,
        MappingPixelSink::new(&frame.mapping),
        config.linear_stride_bytes() as u32,
        config.pixel_size_bytes,
    );

    let mut ui = RecoveryUI { face: face, canvas, config, text_size: config.height / 12 };

    executor.run_singlethreaded(run(&mut ui))?;
    unreachable!();
}

#[cfg(test)]
mod tests {
    use super::{RecoveryUI, FONT_DATA};
    use carnelian::{Canvas, FontFace, IntSize, PixelSink};
    use fuchsia_framebuffer::{Config, PixelFormat};

    struct TestPixelSink {}

    impl PixelSink for TestPixelSink {
        fn write_pixel_at_offset(&mut self, _offset: usize, _value: &[u8]) {}
    }

    #[test]
    fn test_draw() {
        const WIDTH: i32 = 800;
        const HEIGHT: i32 = 600;
        let face = FontFace::new(FONT_DATA).unwrap();
        let sink = TestPixelSink {};
        let config = Config {
            display_id: 0,
            width: WIDTH as u32,
            height: HEIGHT as u32,
            linear_stride_pixels: WIDTH as u32,
            format: PixelFormat::Argb8888,
            pixel_size_bytes: 4,
        };
        let canvas = Canvas::new(
            IntSize::new(WIDTH, HEIGHT),
            sink,
            config.linear_stride_bytes() as u32,
            config.pixel_size_bytes,
        );

        let mut ui = RecoveryUI { face: face, canvas, config, text_size: 24 };
        ui.draw("Heading", "Body");
    }
}
