// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use {
    failure::{bail, Error, ResultExt},
    fidl_fuchsia_bluetooth_control::{
        ControlEvent, ControlEventStream, ControlMarker, ControlProxy,
    },
    fuchsia_async::{self as fasync, futures::select},
    fuchsia_bluetooth::types::Status,
    fuchsia_component::client::connect_to_service,
    futures::{
        channel::mpsc::{channel, SendError},
        FutureExt, Sink, SinkExt, Stream, StreamExt, TryFutureExt, TryStreamExt,
    },
    parking_lot::Mutex,
    pin_utils::pin_mut,
    regex::Regex,
    rustyline::{error::ReadlineError, CompletionType, Config, EditMode, Editor},
    std::{collections::HashMap, fmt::Write, iter::FromIterator, sync::Arc, thread},
};

use crate::{
    commands::{Cmd, CmdHelper, ReplControl},
    types::{AdapterInfo, DeviceClass, MajorClass, MinorClass, RemoteDevice, TryInto},
};

mod commands;
mod types;

static PROMPT: &str = "\x1b[34mbt>\x1b[0m ";
/// Escape code to clear the pty line on which the cursor is located.
/// Used when evented output is intermingled with the REPL prompt.
static CLEAR_LINE: &str = "\x1b[2K";

async fn get_active_adapter(control_svc: &ControlProxy) -> Result<String, Error> {
    if let Some(adapter) = await!(control_svc.get_active_adapter_info())? {
        return Ok(AdapterInfo::from(*adapter).to_string());
    }
    Ok(String::from("No Active Adapter"))
}

async fn get_adapters(control_svc: &ControlProxy) -> Result<String, Error> {
    if let Some(adapters) = await!(control_svc.get_adapters())? {
        let mut string = String::new();
        for adapter in adapters {
            let _ = writeln!(string, "{}", AdapterInfo::from(adapter));
        }
        return Ok(string);
    }
    Ok(String::from("No adapters detected"))
}

async fn set_active_adapter<'a>(
    args: &'a [&'a str],
    control_svc: &'a ControlProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        bail!("usage: {}", Cmd::SetActiveAdapter.cmd_help());
    }
    println!("Setting active adapter");
    // `args[0]` is the identifier of the adapter to make active
    let response = await!(control_svc.set_active_adapter(args[0]))?;
    if response.error.is_some() {
        Ok(Status::from(response).to_string())
    } else {
        Ok(String::new())
    }
}

async fn set_adapter_name<'a>(
    args: &'a [&'a str],
    control_svc: &'a ControlProxy,
) -> Result<String, Error> {
    if args.len() > 1 {
        bail!("usage: {}", Cmd::SetAdapterName.cmd_help());
    }
    println!("Setting local name of the active adapter");
    // `args[0]` is the value to set as the name of the adapter
    let response = await!(control_svc.set_name(args.get(0).map(|&name| name)))?;
    if response.error.is_some() {
        Ok(Status::from(response).to_string())
    } else {
        Ok(String::new())
    }
}

/// Set the class of device for the currently active adapter. Arguments are optional, and defaults
/// will be used if arguments aren't provided.
///
/// Returns an error if the input is not recognized as a valid device class .
async fn set_adapter_device_class<'a>(
    args: &'a [&'a str],
    control_svc: &'a ControlProxy,
) -> Result<String, Error> {
    let mut args = args.iter();
    println!("Setting device class of the active adapter");
    let mut cod = DeviceClass {
        major: args.next().map(|arg| arg.try_into()).unwrap_or(Ok(MajorClass::Uncategorized))?,
        minor: args.next().map(|arg| arg.try_into()).unwrap_or(Ok(MinorClass::not_set()))?,
        service: args.try_into()?,
    }
    .into();
    let response = await!(control_svc.set_device_class(&mut cod))?;
    if response.error.is_some() {
        Ok(Status::from(response).to_string())
    } else {
        Ok(format!("Set device class to 0x{:x}", cod.value))
    }
}

fn get_devices(state: &Mutex<State>) -> String {
    let state = state.lock();
    if state.devices.is_empty() {
        String::from("No known remote devices")
    } else {
        String::from_iter(state.devices.values().map(|device| device.to_string()))
    }
}

/// Get the string representation of a device from either an identifier or address
fn get_device<'a>(args: &'a [&'a str], state: &Mutex<State>) -> String {
    if args.len() != 1 {
        return format!("usage: {}", Cmd::GetDevice.cmd_help());
    }

    to_identifier(state, args[0])
        .and_then(|id| state.lock().devices.get(&id).map(|device| device.to_string()))
        .unwrap_or_else(|| String::from("No known device"))
}

async fn set_discovery(discovery: bool, control_svc: &ControlProxy) -> Result<String, Error> {
    println!("{} Discovery!", if discovery { "Starting" } else { "Stopping" });
    let response = await!(control_svc.request_discovery(discovery))?;
    if response.error.is_some() {
        Ok(Status::from(response).to_string())
    } else {
        Ok(String::new())
    }
}

// Find the identifier for a `RemoteDevice` based on a `key` that is either an identifier or an
// address.
// Returns `None` if the given address does not belong to a known device.
fn to_identifier(state: &Mutex<State>, key: &str) -> Option<String> {
    // Compile regex inline because it is not ever expected to be a bottleneck
    let address_pattern = Regex::new(r"^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$")
        .expect("Could not compile mac address regex pattern.");
    if address_pattern.is_match(key) {
        state
            .lock()
            .devices
            .values()
            .find(|device| device.0.address == key)
            .map(|device| device.0.identifier.to_string())
    } else {
        Some(key.to_string())
    }
}

async fn connect<'a>(
    args: &'a [&'a str],
    state: &'a Mutex<State>,
    control_svc: &'a ControlProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        return Ok(format!("usage: {}", Cmd::Connect.cmd_help()));
    }
    // `args[0]` is the identifier of the remote device to connect to
    let id = match to_identifier(state, args[0]) {
        Some(id) => id,
        None => return Ok(format!("Unable to connect: Unknown address {}", args[0])),
    };
    let response = await!(control_svc.connect(&id))?;
    if response.error.is_some() {
        Ok(Status::from(response).to_string())
    } else {
        Ok(String::new())
    }
}

async fn set_discoverable(discoverable: bool, control_svc: &ControlProxy) -> Result<String, Error> {
    if discoverable {
        println!("Becoming discoverable..");
    } else {
        println!("Revoking discoverability..");
    }
    let response = await!(control_svc.set_discoverable(discoverable))?;
    if response.error.is_some() {
        Ok(Status::from(response).to_string())
    } else {
        Ok(String::new())
    }
}

/// Listen on the control event channel for new events. Track state and print output where
/// appropriate.
async fn run_listeners(mut stream: ControlEventStream, state: &Mutex<State>) -> Result<(), Error> {
    while let Some(evt) = await!(stream.try_next())? {
        print!("{}", CLEAR_LINE);
        match evt {
            ControlEvent::OnActiveAdapterChanged { adapter: Some(adapter) } => {
                println!("Active adapter set to {}", adapter.address);
            }
            ControlEvent::OnActiveAdapterChanged { adapter: None } => {
                println!("No active adapter");
            }
            ControlEvent::OnAdapterUpdated { adapter } => {
                println!("Adapter {} updated", adapter.address);
            }
            ControlEvent::OnAdapterRemoved { identifier } => {
                println!("Adapter {} removed", identifier);
            }
            ControlEvent::OnDeviceUpdated { device } => {
                let device = RemoteDevice::from(device);
                print_peer_state_updates(&state.lock(), &device);
                state.lock().devices.insert(device.0.identifier.clone(), device);
            }
            ControlEvent::OnDeviceRemoved { identifier } => {
                state.lock().devices.remove(&identifier);
            }
        }
    }
    Ok(())
}

fn print_peer_state_updates(state: &State, device: &RemoteDevice) {
    if let Some(msg) = peer_state_updates(state, device) {
        println!("{} {} {}", device.0.identifier, device.0.address, msg)
    }
}

fn peer_state_updates(state: &State, device: &RemoteDevice) -> Option<String> {
    let previous = state.devices.get(&device.0.identifier);
    let was_connected = previous.map_or(false, |d| d.0.connected);
    let was_bonded = previous.map_or(false, |d| d.0.bonded);

    let conn_str = match (was_connected, device.0.connected) {
        (false, true) => Some("[connected]"),
        (true, false) => Some("[disconnected]"),
        _ => None,
    };
    let bond_str = match (was_bonded, device.0.bonded) {
        (false, true) => Some("[bonded]"),
        (true, false) => Some("[unbonded]"),
        _ => None,
    };
    match (conn_str, bond_str) {
        (Some(a), Some(b)) => Some(format!("{} {}", a, b)),
        (Some(a), None) => Some(a.to_string()),
        (None, Some(b)) => Some(b.to_string()),
        (None, None) => None,
    }
}

/// Tracks all state local to the command line tool.
pub struct State {
    pub devices: HashMap<String, RemoteDevice>,
}

impl State {
    pub fn new(devs: Vec<fidl_fuchsia_bluetooth_control::RemoteDevice>) -> Arc<Mutex<State>> {
        let devices: HashMap<_, _> =
            devs.into_iter().map(|d| (d.identifier.clone(), RemoteDevice(d))).collect();
        Arc::new(Mutex::new(State { devices }))
    }
}

/// Handle a single raw input command from a user and indicate whether the command should
/// result in continuation or breaking of the read evaluate print loop.
async fn handle_cmd(
    bt_svc: &ControlProxy,
    state: Arc<Mutex<State>>,
    line: String,
) -> Result<ReplControl, Error> {
    let components: Vec<_> = line.trim().split_whitespace().collect();
    if let Some((raw_cmd, args)) = components.split_first() {
        let cmd = raw_cmd.parse();
        let res = match cmd {
            Ok(Cmd::Connect) => await!(connect(args, &state, &bt_svc)),
            Ok(Cmd::StartDiscovery) => await!(set_discovery(true, &bt_svc)),
            Ok(Cmd::StopDiscovery) => await!(set_discovery(false, &bt_svc)),
            Ok(Cmd::Discoverable) => await!(set_discoverable(true, &bt_svc)),
            Ok(Cmd::NotDiscoverable) => await!(set_discoverable(false, &bt_svc)),
            Ok(Cmd::GetDevices) => Ok(get_devices(&state)),
            Ok(Cmd::GetDevice) => Ok(get_device(args, &state)),
            Ok(Cmd::GetAdapters) => await!(get_adapters(&bt_svc)),
            Ok(Cmd::SetActiveAdapter) => await!(set_active_adapter(args, &bt_svc)),
            Ok(Cmd::SetAdapterName) => await!(set_adapter_name(args, &bt_svc)),
            Ok(Cmd::SetAdapterDeviceClass) => await!(set_adapter_device_class(args, &bt_svc)),
            Ok(Cmd::ActiveAdapter) => await!(get_active_adapter(&bt_svc)),
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
fn cmd_stream(
    state: Arc<Mutex<State>>,
) -> (impl Stream<Item = String>, impl Sink<(), SinkError = SendError>) {
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
            let c = CmdHelper::new(state);
            let mut rl: Editor<CmdHelper> = Editor::with_config(config);
            rl.set_helper(Some(c));
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

/// REPL execution
async fn run_repl(bt_svc: ControlProxy, state: Arc<Mutex<State>>) -> Result<(), Error> {
    // `cmd_stream` blocks on input in a separate thread and passes commands and acks back to
    // the main thread via async channels.
    let (mut commands, mut acks) = cmd_stream(state.clone());
    loop {
        if let Some(cmd) = await!(commands.next()) {
            match await!(handle_cmd(&bt_svc, state.clone(), cmd)) {
                Ok(ReplControl::Continue) => {}
                Ok(ReplControl::Break) => {
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

fn main() -> Result<(), Error> {
    let mut exec = fasync::Executor::new().context("error creating event loop")?;
    let bt_svc = connect_to_service::<ControlMarker>()
        .context("failed to connect to bluetooth control interface")?;
    let bt_svc_thread = bt_svc.clone();
    let evt_stream = bt_svc_thread.take_event_stream();

    let fut = async {
        let devices = await!(bt_svc.get_known_remote_devices()).unwrap_or(vec![]);
        let state = State::new(devices);
        let repl = run_repl(bt_svc, state.clone())
            .unwrap_or_else(|e| println!("REPL failed unexpectedly {:?}", e));
        let listeners = run_listeners(evt_stream, &state)
            .unwrap_or_else(|e| println!("Failed to subscribe to bluetooth events {:?}", e));
        pin_mut!(repl);
        pin_mut!(listeners);
        select! {
            () = repl.fuse() => (),
            () = listeners.fuse() => (),
        };
    };
    exec.run_singlethreaded(fut);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_bluetooth_control as control;

    fn device(connected: bool, bonded: bool) -> RemoteDevice {
        RemoteDevice(control::RemoteDevice {
            identifier: "device".to_string(),
            address: "00:00:00:00:00:01".to_string(),
            technology: control::TechnologyType::LowEnergy,
            name: None,
            appearance: control::Appearance::Phone,
            rssi: None,
            tx_power: None,
            connected,
            bonded,
            service_uuids: vec![],
        })
    }

    fn state_with(d: RemoteDevice) -> State {
        let mut devices = HashMap::new();
        devices.insert(d.0.identifier.clone(), d);
        State { devices }
    }

    #[test]
    fn test_peer_updates() {
        // Expected Value Table
        // each row lists:
        //   (current device conn/bond state, new device conn/bond state, expected string)
        let test_cases = vec![
            // missing
            (None, (true, false), Some("[connected]")),
            (None, (true, true), Some("[connected] [bonded]")),
            (None, (false, true), Some("[bonded]")),
            (None, (false, false), None),
            // disconnected, unbonded
            (Some((false, false)), (true, false), Some("[connected]")),
            (Some((false, false)), (true, true), Some("[connected] [bonded]")),
            (Some((false, false)), (false, true), Some("[bonded]")),
            (Some((false, false)), (false, false), None),
            // connected, unbonded
            (Some((true, false)), (true, false), None),
            (Some((true, false)), (true, true), Some("[bonded]")),
            (Some((true, false)), (false, true), Some("[disconnected] [bonded]")),
            (Some((true, false)), (false, false), Some("[disconnected]")),
            // disconnected, bonded
            (Some((false, true)), (true, false), Some("[connected] [unbonded]")),
            (Some((false, true)), (true, true), Some("[connected]")),
            (Some((false, true)), (false, true), None),
            (Some((false, true)), (false, false), Some("[unbonded]")),
            // connected, bonded
            (Some((true, true)), (true, false), Some("[unbonded]")),
            (Some((true, true)), (true, true), None),
            (Some((true, true)), (false, true), Some("[disconnected]")),
            (Some((true, true)), (false, false), Some("[disconnected] [unbonded]")),
        ];

        for case in test_cases {
            let (prev, (connected, bonded), expected) = case;
            let state = match prev {
                Some((c, b)) => state_with(device(c, b)),
                None => State { devices: HashMap::new() },
            };
            assert_eq!(
                peer_state_updates(&state, &device(connected, bonded)),
                expected.map(|s| s.to_string())
            );
        }
    }
}
