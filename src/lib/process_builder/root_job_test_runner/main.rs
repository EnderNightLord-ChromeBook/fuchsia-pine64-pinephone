// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Run a test binary as a new process under the root job.
//!
//! This is needed since most or all jobs besides the root job have restricted job policy that
//! disallows use of the zx_process_create syscall, which process_builder uses. Processes that use
//! process_builder normally run in the root job, so we need a similar environment for the test.
//!
//! This approach is a temporary hack. It relies on the fact that the sysinfo driver freely hands
//! out handles to the root job through /dev/misc/sysinfo, which is a security hole that will be
//! closed soon.
//!
//! TODO: Figure out a better way to run these tests.

#![feature(async_await, await_macro)]

use {
    failure::{err_msg, format_err, Error, ResultExt},
    fidl::endpoints::ServiceMarker,
    fidl_fidl_examples_echo::EchoMarker,
    fidl_fuchsia_sysinfo as fsysinfo, fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_zircon::{self as zx, HandleBased},
    futures::prelude::*,
    std::env,
    std::ffi::{CStr, CString},
    std::fs,
    std::path::{Path, PathBuf},
};

/// Create a ServiceFs that proxies through limited services from our namespace. Specifically, we
/// don't pass through fuchsia.process.Launcher, which this process needs to use fdio_spawn but
/// which we don't want in the actual test process' namespace to ensure the test process isn't
/// incorrectly using it rather than serving its own.
fn serve_proxy_svc_dir() -> Result<zx::Channel, Error> {
    let mut fs = ServiceFs::new_local();

    // This can be expanded to proxy additional services as needed, though it's not totally clear
    // how to generalize this to loop over a set over markers.
    let echo_path = PathBuf::from(format!("/svc/{}", EchoMarker::NAME));
    if echo_path.exists() {
        fs.add_proxy_service::<EchoMarker, _>();
    }

    let (client, server) = zx::Channel::create().expect("Failed to create channel");
    fs.serve_connection(server)?;
    fasync::spawn_local(fs.collect());
    Ok(client)
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        return Err(err_msg("Usage: root_job_test_runner <test binary> [extra args]"));
    }

    let path = &args[1];
    if !Path::new(path).exists() {
        return Err(format_err!("Test binary '{}' does not exist in namespace", path));
    }

    // Provide the test process with a namespace containing only /pkg and a more limited /svc.
    let svc_str = CString::new("/svc")?;
    let pkg_str = CString::new("/pkg")?;
    let pkg_dir = fs::File::open("/pkg")?;
    let mut actions = vec![
        fdio::SpawnAction::add_namespace_entry(&svc_str, serve_proxy_svc_dir()?.into_handle()),
        fdio::SpawnAction::add_namespace_entry(&pkg_str, fdio::transfer_fd(pkg_dir)?),
    ];

    let root_job = await!(get_root_job())?;
    let argv: Vec<CString> =
        args.into_iter().skip(1).map(CString::new).collect::<Result<_, _>>()?;
    let argv_ref: Vec<&CStr> = argv.iter().map(|a| &**a).collect();
    let options = fdio::SpawnOptions::CLONE_ALL & !fdio::SpawnOptions::CLONE_NAMESPACE;
    let process =
        fdio::spawn_etc(&root_job, options, argv_ref[0], argv_ref.as_slice(), None, &mut actions)
            .map_err(|(status, errmsg)| {
            format_err!("fdio::spawn_etc failed: {}, {}", status, errmsg)
        })?;

    // Wait for the process to terminate. We're hosting it's /svc directory, which will go away if
    // we exit before it.
    await!(fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED))?;

    // Return the same code as the test process, in case it failed.
    let info = process.info()?;
    zx::Process::exit(info.return_code);
}

async fn get_root_job() -> Result<zx::Job, Error> {
    let (proxy, server_end) = fidl::endpoints::create_proxy::<fsysinfo::DeviceMarker>()?;
    fdio::service_connect("/dev/misc/sysinfo", server_end.into_channel())
        .context("Failed to connect to sysinfo servie")?;

    let (status, job) = await!(proxy.get_root_job())?;
    zx::Status::ok(status)?;
    Ok(job.ok_or(err_msg("sysinfo returned OK status but no root job handle!"))?)
}
