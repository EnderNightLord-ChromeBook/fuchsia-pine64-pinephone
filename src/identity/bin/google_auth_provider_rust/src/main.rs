// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! GoogleAuthProvider is an implementation of the `AuthProvider` FIDL protocol
//! that communicates with the Google identity system to perform authentication
//! for and issue tokens for Google accounts.

#![deny(missing_docs)]
#![feature(async_await)]

mod auth_provider;
mod auth_provider_factory;
mod constants;
mod error;
mod http;
mod oauth;
mod openid;
mod web;

use crate::auth_provider_factory::GoogleAuthProviderFactory;
use failure::{Error, ResultExt};
use fuchsia_async as fasync;
use fuchsia_component::server::ServiceFs;
use futures::StreamExt;
use log::{error, info};

fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["auth"]).expect("Can't init logger");
    info!("Starting Google Auth Provider");

    let mut executor = fasync::Executor::new().context("Error creating executor")?;

    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |stream| {
        let auth_provider_factory =
            GoogleAuthProviderFactory::new().expect("Error creating GoogleAuthProviderFactory");
        fasync::spawn(async move {
            auth_provider_factory
                .handle_requests_from_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Error handling AuthProviderFactory channel {:?}", e));
        });
    });
    fs.take_and_serve_directory_handle()?;

    executor.run_singlethreaded(fs.collect::<()>());
    Ok(())
}
