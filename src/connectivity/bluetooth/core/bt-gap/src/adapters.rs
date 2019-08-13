// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module defines a watcher that subscribes to the device filesystem and
//! produces a stream of messages when bt-host devices are added or removed from
//! the system

use {
    failure::Error,
    fuchsia_bluetooth::constants::HOST_DEVICE_DIR,
    fuchsia_syslog::fx_log_warn,
    fuchsia_vfs_watcher::{self as vfs_watcher, WatchEvent, WatchMessage},
    futures::{FutureExt, Stream, TryStreamExt},
    std::{
        fs::File,
        io,
        path::{Path, PathBuf},
    },
};

pub enum AdapterEvent {
    AdapterAdded(PathBuf),
    AdapterRemoved(PathBuf),
}

/// Watch the VFS for host adapter devices being added or removed, and produce
/// a stream of AdapterEvent messages
pub fn watch_hosts() -> impl Stream<Item = Result<AdapterEvent, Error>> {
    async {
        let dev = File::open(&HOST_DEVICE_DIR).unwrap();
        let watcher = vfs_watcher::Watcher::new(&dev)
            .await
            .expect("Cannot open vfs watcher for bt-host device path");
        watcher.try_filter_map(as_adapter_msg).err_into()
    }.flatten_stream()
}

async fn as_adapter_msg(msg: WatchMessage) -> Result<Option<AdapterEvent>, io::Error> {
    use self::AdapterEvent::*;

    let path = Path::new(HOST_DEVICE_DIR).join(&msg.filename);
    Ok(match msg.event {
        WatchEvent::EXISTING | WatchEvent::ADD_FILE => Some(AdapterAdded(path)),
        WatchEvent::REMOVE_FILE => Some(AdapterRemoved(path)),
        WatchEvent::IDLE => None,
        e => {
            fx_log_warn!("Unrecognized host watch event: {:?}", e);
            None
        }
    })
}
