// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await, await_macro)]

use {
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_netemul_environment::ManagedEnvironmentMarker,
    fidl_fuchsia_netemul_sync::{BusMarker, BusProxy, Event, SyncManagerMarker},
    fuchsia_async as fasync,
    fuchsia_component::client,
    fuchsia_syslog::fx_log_info,
    fuchsia_zircon as zx,
    futures::TryStreamExt,
    structopt::StructOpt,
};

#[derive(StructOpt, Debug)]
struct Opt {
    #[structopt(short = "f")]
    fail: bool,
    #[structopt(short = "n", default_value = "root")]
    name: String,
    #[structopt(short = "w")]
    wait: Option<u64>,
    #[structopt(short = "p")]
    publish: Option<i32>,
    #[structopt(short = "e")]
    event: Option<i32>,
    #[structopt(short = "d")]
    look_at_data: bool,
    #[structopt(short = "s")]
    service: Option<String>,
    #[structopt(short = "l")]
    log: Option<String>,
}

const BUS_NAME: &str = "test-bus";

pub struct BusConnection {
    bus: BusProxy,
}

impl BusConnection {
    pub fn new(client: &str) -> Result<BusConnection, Error> {
        let busm = client::connect_to_service::<SyncManagerMarker>()
            .context("SyncManager not available")?;
        let (bus, busch) = fidl::endpoints::create_proxy::<BusMarker>()?;
        busm.bus_subscribe(BUS_NAME, client, busch)?;
        Ok(BusConnection { bus })
    }

    pub async fn publish_code(&self, code: i32) -> Result<(), Error> {
        let () = await!(self.bus.ensure_publish(Event {
            code: Some(code),
            message: None,
            arguments: None,
        }))?;
        Ok(())
    }

    pub async fn wait_for_event(&self, code: i32) -> Result<(), Error> {
        let mut stream = self.bus.take_event_stream().try_filter_map(|event| match event {
            fidl_fuchsia_netemul_sync::BusEvent::OnBusData { data } => match data.code {
                Some(rcv_code) => {
                    if rcv_code == code {
                        futures::future::ok(Some(()))
                    } else {
                        futures::future::ok(None)
                    }
                }
                None => futures::future::ok(None),
            },
            _ => futures::future::ok(None),
        });
        await!(stream.try_next())?;
        Ok(())
    }

    pub async fn perform_bus_ops(
        &self,
        publish: Option<i32>,
        wait: Option<i32>,
    ) -> Result<(), Error> {
        if let Some(code) = wait {
            let () = await!(self.wait_for_event(code))?;
        }
        if let Some(code) = publish {
            let () = await!(self.publish_code(code))?;
        }
        Ok(())
    }
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let opt = Opt::from_args();

    // Attempt to connect to the bus early on so that we do not miss any events.
    //
    // We won't actually unwrap `bus` until later in case this
    // test does not actually need the bus.
    let bus = BusConnection::new(&opt.name);

    if let Some(log) = opt.log {
        println!("Initing syslog...");
        fuchsia_syslog::init_with_tags(&["dummy-proc"])?;

        println!("Logging to syslog: {}", log);
        fx_log_info!("{}", log);
    }

    if let Some(svc) = opt.service {
        println!("Connecting to service [{}]...", svc);
        let env = client::connect_to_service::<ManagedEnvironmentMarker>()?;
        let (_dummy, server) = zx::Channel::create()?;
        env.connect_to_service(&svc, server)?;
    }

    if let Some(wait) = opt.wait {
        println!("Sleeping for {}...", wait);
        std::thread::sleep(std::time::Duration::from_millis(wait));
    }

    let _file = if opt.look_at_data {
        println!("Looking at data...");
        Some(
            std::fs::File::open(std::path::Path::new("/vdata/.THIS_IS_A_VIRTUAL_FS"))
                .context("failed to get vdata")?,
        )
    } else {
        None
    };

    if opt.publish != None || opt.event != None {
        // Unwrap the `bus` which should be an error
        // if the test requires publish or event waiting operations.
        let bus = bus.context("Failed to connect to bus")?;

        println!("Publishing: {:?} | Waiting for event: {:?}", opt.publish, opt.event);
        let () = await!(bus.perform_bus_ops(opt.publish, opt.event))?;
    }

    if opt.fail {
        Err(format_err!("Failing because was asked to."))
    } else {
        println!("All done!");
        Ok(())
    }
}
