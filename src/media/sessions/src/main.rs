// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]
#![recursion_limit = "512"]

#[macro_use]
mod log_error;
mod fidl_clones;
mod mpmc;
mod proxies;
mod services;
mod state;
#[cfg(test)]
mod test;

use self::services::{publisher::Publisher, registry::Registry};
use failure::Error;
use fuchsia_async as fasync;
use fuchsia_component::server::ServiceFs;
use fuchsia_zircon as zx;
use futures::{lock::Mutex, Future, StreamExt, TryFutureExt};
use std::rc::Rc;
use zx::AsHandleRef;

type Result<T> = std::result::Result<T, Error>;

type Ref<T> = Rc<Mutex<T>>;

const CHANNEL_BUFFER_SIZE: usize = 100;

/// This number should be forgiving. If we lower it we may want to build some
/// in-process staging area for changes so we can send them to clients that ACK
/// late. At 20 though, clients that don't ACK can't reasonably expect to be
/// accomodated.
const MAX_EVENTS_SENT_WITHOUT_ACK: usize = 20;

fn session_id_rights() -> zx::Rights {
    zx::Rights::DUPLICATE | zx::Rights::TRANSFER | zx::Rights::INSPECT
}

fn clone_session_id_handle(session_id_handle: &zx::Event) -> Result<zx::Event> {
    Ok(session_id_handle.as_handle_ref().duplicate(session_id_rights())?.into())
}

fn spawn_log_error(fut: impl Future<Output = Result<()>> + 'static) {
    fasync::spawn_local(fut.unwrap_or_else(|e| eprintln!("{}", e)))
}

#[fasync::run_singlethreaded]
async fn main() {
    let session_list = Ref::default();
    let active_session_queue = Ref::default();
    let (active_session_sink, active_session_stream) = mpmc::channel(CHANNEL_BUFFER_SIZE);
    let (collection_event_sink, collection_event_stream) = mpmc::channel(CHANNEL_BUFFER_SIZE);

    let publisher = Publisher::new(
        session_list.clone(),
        active_session_queue.clone(),
        active_session_sink,
        collection_event_sink,
    );
    let registry = Registry::new(
        session_list.clone(),
        active_session_queue.clone(),
        active_session_stream,
        collection_event_stream,
    );

    let mut server = ServiceFs::new_local();
    server
        .dir("svc")
        .add_fidl_service(|request_stream| spawn_log_error(publisher.clone().serve(request_stream)))
        .add_fidl_service(|request_stream| spawn_log_error(registry.clone().serve(request_stream)));
    server.take_and_serve_directory_handle().expect("To serve Media Session services");

    await!(server.collect::<()>());
}
