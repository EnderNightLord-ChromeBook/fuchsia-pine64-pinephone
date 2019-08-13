// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(dead_code)]

use failure::{format_err, Error, ResultExt};
use fdio::watch_directory;
use fidl::endpoints;
use fidl_fuchsia_hardware_display::{
    ControllerEvent, ControllerMarker, ControllerProxy, ImageConfig, ImagePlane,
    ProviderSynchronousProxy,
};
use fuchsia_async as fasync;
use fuchsia_zircon::{
    self as zx,
    sys::{zx_cache_policy_t::ZX_CACHE_POLICY_WRITE_COMBINING, ZX_TIME_INFINITE},
    HandleBased, Rights, Vmo,
};
use futures::{future, StreamExt, TryFutureExt, TryStreamExt};
use mapped_vmo::Mapping;
use std::cell::RefCell;
use std::fs::OpenOptions;
use std::rc::Rc;
use std::{sync::Arc, thread, time};

#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_NONE: u32 = 0;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_565: u32 = 131073;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_332: u32 = 65538;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_2220: u32 = 65539;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_ARGB_8888: u32 = 262148;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_RGB_x888: u32 = 262149;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_MONO_8: u32 = 65543;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_GRAY_8: u32 = 65543;
#[allow(non_camel_case_types, non_upper_case_globals)]
const ZX_PIXEL_FORMAT_MONO_1: u32 = 6;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PixelFormat {
    Argb8888,
    Gray8,
    Mono1,
    Mono8,
    Rgb2220,
    Rgb332,
    Rgb565,
    RgbX888,
    Unknown,
}

impl Default for PixelFormat {
    fn default() -> PixelFormat {
        PixelFormat::Unknown
    }
}

impl From<u32> for PixelFormat {
    fn from(pixel_format: u32) -> Self {
        #[allow(non_upper_case_globals)]
        match pixel_format {
            ZX_PIXEL_FORMAT_ARGB_8888 => PixelFormat::Argb8888,
            ZX_PIXEL_FORMAT_MONO_1 => PixelFormat::Mono1,
            ZX_PIXEL_FORMAT_MONO_8 => PixelFormat::Mono8,
            ZX_PIXEL_FORMAT_RGB_2220 => PixelFormat::Rgb2220,
            ZX_PIXEL_FORMAT_RGB_332 => PixelFormat::Rgb332,
            ZX_PIXEL_FORMAT_RGB_565 => PixelFormat::Rgb565,
            ZX_PIXEL_FORMAT_RGB_x888 => PixelFormat::RgbX888,
            // ZX_PIXEL_FORMAT_GRAY_8 is an alias for ZX_PIXEL_FORMAT_MONO_8
            ZX_PIXEL_FORMAT_NONE => PixelFormat::Unknown,
            _ => PixelFormat::Unknown,
        }
    }
}

impl Into<u32> for PixelFormat {
    fn into(self) -> u32 {
        match self {
            PixelFormat::Argb8888 => ZX_PIXEL_FORMAT_ARGB_8888,
            PixelFormat::Mono1 => ZX_PIXEL_FORMAT_MONO_1,
            PixelFormat::Mono8 => ZX_PIXEL_FORMAT_MONO_8,
            PixelFormat::Rgb2220 => ZX_PIXEL_FORMAT_RGB_2220,
            PixelFormat::Rgb332 => ZX_PIXEL_FORMAT_RGB_332,
            PixelFormat::Rgb565 => ZX_PIXEL_FORMAT_RGB_565,
            PixelFormat::RgbX888 => ZX_PIXEL_FORMAT_RGB_x888,
            PixelFormat::Gray8 => ZX_PIXEL_FORMAT_GRAY_8,
            PixelFormat::Unknown => ZX_PIXEL_FORMAT_NONE,
        }
    }
}

fn pixel_format_bytes(pixel_format: u32) -> usize {
    ((pixel_format >> 16) & 7) as usize
}

#[derive(Debug, Clone, Copy, Default)]
pub struct Config {
    pub display_id: u64,
    pub width: u32,
    pub height: u32,
    pub linear_stride_pixels: u32,
    pub format: PixelFormat,
    pub pixel_size_bytes: u32,
}

impl Config {
    pub fn linear_stride_bytes(&self) -> usize {
        self.linear_stride_pixels as usize * self.pixel_size_bytes as usize
    }
}

pub struct Frame {
    config: Config,
    vmo: Vmo,
    image_id: u64,
    pub mapping: Arc<Mapping>,
}

impl Frame {
    fn allocate_image_vmo(
        framebuffer: &FrameBuffer,
        executor: &mut fasync::Executor,
    ) -> Result<Vmo, Error> {
        let vmo: Rc<RefCell<Option<Vmo>>> = Rc::new(RefCell::new(None));
        let vmo_response = framebuffer
            .controller
            .allocate_vmo(framebuffer.byte_size() as u64)
            .map_ok(|(status, allocated_vmo)| {
                if status == zx::sys::ZX_OK {
                    vmo.replace(allocated_vmo);
                }
            });
        executor
            .run_singlethreaded(vmo_response)
            .context("allocate_image_vmo - run_singlethreaded")?;
        let vmo = vmo.replace(None);
        if let Some(vmo) = vmo {
            Ok(vmo)
        } else {
            Err(format_err!("Could not allocate image vmo"))
        }
    }

    fn import_image_vmo(
        framebuffer: &FrameBuffer,
        executor: &mut fasync::Executor,
        image_vmo: Vmo,
    ) -> Result<u64, Error> {
        let pixel_format: u32 = framebuffer.config.format.into();
        let plane = ImagePlane {
            byte_offset: 0,
            bytes_per_row: framebuffer.config.linear_stride_bytes() as u32,
        };
        let mut image_config = ImageConfig {
            width: framebuffer.config.width,
            height: framebuffer.config.height,
            pixel_format: pixel_format as u32,
            type_: 0,
            planes: [
                plane,
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ImagePlane { byte_offset: 0, bytes_per_row: 0 },
            ],
        };

        let image_id: Rc<RefCell<Option<u64>>> = Rc::new(RefCell::new(None));
        let import_response = framebuffer
            .controller
            .import_vmo_image(&mut image_config, image_vmo, 0)
            .map_ok(|(status, id)| {
                if status == zx::sys::ZX_OK {
                    image_id.replace(Some(id));
                }
            });

        executor
            .run_singlethreaded(import_response)
            .context("import_image_vmo - run_singlethreaded")?;

        let image_id = image_id.replace(None);
        if let Some(image_id) = image_id {
            Ok(image_id)
        } else {
            Err(format_err!("Could not import image vmo"))
        }
    }

    pub fn new(framebuffer: &FrameBuffer, executor: &mut fasync::Executor) -> Result<Frame, Error> {
        let image_vmo = Self::allocate_image_vmo(framebuffer, executor)
            .context("Frame::new() allocate_image_vmo")?;
        image_vmo
            .set_cache_policy(ZX_CACHE_POLICY_WRITE_COMBINING)
            .unwrap_or_else(|_err| println!("set_cache_policy failed"));

        let mapping = Mapping::create_from_vmo(
            &image_vmo,
            framebuffer.byte_size(),
            zx::VmarFlags::PERM_READ
                | zx::VmarFlags::PERM_WRITE
                | zx::VmarFlags::MAP_RANGE
                | zx::VmarFlags::REQUIRE_NON_RESIZABLE,
        )
        .context("Frame::new() Mapping::create_from_vmo failed")?;

        // import image VMO
        let imported_image_vmo = image_vmo.duplicate_handle(Rights::SAME_RIGHTS)?;
        let image_id = Self::import_image_vmo(framebuffer, executor, imported_image_vmo)
            .context("Frame::new() import_image_vmo")?;

        Ok(Frame {
            config: framebuffer.get_config(),
            image_id: image_id,
            vmo: image_vmo,
            mapping: Arc::new(mapping),
        })
    }

    pub fn write_pixel(&mut self, x: u32, y: u32, value: &[u8]) {
        if x < self.config.width && y < self.config.height {
            let pixel_size = self.config.pixel_size_bytes as usize;
            let offset = self.linear_stride_bytes() * y as usize + x as usize * pixel_size;
            self.write_pixel_at_offset(offset, value);
        }
    }

    pub fn write_pixel_at_offset(&mut self, offset: usize, value: &[u8]) {
        self.mapping.write_at(offset, value);
    }

    pub fn fill_rectangle(&mut self, x: u32, y: u32, width: u32, height: u32, value: &[u8]) {
        let left = x.min(self.config.width);
        let right = (left + width).min(self.config.width);
        let top = y.min(self.config.height);
        let bottom = (top + height).min(self.config.width);
        for j in top..bottom {
            for i in left..right {
                self.write_pixel(i, j, value);
            }
        }
    }

    pub fn present(&self, framebuffer: &FrameBuffer) -> Result<(), Error> {
        framebuffer
            .controller
            .set_layer_image(framebuffer.layer_id, self.image_id, 0, 0)
            .context("Frame::present() set_layer_image")?;
        framebuffer.controller.apply_config().context("Frame::present() apply_config")?;
        Ok(())
    }

    fn byte_size(&self) -> usize {
        self.linear_stride_bytes() * self.config.height as usize
    }

    pub fn linear_stride_bytes(&self) -> usize {
        self.config.linear_stride_pixels as usize * self.config.pixel_size_bytes as usize
    }

    pub fn pixel_size_bytes(&self) -> usize {
        self.config.pixel_size_bytes as usize
    }
}

pub struct FrameBuffer {
    display_controller: zx::Channel,
    controller: ControllerProxy,
    config: Config,
    layer_id: u64,
}

impl FrameBuffer {
    fn create_config_from_event_stream(
        proxy: &ControllerProxy,
        executor: &mut fasync::Executor,
    ) -> Result<Config, Error> {
        let display_info: Rc<RefCell<Option<(u64, u32, u32, u32)>>> = Rc::new(RefCell::new(None));
        let stream = proxy.take_event_stream();
        let mut event_listener = stream.filter(|event| {
            if let Ok(ControllerEvent::DisplaysChanged { added, .. }) = event {
                if added.len() > 0 {
                    let first_added = &added[0];
                    if first_added.pixel_format.len() > 0 && first_added.modes.len() > 0 {
                        let display_id = first_added.id;
                        let pixel_format = first_added.pixel_format[0];
                        let width = first_added.modes[0].horizontal_resolution;
                        let height = first_added.modes[0].vertical_resolution;
                        display_info.replace(Some((display_id, pixel_format, width, height)));
                    }
                }
            }
            future::ready(true)
        });

        // run for two seconds waiting for a ControllerEvent::DisplaysChanged event,
        // needed since other events may come in first
        let delay = time::Duration::from_millis(100);
        let timeout = time::Duration::from_millis(2000);
        let now = time::Instant::now();
        while display_info.borrow().is_none() && now.elapsed() < timeout {
            executor.run_singlethreaded(event_listener.try_next())?;
            thread::sleep(delay);
        }

        let display_info = display_info.replace(None);
        if let Some((display_id, pixel_format, width, height)) = display_info {
            let stride: Rc<RefCell<u32>> = Rc::new(RefCell::new(0));
            let stride_response =
                proxy.compute_linear_image_stride(width, pixel_format).map_ok(|px_stride| {
                    stride.replace(px_stride);
                });

            executor.run_singlethreaded(stride_response).context("")?;

            let stride = stride.replace(0);
            if 0 == stride {
                Err(format_err!("Could not calculate stride"))
            } else {
                Ok(Config {
                    display_id: display_id,
                    width: width,
                    height: height,
                    linear_stride_pixels: stride,
                    format: pixel_format.into(),
                    pixel_size_bytes: pixel_format_bytes(pixel_format) as u32,
                })
            }
        } else {
            Err(format_err!("Could not find display"))
        }
    }

    fn configure_layer(
        config: Config,
        proxy: &ControllerProxy,
        executor: &mut fasync::Executor,
    ) -> Result<u64, Error> {
        let layer_id: Rc<RefCell<Option<u64>>> = Rc::new(RefCell::new(None));
        let layer_id_response = proxy.create_layer().map_ok(|(status, id)| {
            if status == zx::sys::ZX_OK {
                layer_id.replace(Some(id));
            }
        });

        executor.run_singlethreaded(layer_id_response)?;
        let layer_id = layer_id.replace(None);
        if let Some(id) = layer_id {
            let pixel_format: u32 = config.format.into();
            let plane =
                ImagePlane { byte_offset: 0, bytes_per_row: config.linear_stride_bytes() as u32 };
            let mut image_config = ImageConfig {
                width: config.width,
                height: config.height,
                pixel_format: pixel_format as u32,
                type_: 0,
                planes: [
                    plane,
                    ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                    ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                    ImagePlane { byte_offset: 0, bytes_per_row: 0 },
                ],
            };
            proxy.set_layer_primary_config(id, &mut image_config)?;

            let mut layers = std::iter::once(id);
            proxy.set_display_layers(config.display_id, &mut layers)?;
            Ok(id)
        } else {
            Err(format_err!("Failed to create layer"))
        }
    }

    pub fn new(
        display_index: Option<usize>,
        executor: &mut fasync::Executor,
    ) -> Result<FrameBuffer, Error> {
        let device_path = if let Some(index) = display_index {
            format!("/dev/class/display-controller/{:03}", index)
        } else {
            // If the caller did not supply a display index, we watch the
            // display-controller and use the first display that appears.
            let mut first_path = None;
            let dir = OpenOptions::new().read(true).open("/dev/class/display-controller")?;
            watch_directory(&dir, ZX_TIME_INFINITE, |_event, path| {
                first_path = Some(format!("/dev/class/display-controller/{}", path.display()));
                Err(zx::Status::STOP)
            });
            first_path.unwrap()
        };
        let file = OpenOptions::new().read(true).write(true).open(device_path)?;

        let channel = fdio::clone_channel(&file)?;
        let mut provider = ProviderSynchronousProxy::new(channel);

        let (device_client, device_server) = zx::Channel::create()?;
        let (dc_client, dc_server) = endpoints::create_endpoints::<ControllerMarker>()?;
        let status = provider.open_controller(device_server, dc_server, zx::Time::INFINITE)?;
        if status != zx::sys::ZX_OK {
            return Err(format_err!("Failed to open display controller"));
        }

        let proxy = dc_client.into_proxy()?;
        let config = Self::create_config_from_event_stream(&proxy, executor)?;
        let layer = Self::configure_layer(config, &proxy, executor)?;

        Ok(FrameBuffer {
            display_controller: device_client,
            controller: proxy,
            config: config,
            layer_id: layer,
        })
    }

    pub fn new_frame(&self, executor: &mut fasync::Executor) -> Result<Frame, Error> {
        Frame::new(&self, executor)
    }

    pub fn get_config(&self) -> Config {
        self.config
    }

    pub fn byte_size(&self) -> usize {
        self.config.height as usize * self.config.linear_stride_bytes()
    }
}

impl Drop for FrameBuffer {
    fn drop(&mut self) {}
}

#[cfg(test)]
mod tests {
    use fuchsia_async as fasync;

    use FrameBuffer;

    #[test]
    fn test_framebuffer() {
        let mut executor = fasync::Executor::new().unwrap();
        let fb = FrameBuffer::new(None, &mut executor).unwrap();
        let _frame = fb.new_frame(&mut executor).unwrap();
    }
}
