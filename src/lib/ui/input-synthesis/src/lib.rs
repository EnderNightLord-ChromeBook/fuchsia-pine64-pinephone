// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use std::{
    thread,
    time::{Duration, SystemTime},
};

use failure::Error;

use fidl::endpoints::{self, ServerEnd};

use fidl_fuchsia_ui_input::{
    self, Axis, AxisScale, DeviceDescriptor, InputDeviceMarker, InputDeviceProxy,
    InputDeviceRegistryMarker, InputReport, KeyboardDescriptor, KeyboardReport,
    MediaButtonsDescriptor, MediaButtonsReport, Range, Touch, TouchscreenDescriptor,
    TouchscreenReport,
};

use fuchsia_component as app;

pub mod inverse_keymap;
pub mod keymaps;
pub mod usages;

use crate::{inverse_keymap::InverseKeymap, usages::Usages};

trait ServerConsumer {
    fn consume(
        &self,
        device: &mut DeviceDescriptor,
        server: ServerEnd<InputDeviceMarker>,
    ) -> Result<(), Error>;
}

struct RegistryServerConsumer;

impl ServerConsumer for RegistryServerConsumer {
    fn consume(
        &self,
        device: &mut DeviceDescriptor,
        server: ServerEnd<InputDeviceMarker>,
    ) -> Result<(), Error> {
        let registry = app::client::connect_to_service::<InputDeviceRegistryMarker>()?;
        registry.register_device(device, server)?;

        Ok(())
    }
}

macro_rules! register_device {
    ( $consumer:expr , $field:ident : $value:expr ) => {{
        let mut device = DeviceDescriptor {
            device_info: None,
            keyboard: None,
            media_buttons: None,
            mouse: None,
            stylus: None,
            touchscreen: None,
            sensor: None,
        };
        device.$field = Some(Box::new($value));

        let (input_device_client, input_device_server) =
            endpoints::create_endpoints::<InputDeviceMarker>()?;
        $consumer.consume(&mut device, input_device_server)?;

        Ok(input_device_client.into_proxy()?)
    }};
}

fn register_touchsreen(
    consumer: impl ServerConsumer,
    width: u32,
    height: u32,
) -> Result<InputDeviceProxy, Error> {
    register_device! {
        consumer,
        touchscreen: TouchscreenDescriptor {
            x: Axis {
                range: Range { min: 0, max: width as i32 },
                resolution: 1,
                scale: AxisScale::Linear,
            },
            y: Axis {
                range: Range { min: 0, max: height as i32 },
                resolution: 1,
                scale: AxisScale::Linear,
            },
            max_finger_id: 255,
        }
    }
}

fn register_keyboard(consumer: impl ServerConsumer) -> Result<InputDeviceProxy, Error> {
    register_device! {
        consumer,
        keyboard: KeyboardDescriptor {
            keys: (Usages::HidUsageKeyA as u32..Usages::HidUsageKeyRightGui as u32).collect(),
        }
    }
}

fn register_media_buttons(consumer: impl ServerConsumer) -> Result<InputDeviceProxy, Error> {
    register_device! {
        consumer,
        media_buttons: MediaButtonsDescriptor {
            buttons: fidl_fuchsia_ui_input::MIC_MUTE
                | fidl_fuchsia_ui_input::VOLUME_DOWN
                | fidl_fuchsia_ui_input::VOLUME_UP,
        }
    }
}

fn nanos_from_epoch() -> Result<u64, Error> {
    SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|duration| duration.as_nanos() as u64)
        .map_err(Into::into)
}

fn repeat_with_delay(
    times: usize,
    delay: Duration,
    f1: impl Fn(usize) -> Result<(), Error>,
    f2: impl Fn(usize) -> Result<(), Error>,
) -> Result<(), Error> {
    for i in 0..times {
        f1(i)?;
        thread::sleep(delay);
        f2(i)?;
    }

    Ok(())
}

fn media_buttons(
    volume_up: bool,
    volume_down: bool,
    mic_mute: bool,
    reset: bool,
    time: u64,
) -> InputReport {
    InputReport {
        event_time: time,
        keyboard: None,
        media_buttons: Some(Box::new(MediaButtonsReport {
            volume_up,
            volume_down,
            mic_mute,
            reset,
        })),
        mouse: None,
        stylus: None,
        touchscreen: None,
        sensor: None,
        trace_id: 0,
    }
}

fn media_button_event(
    volume_up: bool,
    volume_down: bool,
    mic_mute: bool,
    reset: bool,
    consumer: impl ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_media_buttons(consumer)?;

    input_device
        .dispatch_report(&mut media_buttons(
            volume_up,
            volume_down,
            mic_mute,
            reset,
            nanos_from_epoch()?,
        ))
        .map_err(Into::into)
}

/// Simulates a media button event.
pub async fn media_button_event_command(
    volume_up: bool,
    volume_down: bool,
    mic_mute: bool,
    reset: bool,
) -> Result<(), Error> {
    media_button_event(volume_up, volume_down, mic_mute, reset, RegistryServerConsumer)
}

fn key_press(keyboard: KeyboardReport, time: u64) -> InputReport {
    InputReport {
        event_time: time,
        keyboard: Some(Box::new(keyboard)),
        media_buttons: None,
        mouse: None,
        stylus: None,
        touchscreen: None,
        sensor: None,
        trace_id: 0,
    }
}

fn key_press_usage(usage: Option<u32>, time: u64) -> InputReport {
    key_press(
        KeyboardReport {
            pressed_keys: match usage {
                Some(usage) => vec![usage],
                None => vec![],
            },
        },
        time,
    )
}

fn keyboard_event(usage: u32, duration: Duration, consumer: impl ServerConsumer) -> Result<(), Error> {
    let input_device = register_keyboard(consumer)?;

    repeat_with_delay(
        1,
        duration,
        |_| {
            // Key pressed.
            input_device
                .dispatch_report(&mut key_press_usage(Some(usage), nanos_from_epoch()?))
                .map_err(Into::into)
        },
        |_| {
            // Key released.
            input_device
                .dispatch_report(&mut key_press_usage(None, nanos_from_epoch()?))
                .map_err(Into::into)
        },
    )
}

/// Simulates a key press of specified `usage`.
///
/// `duration` is the time spent between key-press and key-release events.
pub async fn keyboard_event_command(usage: u32, duration: Duration) -> Result<(), Error> {
    keyboard_event(usage, duration, RegistryServerConsumer)
}

fn text(input: String, duration: Duration, consumer: impl ServerConsumer) -> Result<(), Error> {
    let input_device = register_keyboard(consumer)?;
    let key_sequence = InverseKeymap::new(keymaps::QWERTY_MAP)
        .derive_key_sequence(&input)
        .ok_or_else(|| failure::err_msg("Cannot translate text to key sequence"))?;

    let stroke_duration = duration / (key_sequence.len() - 1) as u32;
    let mut key_iter = key_sequence.into_iter().peekable();

    while let Some(keyboard) = key_iter.next() {
        let result: Result<(), Error> = input_device
            .dispatch_report(&mut key_press(keyboard, nanos_from_epoch()?))
            .map_err(Into::into);
        result?;

        if key_iter.peek().is_some() {
            thread::sleep(stroke_duration);
        }
    }

    Ok(())
}

/// Simulates `input` being typed on a [qwerty] keyboard by making use of [`InverseKeymap`].
///
/// `duration` is divided equally between all keyboard events.
///
/// [qwerty]: keymaps/constant.QWERTY_MAP.html
pub async fn text_command(input: String, duration: Duration) -> Result<(), Error> {
    text(input, duration, RegistryServerConsumer)
}

fn tap(pos: Option<(u32, u32)>, time: u64) -> InputReport {
    InputReport {
        event_time: time,
        keyboard: None,
        media_buttons: None,
        mouse: None,
        stylus: None,
        touchscreen: Some(Box::new(TouchscreenReport {
            touches: match pos {
                Some((x, y)) => {
                    vec![Touch { finger_id: 1, x: x as i32, y: y as i32, width: 0, height: 0 }]
                }
                None => vec![],
            },
        })),
        sensor: None,
        trace_id: 0,
    }
}

fn tap_event(
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    tap_event_count: usize,
    duration: Duration,
    consumer: impl ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_touchsreen(consumer, width, height)?;
    let tap_duration = duration / tap_event_count as u32;

    repeat_with_delay(
        tap_event_count,
        tap_duration,
        |_| {
            // Touch down.
            input_device
                .dispatch_report(&mut tap(Some((x, y)), nanos_from_epoch()?))
                .map_err(Into::into)
        },
        |_| {
            // Touch up.
            input_device.dispatch_report(&mut tap(None, nanos_from_epoch()?)).map_err(Into::into)
        },
    )
}

/// Simulates `tap_event_count` taps at coordinates `(x, y)` for a touchscreen of explicit
/// `width` and `height`.
///
/// `duration` is divided equally between touch-down and touch-up event pairs, while the
/// transition between these pairs is immediate.
pub async fn tap_event_command(
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    tap_event_count: usize,
    duration: Duration,
) -> Result<(), Error> {
    tap_event(x, y, width, height, tap_event_count, duration, RegistryServerConsumer)
}

fn swipe(
    x0: u32,
    y0: u32,
    x1: u32,
    y1: u32,
    width: u32,
    height: u32,
    move_event_count: usize,
    duration: Duration,
    consumer: impl ServerConsumer,
) -> Result<(), Error> {
    let input_device = register_touchsreen(consumer, width, height)?;

    let mut delta_x = x1 as f64 - x0 as f64;
    let mut delta_y = y1 as f64 - y0 as f64;

    let swipe_event_delay = if move_event_count > 1 {
        // We have move_event_count + 2 events:
        //   DOWN
        //   MOVE x move_event_count
        //   UP
        // so we need (move_event_count + 1) delays.
        delta_x /= move_event_count as f64;
        delta_y /= move_event_count as f64;
        duration / (move_event_count + 1) as u32
    } else {
        duration
    };

    repeat_with_delay(
        move_event_count + 2,
        swipe_event_delay,
        |i| {
            let time = nanos_from_epoch()?;
            let mut report = match i {
                // DOWN
                0 => tap(Some((x0, y0)), time),
                // MOVE
                i if i <= move_event_count => tap(
                    Some((
                        (x0 as f64 + (i as f64 * delta_x).round()) as u32,
                        (y0 as f64 + (i as f64 * delta_y).round()) as u32,
                    )),
                    time,
                ),
                // UP
                _ => tap(None, time),
            };

            input_device.dispatch_report(&mut report).map_err(Into::into)
        },
        |_| Ok(()),
    )
}

/// Simulates swipe from coordinates `(x0, y0)` to `(x1, y1)` for a touchscreen of explicit
/// `width` and `height`, with `move_event_count` touch-move events in between.
///
/// `duration` is the time spent between the touch-down and first touch-move events when
/// `move_event_count > 0` or between the touch-down the touch-up events otherwise.
pub async fn swipe_command(
    x0: u32,
    y0: u32,
    x1: u32,
    y1: u32,
    width: u32,
    height: u32,
    move_event_count: usize,
    duration: Duration,
) -> Result<(), Error> {
    swipe(x0, y0, x1, y1, width, height, move_event_count, duration, RegistryServerConsumer)
}

#[cfg(test)]
mod tests {
    use super::*;

    use fidl_fuchsia_ui_input::InputDeviceRequest;

    use fuchsia_async as fasync;

    use futures::TryStreamExt;

    macro_rules! assert_reports_eq {
        (
            $report:ident ,
            $event:expr ,
            [ $( { $name:ident : $value:expr } ),* $( , )? ]
            $( , )?
        ) => {{
            let mut executor = fasync::Executor::new().expect("failed to create executor");

            struct $report;

            impl ServerConsumer for $report {
                fn consume(
                    &self,
                    _: &mut DeviceDescriptor,
                    server: ServerEnd<InputDeviceMarker>,
                ) -> Result<(), Error> {
                    fasync::spawn(async move {
                        let mut stream = server.into_stream().expect("failed to convert to stream");

                        $(
                            let option = stream.try_next().await
                                .expect("failed to await on the next element");
                            assert!(option.is_some(), "stream should not be empty");

                            if let Some(request) = option {
                                let InputDeviceRequest::DispatchReport { report, .. } = request;
                                assert_eq!(
                                    report.$name,
                                    Some(Box::new($value)),
                                );
                            }
                        )*

                        assert!(
                            stream.try_next().await
                                .expect("failed to await on the next element")
                                .is_none(),
                            "stream should be empty"
                        );
                    });

                    Ok(())
                }
            }

            executor.run(async { $event }, 2).expect("failed to run input event");
        }};
    }

    #[test]
    fn media_event_report() {
        assert_reports_eq!(TestConsumer,
            media_button_event(true, false, true, false, TestConsumer),
            [
                {
                    media_buttons: MediaButtonsReport {
                        volume_up: true,
                        volume_down: false,
                        mic_mute: true,
                        reset: false,
                    }
                },
            ]
        );
    }

    #[test]
    fn keyboard_event_report() {
        assert_reports_eq!(TestConsumer,
            keyboard_event(40, Duration::from_millis(0), TestConsumer),
            [
                {
                    keyboard: KeyboardReport {
                        pressed_keys: vec![40]
                    }
                },
                {
                    keyboard: KeyboardReport {
                        pressed_keys: vec![]
                    }
                },
            ]
        );
    }

    #[test]
    fn text_event_report() {
        assert_reports_eq!(TestConsumer,
            text("A".to_string(), Duration::from_millis(0), TestConsumer),
            [
                {
                    keyboard: KeyboardReport {
                        pressed_keys: vec![225]
                    }
                },
                {
                    keyboard: KeyboardReport {
                        pressed_keys: vec![4, 225]
                    }
                },
                {
                    keyboard: KeyboardReport {
                        pressed_keys: vec![]
                    }
                },
            ]
        );
    }

    #[test]
    fn tap_event_report() {
        assert_reports_eq!(TestConsumer,
            tap_event(10, 10, 1000, 1000, 1, Duration::from_millis(0), TestConsumer),
            [
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 10,
                            y: 10,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![],
                    }
                },
            ]
        );
    }

    #[test]
    fn swipe_event_report() {
        assert_reports_eq!(TestConsumer,
            swipe(10, 10, 100, 100, 1000, 1000, 2, Duration::from_millis(0), TestConsumer),
            [
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 10,
                            y: 10,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 55,
                            y: 55,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 100,
                            y: 100,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![],
                    }
                },
            ]
        );
    }

    #[test]
    fn swipe_event_report_inverted() {
        assert_reports_eq!(TestConsumer,
            swipe(100, 100, 10, 10, 1000, 1000, 2, Duration::from_millis(0), TestConsumer),
            [
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 100,
                            y: 100,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 55,
                            y: 55,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![Touch {
                            finger_id: 1,
                            x: 10,
                            y: 10,
                            width: 0,
                            height: 0,
                        }],
                    }
                },
                {
                    touchscreen: TouchscreenReport {
                        touches: vec![],
                    }
                },
            ]
        );
    }
}
