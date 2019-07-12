// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use failure::{Error, ResultExt};
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::prelude::*;
use std::env;
use std::fs::File;

fn main() -> Result<(), Error> {
    let vmo = zx::Vmo::create(256 * ethernet::DEFAULT_BUFFER_SIZE as u64)?;

    let mut executor = fasync::Executor::new().context("could not create executor")?;

    let path = env::args().nth(1).expect("missing device argument");
    let dev = File::open(path)?;
    let fut = async {
        let client =
            await!(ethernet::Client::from_file(dev, vmo, ethernet::DEFAULT_BUFFER_SIZE, "eth-rs"))?;
        println!("created client {:?}", client);
        println!("info: {:?}", await!(client.info())?);
        println!("status: {:?}", await!(client.get_status())?);
        await!(client.start())?;
        await!(client.tx_listen_start())?;

        let mut events = client.get_stream();
        while let Some(evt) = await!(events.try_next())? {
            if let ethernet::Event::Receive(rx, flags) = evt {
                let mut buf = [0; 64];
                let r = rx.read(&mut buf);
                let is_tx_echo = flags.intersects(ethernet::EthernetQueueFlags::TX_ECHO);
                println!(
                    "{} first {} bytes:\n{:02x?}",
                    if is_tx_echo { "TX_ECHO" } else { "RX     " },
                    r,
                    &buf[0..r]
                );
            }
        }
        Ok(())
    };
    executor.run_singlethreaded(fut)
}
