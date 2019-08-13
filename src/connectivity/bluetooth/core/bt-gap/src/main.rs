// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

// Macros used to serialize bonding data FIDL types for persistent storage.
#[macro_use]
extern crate serde_derive;

use {
    failure::{Error, ResultExt},
    fidl::endpoints::ServiceMarker,
    fidl_fuchsia_bluetooth_bredr::ProfileMarker,
    fidl_fuchsia_bluetooth_control::ControlRequestStream,
    fidl_fuchsia_bluetooth_gatt::Server_Marker,
    fidl_fuchsia_bluetooth_le::{CentralMarker, PeripheralMarker},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{self as syslog, fx_log_err, fx_log_info, fx_log_warn},
    futures::{future::try_join, FutureExt, StreamExt, TryFutureExt, TryStreamExt},
    pin_utils::pin_mut,
};

use crate::{
    adapters::{AdapterEvent::*, *},
    host_dispatcher::{HostService::*, *},
};

mod adapters;
mod host_device;
mod host_dispatcher;
mod services;
mod store;
#[cfg(test)]
mod test;
mod types;

const BT_GAP_COMPONENT_ID: &'static str = "bt-gap";

fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["bt-gap"]).expect("Can't init logger");
    fx_log_info!("Starting bt-gap...");
    let result = run().context("Error running BT-GAP");
    if let Err(e) = &result {
        fx_log_err!("{:?}", e)
    };
    Ok(result?)
}

fn run() -> Result<(), Error> {
    let mut executor = fasync::Executor::new().context("Error creating executor")?;
    let inspect = fuchsia_inspect::Inspector::new();
    let stash_inspect = inspect.root().create_child("persistent");
    let stash = executor
        .run_singlethreaded(store::stash::init_stash(BT_GAP_COMPONENT_ID, stash_inspect))
        .context("Error initializing Stash service")?;

    let hd = HostDispatcher::new(stash, inspect.root().create_child("system"));
    let watch_hd = hd.clone();
    let central_hd = hd.clone();
    let control_hd = hd.clone();
    let peripheral_hd = hd.clone();
    let profile_hd = hd.clone();
    let gatt_hd = hd.clone();

    let host_watcher = async {
        let stream = watch_hosts();
        pin_mut!(stream);
        while let Some(msg) = stream.try_next().await? {
            let hd = watch_hd.clone();
            match msg {
                AdapterAdded(device_path) => {
                    let result = hd.add_adapter(&device_path).await;
                    if let Err(e) = &result {
                        fx_log_warn!("Error adding bt-host device '{:?}': {:?}", device_path, e);
                    }
                    result?
                }
                AdapterRemoved(device_path) => {
                    hd.rm_adapter(&device_path);
                }
            }
        }
        Ok(())
    };

    let mut fs = ServiceFs::new();
    inspect.export(&mut fs);
    fs.dir("svc")
        .add_fidl_service(move |s| control_service(control_hd.clone(), s))
        .add_service_at(CentralMarker::NAME, move |chan| {
            if let Ok(chan) = fasync::Channel::from_channel(chan) {
                fx_log_info!("Connecting CentralService to Adapter");
                fasync::spawn(central_hd.clone().request_host_service(chan, LeCentral));
            }
            None
        })
        .add_service_at(PeripheralMarker::NAME, move |chan| {
            if let Ok(chan) = fasync::Channel::from_channel(chan) {
                fx_log_info!("Connecting Peripheral Service to Adapter");
                fasync::spawn(peripheral_hd.clone().request_host_service(chan, LePeripheral));
            }
            None
        })
        .add_service_at(ProfileMarker::NAME, move |chan| {
            if let Ok(chan) = fasync::Channel::from_channel(chan) {
                fx_log_info!("Connecting Profile Service to Adapter");
                fasync::spawn(profile_hd.clone().request_host_service(chan, Profile));
            }
            None
        })
        .add_service_at(Server_Marker::NAME, move |chan| {
            if let Ok(chan) = fasync::Channel::from_channel(chan) {
                fx_log_info!("Connecting Gatt Service to Adapter");
                fasync::spawn(gatt_hd.clone().request_host_service(chan, LeGatt));
            }
            None
        });
    fs.take_and_serve_directory_handle()?;

    executor
        .run_singlethreaded(try_join(fs.collect::<()>().map(Ok), host_watcher))
        .map(|((), ())| ())
}

fn control_service(hd: HostDispatcher, stream: ControlRequestStream) {
    fx_log_info!("Spawning Control Service");
    fasync::spawn(
        services::start_control_service(hd.clone(), stream)
            .unwrap_or_else(|e| eprintln!("Failed to spawn {:?}", e)),
    )
}
