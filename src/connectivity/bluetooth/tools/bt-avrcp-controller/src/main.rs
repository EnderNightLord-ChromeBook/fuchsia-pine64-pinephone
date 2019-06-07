// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro, mpsc_select)]

use {
    failure::{Error, ResultExt},
    fidl::endpoints::create_endpoints,
    fidl_fuchsia_bluetooth_avrcp::{
        AvrcpMarker, ControllerEvent, ControllerEventStream, ControllerMarker, ControllerProxy,
        TestAvrcpMarker, TestControllerMarker, TestControllerProxy,
    },
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_service,
    futures::{
        channel::mpsc::{channel, SendError},
        select, FutureExt, Sink, SinkExt, Stream, StreamExt, TryStreamExt,
    },
    hex::FromHex,
    pin_utils::pin_mut,
    rustyline::{error::ReadlineError, CompletionType, Config, EditMode, Editor},
    std::thread,
    structopt::StructOpt,
};

use crate::commands::{avc_match_string, Cmd, CmdHelper, ReplControl};

mod commands;

static PROMPT: &str = "\x1b[34mavrcp>\x1b[0m ";
/// Escape code to clear the pty line on which the cursor is located.
/// Used when evented output is intermingled with the REPL prompt.
static CLEAR_LINE: &str = "\x1b[2K";

/// Define the command line arguments that the tool accepts.
#[derive(StructOpt)]
#[structopt(
    version = "0.2.0",
    author = "Fuchsia Bluetooth Team",
    about = "Bluetooth AVRCP Controller CLI"
)]
struct Opt {
    #[structopt(help = "Target Device ID")]
    device: String,
}

async fn send_passthrough<'a>(
    args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        return Ok(format!("usage: {}", Cmd::AvcCommand.cmd_help()));
    }
    let cmd = avc_match_string(args[0]);
    if cmd.is_none() {
        return Ok(String::from("invalid avc command"));
    }

    // `args[0]` is the identifier of the remote device to connect to
    match await!(controller.send_command(cmd.unwrap()))? {
        Ok(_) => Ok(String::from("")),
        Err(e) => Ok(format!("Error sending AVC Command: {:?}", e)),
    }
}

async fn get_media<'a>(
    _args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    match await!(controller.get_media_attributes())? {
        Ok(media) => Ok(format!("Media attributes: {:#?}", media)),
        Err(e) => Ok(format!("Error fetching media attributes: {:?}", e)),
    }
}

async fn get_events_supported<'a>(
    _args: &'a [&'a str],
    controller: &'a TestControllerProxy,
) -> Result<String, Error> {
    match await!(controller.get_events_supported())? {
        Ok(events) => Ok(format!("Supported events: {:#?}", events)),
        Err(e) => Ok(format!("Error fetching supported events: {:?}", e)),
    }
}

async fn send_raw_vendor<'a>(
    args: &'a [&'a str],
    controller: &'a TestControllerProxy,
) -> Result<String, Error> {
    if args.len() < 2 {
        return Ok(format!("usage: {}", Cmd::SendRawVendorCommand.cmd_help()));
    }

    let pdu_id;
    if args[0].starts_with("0x") || args[0].starts_with("0X") {
        if let Ok(hex) = Vec::from_hex(&args[0][2..]) {
            if hex.len() < 1 {
                return Ok(format!("invalid pdu_id {}", args[0]));
            }
            pdu_id = hex[0];
        } else {
            return Ok(format!("invalid pdu_id {}", args[0]));
        }
    } else {
        if let Ok(b) = args[0].parse::<u8>() {
            pdu_id = b;
        } else {
            return Ok(format!("invalid pdu_id {}", args[0]));
        }
    }

    let byte_string = args[1..].join(" ").replace(",", " ");
    let bytes = byte_string.split(" ");

    let mut buf = vec![];

    for b in bytes {
        let b = b.trim();
        if b.eq("") {
            continue;
        } else if b.starts_with("0x") || b.starts_with("0X") {
            if let Ok(hex) = Vec::from_hex(&b[2..]) {
                buf.extend_from_slice(&hex[..]);
            } else {
                return Ok(format!("invalid hex string at {}", b));
            }
        } else {
            if let Ok(hex) = Vec::from_hex(&b[..]) {
                buf.extend_from_slice(&hex[..]);
            } else {
                return Ok(format!("invalid hex string at {}", b));
            }
        }
    }

    eprintln!("Sending {:#?}", buf);

    match await!(controller.send_raw_vendor_dependent_command(pdu_id, &mut buf.into_iter()))? {
        Ok(response) => Ok(format!("response: {:#?}", response)),
        Err(e) => Ok(format!("Error sending raw dependent command: {:?}", e)),
    }
}

async fn is_connected<'a>(
    _args: &'a [&'a str],
    controller: &'a TestControllerProxy,
) -> Result<String, Error> {
    match await!(controller.is_connected()) {
        Ok(status) => Ok(format!("Is Connected: {}", status)),
        Err(e) => Ok(format!("Error fetching supported events: {:?}", e)),
    }
}

/// Handle a single raw input command from a user and indicate whether the command should
/// result in continuation or breaking of the read evaluate print loop.
async fn handle_cmd<'a>(
    controller: &'a ControllerProxy,
    test_controller: &'a TestControllerProxy,
    line: String,
) -> Result<ReplControl, Error> {
    let components: Vec<_> = line.trim().split_whitespace().collect();
    if let Some((raw_cmd, args)) = components.split_first() {
        let cmd = raw_cmd.parse();
        let res = match cmd {
            Ok(Cmd::AvcCommand) => await!(send_passthrough(args, &controller)),
            Ok(Cmd::GetMediaAttributes) => await!(get_media(args, &controller)),
            Ok(Cmd::SendRawVendorCommand) => await!(send_raw_vendor(args, &test_controller)),
            Ok(Cmd::SupportedEvents) => await!(get_events_supported(args, &test_controller)),
            Ok(Cmd::IsConnected) => await!(is_connected(args, &test_controller)),
            Ok(Cmd::Help) => Ok(Cmd::help_msg().to_string()),
            Ok(Cmd::Exit) | Ok(Cmd::Quit) => return Ok(ReplControl::Break),
            Err(_) => Ok(format!("\"{}\" is not a valid command", raw_cmd)),
        }?;
        if res != "" {
            println!("{}", res);
        }
    }

    Ok(ReplControl::Continue)
}

/// Generates a rustyline `Editor` in a separate thread to manage user input. This input is returned
/// as a `Stream` of lines entered by the user.
///
/// The thread exits and the `Stream` is exhausted when an error occurs on stdin or the user
/// sends a ctrl-c or ctrl-d sequence.
///
/// Because rustyline shares control over output to the screen with other parts of the system, a
/// `Sink` is passed to the caller to send acknowledgements that a command has been processed and
/// that rustyline should handle the next line of input.
fn cmd_stream() -> (impl Stream<Item = String>, impl Sink<(), SinkError = SendError>) {
    // Editor thread and command processing thread must be synchronized so that output
    // is printed in the correct order.
    let (mut cmd_sender, cmd_receiver) = channel(512);
    let (ack_sender, mut ack_receiver) = channel(512);

    thread::spawn(move || -> Result<(), Error> {
        let mut exec = fasync::Executor::new().context("error creating readline event loop")?;

        let fut = async {
            let config = Config::builder()
                .auto_add_history(true)
                .history_ignore_space(true)
                .completion_type(CompletionType::List)
                .edit_mode(EditMode::Emacs)
                .build();
            let mut rl: Editor<CmdHelper> = Editor::with_config(config);
            rl.set_helper(Some(CmdHelper::new()));
            loop {
                let readline = rl.readline(PROMPT);
                match readline {
                    Ok(line) => {
                        cmd_sender.try_send(line)?;
                    }
                    Err(ReadlineError::Eof) | Err(ReadlineError::Interrupted) => {
                        return Ok(());
                    }
                    Err(e) => {
                        println!("Error: {:?}", e);
                        return Err(e.into());
                    }
                }
                // wait until processing thread is finished evaluating the last command
                // before running the next loop in the repl
                await!(ack_receiver.next());
            }
        };
        exec.run_singlethreaded(fut)
    });
    (cmd_receiver, ack_sender)
}

async fn controller_listener(mut stream: ControllerEventStream) -> Result<(), Error> {
    while let Some(evt) = await!(stream.try_next())? {
        print!("{}", CLEAR_LINE);
        match evt {
            ControllerEvent::OnNotification { notif } => {
                if let Some(value) = notif.volume {
                    println!("Volume event: {:?} {:?}", notif.timestamp.unwrap(), value);
                }
                if let Some(value) = notif.pos {
                    println!("Pos event: {:?} {:?}", notif.timestamp.unwrap(), value);
                }
                if let Some(value) = notif.status {
                    println!("Status event: {:?} {:?}", notif.timestamp.unwrap(), value);
                }
                if let Some(value) = notif.track_id {
                    println!("Track event: {:?} {:?}", notif.timestamp.unwrap(), value);
                }
            }
        }
    }
    Ok(())
}

/// REPL execution
async fn run_repl(
    controller: ControllerProxy,
    test_controller: TestControllerProxy,
) -> Result<(), Error> {
    // `cmd_stream` blocks on input in a separate thread and passes commands and acks back to
    // the main thread via async channels.
    let (mut commands, mut acks) = cmd_stream();
    loop {
        if let Some(cmd) = await!(commands.next()) {
            match await!(handle_cmd(&controller, &test_controller, cmd)) {
                Ok(ReplControl::Continue) => {}
                Ok(ReplControl::Break) => {
                    println!("\n");
                    break;
                }
                Err(e) => {
                    println!("Error handling command: {}", e);
                }
            }
        } else {
            break;
        }
        await!(acks.send(()))?;
    }
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let opt = Opt::from_args();

    let device_id = &opt.device.replace("-", "").to_lowercase();

    // Connect to test controller service

    let test_avrcp_svc = connect_to_service::<TestAvrcpMarker>()
        .context("Failed to connect to Bluetooth Test AVRCP interface")?;

    // Create a channel for our Request<TestController> to live
    let (t_client, t_server) = create_endpoints::<TestControllerMarker>()
        .expect("Error creating Test Controller endpoint");

    let _status =
        await!(test_avrcp_svc.get_test_controller_for_target(&device_id.as_str(), t_server))?;
    eprintln!(
        "Test controller obtained to device \"{device}\" AVRCP remote target service",
        device = &device_id,
    );

    // Connect to avrcp controller service

    let avrcp_svc = connect_to_service::<AvrcpMarker>()
        .context("Failed to connect to Bluetooth AVRCP interface")?;

    // Create a channel for our Request<Controller> to live
    let (c_client, c_server) =
        create_endpoints::<ControllerMarker>().expect("Error creating Controller endpoint");

    let _status = await!(avrcp_svc.get_controller_for_target(&device_id.as_str(), c_server))?;
    eprintln!(
        "Controller obtained to device \"{device}\" AVRCP remote target service",
        device = &device_id,
    );

    // setup repl

    let controller = c_client.into_proxy().expect("error obtaining controller client proxy");
    let test_controller =
        t_client.into_proxy().expect("error obtaining test controller client proxy");

    let evt_stream = controller.clone().take_event_stream();

    let event_fut = controller_listener(evt_stream).fuse();
    let repl_fut = run_repl(controller, test_controller).fuse();
    pin_mut!(event_fut);
    pin_mut!(repl_fut);

    // These futures should only return when something fails.
    select! {
        result = event_fut => {
            eprintln!(
                "Service connection returned {status:?}", status = result);
        },
        _ = repl_fut => {}
    }

    Ok(())
}
