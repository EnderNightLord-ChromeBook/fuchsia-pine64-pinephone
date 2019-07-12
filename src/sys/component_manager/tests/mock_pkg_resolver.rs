// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use {
    failure::{Error, ResultExt},
    fdio,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{DirectoryMarker, OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE},
    fidl_fuchsia_pkg as fpkg, fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::{self, macros::*},
    fuchsia_url::pkg_url::PkgUrl,
    fuchsia_zircon::{HandleBased, Status},
    futures::{StreamExt, TryStreamExt},
    std::ffi::CString,
    std::ptr,
};

fn main() -> Result<(), Error> {
    // Ignore argv[0] which is the program binary. Everything else is what
    // needs to be mocked.
    let packages_to_mock: Vec<String> = std::env::args().skip(1).collect();

    fuchsia_syslog::init_with_tags(&["mock_pkg_resolver"]).expect("can't init logger");
    fx_log_info!("starting mock resolver");
    let mut executor = fasync::Executor::new().context("error creating executor")?;
    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(move |stream| {
        let packages_to_mock = packages_to_mock.clone();
        fasync::spawn_local(async move {
            await!(run_resolver_service(stream, packages_to_mock))
                .expect("failed to run echo service")
        });
    });
    fs.take_and_serve_directory_handle()?;
    executor.run_singlethreaded(fs.collect::<()>());
    Ok(())
}

async fn run_resolver_service(
    mut stream: fpkg::PackageResolverRequestStream,
    packages_to_mock: Vec<String>,
) -> Result<(), Error> {
    fx_log_info!("running mock resolver service");
    while let Some(event) = await!(stream.try_next())? {
        let fpkg::PackageResolverRequest::Resolve { package_url, dir, responder, .. } = event;
        let status = await!(resolve(package_url, dir, packages_to_mock.clone()));
        responder.send(Status::from(status).into_raw())?;
        if let Err(s) = status {
            fx_log_err!("request failed: {}", s);
        }
    }
    Ok(())
}

async fn resolve(
    package_url: String,
    dir: ServerEnd<DirectoryMarker>,
    packages_to_mock: Vec<String>,
) -> Result<(), Status> {
    let url = PkgUrl::parse(&package_url).map_err(|_| Err(Status::INVALID_ARGS))?;
    let name = url.name().ok_or_else(|| Err(Status::INVALID_ARGS))?;
    if !packages_to_mock.contains(&name.to_string()) {
        return Err(Status::NOT_FOUND);
    }
    open_in_namespace("/pkg", dir)
}

fn open_in_namespace(path: &str, dir: ServerEnd<DirectoryMarker>) -> Result<(), Status> {
    let mut ns_ptr: *mut fdio::fdio_sys::fdio_ns_t = ptr::null_mut();
    Status::ok(unsafe { fdio::fdio_sys::fdio_ns_get_installed(&mut ns_ptr) })?;
    let cstr = CString::new(path)?;
    Status::ok(unsafe {
        fdio::fdio_sys::fdio_ns_connect(
            ns_ptr,
            cstr.as_ptr(),
            OPEN_RIGHT_READABLE | OPEN_RIGHT_WRITABLE,
            dir.into_channel().into_raw(),
        )
    })?;
    Ok(())
}
