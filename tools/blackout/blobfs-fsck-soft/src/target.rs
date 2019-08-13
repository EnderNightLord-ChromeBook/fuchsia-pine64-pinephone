// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Target-side test runner for a power failure test on blobfs, verifying the filesystem with fsck
//! after a soft reboot.
//!
//! This rust handles the blobfs administration for the test, which allows the
//! blobfs-load-generator to be run as a component. It can't be run as a component otherwise
//! because the fs-management library can only be run using /boot/bin/blobfs, but our component has
//! the blobfs binary in /pkg/bin/blobfs.
//!
//! This module also contains tests for the generator, to confirm it is generating load on the
//! filesystem, which in turn exercises the spawning functionality of this runner.

use {
    blackout_target::{CommonCommand, CommonOpts},
    cstr::cstr,
    failure::{bail, Error, ResultExt},
    fdio,
    fs_management::Blobfs,
    fuchsia_zircon::{self as zx, AsHandleRef},
    std::ffi::CString,
    structopt::StructOpt,
};

#[derive(Debug, StructOpt)]
#[structopt(rename_all = "kebab-case")]
struct Opts {
    #[structopt(flatten)]
    common: CommonOpts,
    /// A particular step of the test to perform.
    #[structopt(subcommand)]
    commands: CommonCommand,
}

/// the meat of the test run. the wrapping [`test`] function simply blocks on the process
/// terminating, and will never return except for an error, so it's not suitable for testing the
/// test.
fn test_spawn(blobfs: &mut Blobfs, seed: u64, num_ops: u64) -> Result<zx::Process, Error> {
    let root = format!("/test-fs-root-{}", seed);

    println!("formatting provided block device with blobfs");
    blobfs.format().context("failed to format blobfs")?;

    println!("mounting blobfs into default namespace");
    blobfs.mount(&root).context("failed to mount blobfs")?;

    println!("running load generator");
    launch_generator_process(seed, &root, num_ops)
}

fn test(blobfs: &mut Blobfs, seed: u64) -> Result<(), Error> {
    let process = test_spawn(blobfs, seed, 0)?;

    let _signals = process
        .wait_handle(zx::Signals::PROCESS_TERMINATED, zx::Time::INFINITE)
        .context("failed to wait for blobfs load generator process")?;

    // generator should never terminate. if we get here, something went wrong.
    unreachable!();
}

fn verify(blobfs: &mut Blobfs) -> Result<(), Error> {
    println!("verifying disk with fsck");
    blobfs.fsck().context("failed to run fsck")?;

    println!("verification successful");
    Ok(())
}

fn main() -> Result<(), Error> {
    let opts = Opts::from_args();

    let mut blobfs = Blobfs::new(&opts.common.block_device)?;

    match opts.commands {
        CommonCommand::Test => test(&mut blobfs, opts.common.seed),
        CommonCommand::Verify => verify(&mut blobfs),
    }
}

fn launch_generator_process(seed: u64, root: &str, num_ops: u64) -> Result<zx::Process, Error> {
    let seed_cstring = CString::new(seed.to_string()).context("failed to make seed cstring")?;
    let root_cstring = CString::new(root).context("failed to make mount point cstring")?;
    let num_ops_cstring =
        CString::new(num_ops.to_string()).context("failed to make seed cstring")?;
    let argv = &[
        cstr!("/pkg/bin/blobfs-load-generator"),
        seed_cstring.as_c_str(),
        root_cstring.as_c_str(),
        num_ops_cstring.as_c_str(),
    ];
    match fdio::spawn_etc(
        &zx::Handle::invalid().into(),
        fdio::SpawnOptions::CLONE_ALL,
        argv[0],
        argv,
        None,
        &mut [],
    ) {
        Ok(process) => Ok(process),
        Err((status, message)) => {
            bail!(
                "failed to spawn blobfs-load-generator process: status: {}, message: {}",
                status,
                message
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{test_spawn, verify},
        fs_management::Blobfs,
        fuchsia_zircon::{self as zx, AsHandleRef, Task},
        ramdevice_client::RamdiskClient,
    };

    #[test]
    fn test_verify_no_reboot() {
        // essentially do what the real test will do, but with out a power cycle between test and
        // verify. this checks two things - one, that we expect the test to actually pass in the
        // power failure context (since it passes without it) and two, it narrows down possible
        // failure modes for the next test, which checks that a load was actually generated.

        // 1<<16 blocks with a block size 512 bytes is ~33MB
        println!("creating ramdisk");
        let ramdisk = RamdiskClient::create(512, 1 << 16).expect("failed to make ramdisk");
        let device_path = ramdisk.get_path();

        println!("creating blobfs manager");
        let mut blobfs = Blobfs::new(device_path).expect("failed to create blobfs manager");

        println!("running generator"); // with a "random" seed
        let process = test_spawn(&mut blobfs, 1234, 0).expect("failed to run process");

        std::thread::sleep(std::time::Duration::from_secs(1));

        println!("killing generator");
        process.kill().expect("failed to kill blobfs-load-generator process");

        // test_spawn mounts the blobfs partition; unmount it before we verify
        blobfs.unmount().expect("failed to unmount blobfs");

        println!("running verification on device");
        verify(&mut blobfs).expect("failed to verify blobfs partition");
    }

    #[test]
    fn generator_generated_load() {
        println!("creating ramdisk");
        let ramdisk = RamdiskClient::create(512, 1 << 16).expect("failed to make ramdisk");
        let device_path = ramdisk.get_path();

        println!("creating blobfs manager");
        let mut blobfs = Blobfs::new(device_path).expect("failed to create blobfs manager");

        println!("running generator"); // with a "random" seed
        let process = test_spawn(&mut blobfs, 4321, 1000).expect("failed to run process");

        let _signals = process
            .wait_handle(zx::Signals::PROCESS_TERMINATED, zx::Time::INFINITE)
            .expect("failed to wait for blobfs load generator process");

        println!("remounting blobfs");
        let mount_point = "/test-blobfs-root";
        blobfs.unmount().expect("failed to unmount blobfs the first time");
        blobfs.mount(mount_point).expect("failed to mount blobfs");

        println!("confirming there is at least one file in blobfs");
        let mut dir = std::fs::read_dir(mount_point).expect("failed to read blobfs directory");

        let entry = dir.next().expect("no files in blobfs").expect("failed to get directory entry");
        let typ = entry.file_type().expect("failed to get file type");
        assert!(typ.is_file());

        println!("unmounting blobfs");
        blobfs.unmount().expect("failed to unmount blobfs the second time");
    }
}
