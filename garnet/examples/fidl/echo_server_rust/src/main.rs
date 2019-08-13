// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use failure::{Error, ResultExt};
use fidl_fidl_examples_echo::{EchoRequest, EchoRequestStream};
use fuchsia_component::server::ServiceFs;
use fuchsia_async as fasync;
use futures::prelude::*;

async fn run_echo_server(mut stream: EchoRequestStream, quiet: bool) -> Result<(), Error> {
    while let Some(EchoRequest::EchoString { value, responder }) =
        stream.try_next().await.context("error running echo server")?
    {
        if !quiet {
            println!("Received echo request for string {:?}", value);
        }
        responder.send(value.as_ref().map(|s| &**s)).context("error sending response")?;
        if !quiet {
            println!("echo response sent successfully");
        }
    }
    Ok(())
}

enum IncomingService {
    Echo(EchoRequestStream),
    // ... more services here
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let quiet = std::env::args().any(|arg| arg == "-q");

    let mut fs = ServiceFs::new_local();
    fs.dir("svc")
      .add_fidl_service(IncomingService::Echo);

    fs.take_and_serve_directory_handle()?;

    const MAX_CONCURRENT: usize = 10_000;
    let fut = fs.for_each_concurrent(MAX_CONCURRENT, |IncomingService::Echo(stream)|
        run_echo_server(stream, quiet)
            .unwrap_or_else(|e| println!("{:?}", e))
    );

    fut.await;
    Ok(())
}
