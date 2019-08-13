// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    clap::{App, Arg, SubCommand},
    failure::{Error, ResultExt},
    fidl::endpoints::{ClientEnd, RequestStream, ServiceMarker},
    fidl_fidl_examples_echo as echo,
    fidl_fuchsia_overnet::{
        OvernetMarker, OvernetProxy, ServiceProviderRequest, ServiceProviderRequestStream,
    },
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::prelude::*,
};

fn app<'a, 'b>() -> App<'a, 'b> {
    App::new("overnet-echo")
        .version("0.1.0")
        .about("Echo example for overnet")
        .author("Fuchsia Team")
        .arg(Arg::with_name("quiet").help("Should output be quiet"))
        .subcommand(SubCommand::with_name("client").about("Run as client").arg(
            Arg::with_name("text").help("Text string to echo back and forth").takes_value(true),
        ))
        .subcommand(SubCommand::with_name("server").about("Run as server"))
}

////////////////////////////////////////////////////////////////////////////////
// Client implementation

async fn exec_client(svc: OvernetProxy, text: Option<&str>) -> Result<(), Error> {
    let mut last_version: u64 = 0;
    loop {
        let (version, peers) = svc.list_peers(last_version).await?;
        last_version = version;
        for mut peer in peers {
            let (s, p) = zx::Channel::create().context("failed to create zx channel")?;
            if let Err(e) = svc.connect_to_service(&mut peer.id, echo::EchoMarker::NAME, s) {
                println!("{:?}", e);
                continue;
            }
            let proxy = fasync::Channel::from_channel(p).context("failed to make async channel")?;
            let cli = echo::EchoProxy::new(proxy);
            println!("Sending {:?} to {:?}", text, peer.id);
            match cli.echo_string(text).await {
                Ok(r) => {
                    println!("SUCCESS: received {:?}", r);
                    return Ok(());
                }
                Err(e) => {
                    println!("ERROR: {:?}", e);
                    continue;
                }
            };
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Server implementation

fn spawn_echo_server(chan: fasync::Channel, quiet: bool) {
    fasync::spawn(
        async move {
            let mut stream = echo::EchoRequestStream::from_channel(chan);
            while let Some(echo::EchoRequest::EchoString { value, responder }) =
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
            .unwrap_or_else(|e: failure::Error| eprintln!("{:?}", e)),
    );
}

async fn next_request(
    stream: &mut ServiceProviderRequestStream,
) -> Result<Option<ServiceProviderRequest>, Error> {
    println!("Awaiting request");
    Ok(stream.try_next().await.context("error running service provider server")?)
}

async fn exec_server(svc: OvernetProxy, quiet: bool) -> Result<(), Error> {
    let (s, p) = zx::Channel::create().context("failed to create zx channel")?;
    let chan = fasync::Channel::from_channel(s).context("failed to make async channel")?;
    let mut stream = ServiceProviderRequestStream::from_channel(chan);
    svc.register_service(echo::EchoMarker::NAME, ClientEnd::new(p))?;
    while let Some(ServiceProviderRequest::ConnectToService {
        chan,
        control_handle: _control_handle,
    }) = next_request(&mut stream).await?
    {
        if !quiet {
            println!("Received service request for service");
        }
        let chan = fasync::Channel::from_channel(chan).context("failed to make async channel")?;
        spawn_echo_server(chan, quiet);
    }
    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// main

fn main() -> Result<(), Error> {
    let args = app().get_matches();

    let mut executor = fasync::Executor::new().context("error creating event loop")?;
    let svc = fuchsia_component::client::connect_to_service::<OvernetMarker>()
        .context("Failed to connect to overnet service")?;

    match args.subcommand() {
        ("server", Some(_)) => {
            executor.run_singlethreaded(exec_server(svc, args.is_present("quiet")))
        }
        ("client", Some(cmd)) => {
            executor.run_singlethreaded(exec_client(svc, cmd.value_of("text")))
        }
        (_, _) => unimplemented!(),
    }
    .map_err(Into::into)
}
