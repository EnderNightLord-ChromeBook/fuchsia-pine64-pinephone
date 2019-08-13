// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    failure::{format_err, Error, ResultExt},
    fidl_fidl_test_compatibility::{
        EchoEvent, EchoMarker, EchoProxy, EchoRequest, EchoRequestStream,
    },
    fidl_fuchsia_sys::LauncherProxy,
    fuchsia_async as fasync,
    fuchsia_component::{
        client::{launch, launcher, App},
        server::ServiceFs,
    },
    futures::{StreamExt, TryStreamExt},
};

fn launch_and_connect_to_echo(
    launcher: &LauncherProxy,
    url: String,
) -> Result<(EchoProxy, App), Error> {
    let app = launch(&launcher, url, None)?;
    let echo = app.connect_to_service::<EchoMarker>()?;
    Ok((echo, app))
}

async fn echo_server(stream: EchoRequestStream, launcher: &LauncherProxy) -> Result<(), Error> {
    let handler = move |request| {
        Box::pin(async move {
            match request {
                EchoRequest::EchoStruct { mut value, forward_to_server, responder } => {
                    if !forward_to_server.is_empty() {
                        let (echo, app) = launch_and_connect_to_echo(launcher, forward_to_server)
                            .context("Error connecting to proxy")?;
                        value = echo.echo_struct(&mut value, "").await
                            .context("Error calling echo_struct on proxy")?;
                        drop(app);
                    }
                    responder.send(&mut value).context("Error responding")?;
                }
                EchoRequest::EchoStructNoRetVal {
                    mut value,
                    forward_to_server,
                    control_handle,
                } => {
                    if !forward_to_server.is_empty() {
                        let (echo, app) = launch_and_connect_to_echo(launcher, forward_to_server)
                            .context("Error connecting to proxy")?;
                        echo.echo_struct_no_ret_val(&mut value, "")
                            .context("Error sending echo_struct_no_ret_val to proxy")?;
                        let mut event_stream = echo.take_event_stream();
                        let EchoEvent::EchoEvent { value: response_val } =
                            event_stream.try_next().await
                                .context("Error getting event response from proxy")?
                                .ok_or_else(|| format_err!("Proxy sent no events"))?;
                        value = response_val;
                        drop(app);
                    }
                    control_handle
                        .send_echo_event(&mut value)
                        .context("Error responding with event")?;
                }
                EchoRequest::EchoArrays { mut value, forward_to_server, responder } => {
                    if !forward_to_server.is_empty() {
                        let (echo, app) = launch_and_connect_to_echo(launcher, forward_to_server)
                            .context("Error connecting to proxy")?;
                        value = echo.echo_arrays(&mut value, "").await
                            .context("Error calling echo_arrays on proxy")?;
                        drop(app);
                    }
                    responder.send(&mut value).context("Error responding")?;
                }
                EchoRequest::EchoVectors { mut value, forward_to_server, responder } => {
                    if !forward_to_server.is_empty() {
                        let (echo, app) = launch_and_connect_to_echo(launcher, forward_to_server)
                            .context("Error connecting to proxy")?;
                        value = echo.echo_vectors(&mut value, "").await
                            .context("Error calling echo_vectors on proxy")?;
                        drop(app);
                    }
                    responder.send(&mut value).context("Error responding")?;
                }
                EchoRequest::EchoTable { value: _, forward_to_server: _, responder: _ } => {
                    // Enabling this blows the stack.
                }
                /*
                EchoRequest::EchoTable { mut value, forward_to_server, responder } => {
                    if !forward_to_server.is_empty() {
                        let (echo, app) = launch_and_connect_to_echo(launcher, forward_to_server)
                            .context("Error connecting to proxy")?;
                        value = echo.echo_table(value, "").await
                            .context("Error calling echo_table on proxy")?;
                        drop(app);
                    }
                    responder.send(value)
                        .context("Error responding")?;
                }
                */
                EchoRequest::EchoXunions { mut value, forward_to_server, responder } => {
                    if !forward_to_server.is_empty() {
                        let (echo, app) = launch_and_connect_to_echo(launcher, forward_to_server)
                            .context("Error connecting to proxy")?;
                        value = echo.echo_xunions(&mut value.iter_mut(), "").await
                            .context("Error calling echo_xunions on proxy")?;
                        drop(app);
                    }
                    responder.send(&mut value.iter_mut()).context("Error responding")?;
                }
            }
            Ok(())
        })
    };

    let handle_requests_fut = stream
        .err_into() // change error type from fidl::Error to failure::Error
        .try_for_each_concurrent(None /* max concurrent requests per connection */, handler);

    handle_requests_fut.await
}

fn main() -> Result<(), Error> {
    let mut executor = fasync::Executor::new().context("Error creating executor")?;
    let launcher = launcher().context("Error connecting to application launcher")?;

    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(|stream| stream);
    fs.take_and_serve_directory_handle().context("Error serving directory handle")?;

    let serve_fut = fs.for_each_concurrent(None /* max concurrent connections */, |stream| {
        async {
            if let Err(e) = echo_server(stream, &launcher).await {
                eprintln!("Closing echo server {:?}", e);
            }
        }
    });

    executor.run_singlethreaded(serve_fut);
    Ok(())
}
