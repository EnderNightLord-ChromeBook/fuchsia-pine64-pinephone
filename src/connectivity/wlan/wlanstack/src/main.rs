// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! System service for wireless networking

#![feature(async_await)]
#![deny(missing_docs)]
#![recursion_limit = "256"]

mod device;
mod device_watch;
mod fidl_util;
mod future_util;
mod inspect;
mod logger;
mod mlme_query_proxy;
mod service;
mod station;
mod stats_scheduler;
mod telemetry;
mod watchable_map;
mod watcher_service;

use failure::Error;
use fidl_fuchsia_wlan_device_service::DeviceServiceRequestStream;
use fuchsia_async as fasync;
use fuchsia_cobalt::{CobaltConnector, CobaltSender, ConnectionType};
use fuchsia_component::server::{ServiceFs, ServiceObjLocal};
use futures::future::{try_join, try_join5};
use futures::prelude::*;
use log::info;
use std::sync::Arc;
use structopt::StructOpt;
use wlan_sme;

use crate::device::{IfaceDevice, IfaceMap, PhyDevice, PhyMap};
use crate::watcher_service::WatcherService;

const MAX_LOG_LEVEL: log::LevelFilter = log::LevelFilter::Info;

const CONCURRENT_LIMIT: usize = 1000;

static LOGGER: logger::Logger = logger::Logger;

/// Configuration for wlanstack service.
/// This configuration is a super set of individual component configurations such as SME.
#[derive(StructOpt, Clone, Debug, Default)]
pub struct ServiceCfg {
    /// |true| if WEP should be supported by the service instance.
    #[structopt(long = "wep_supported")]
    pub wep_supported: bool,
    /// |true| if legacy WPA1 should be supported by the service instance.
    #[structopt(long = "wpa1_supported")]
    pub wpa1_supported: bool,
    /// |true| if devices are spawned in an isolated devmgr and device_watcher should watch devices
    /// in the isolated devmgr (for wlan-hw-sim based tests)
    #[structopt(long = "isolated_devmgr")]
    pub isolated_devmgr: bool,
}

impl From<ServiceCfg> for wlan_sme::Config {
    fn from(cfg: ServiceCfg) -> Self {
        Self { wep_supported: cfg.wep_supported, wpa1_supported: cfg.wpa1_supported }
    }
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    log::set_logger(&LOGGER)?;
    log::set_max_level(MAX_LOG_LEVEL);

    info!("Starting");
    let cfg = ServiceCfg::from_args();
    let mut fs = ServiceFs::new_local();
    let inspect_tree =
        Arc::new(inspect::WlanstackTree::new(&mut fs).expect("fail creating Inspect tree"));
    fs.dir("svc").add_fidl_service(IncomingServices::Device);

    let (phys, phy_events) = device::PhyMap::new();
    let (ifaces, iface_events) = device::IfaceMap::new();
    let phys = Arc::new(phys);
    let ifaces = Arc::new(ifaces);

    let phy_server = device::serve_phys(phys.clone(), cfg.isolated_devmgr).map_ok(|x| match x {});
    let (cobalt_sender, cobalt_reporter) = CobaltConnector::default()
        .serve(ConnectionType::project_name(wlan_metrics_registry::PROJECT_NAME));
    let telemetry_server =
        telemetry::report_telemetry_periodically(ifaces.clone(), cobalt_sender.clone());
    // TODO(WLAN-927): Remove once drivers support SME channel.
    let iface_server = device::serve_ifaces(
        cfg.clone(),
        ifaces.clone(),
        cobalt_sender.clone(),
        inspect_tree.clone(),
        cfg.isolated_devmgr,
    )
    .map_ok(|x| match x {});
    let (watcher_service, watcher_fut) =
        watcher_service::serve_watchers(phys.clone(), ifaces.clone(), phy_events, iface_events);
    let serve_fidl_fut =
        serve_fidl(cfg, fs, phys, ifaces, watcher_service, inspect_tree, cobalt_sender);
    let services_server = try_join(serve_fidl_fut, watcher_fut);

    try_join5(
        services_server,
        phy_server,
        iface_server,
        cobalt_reporter.map(Ok),
        telemetry_server.map(Ok),
    ).await?;
    Ok(())
}

enum IncomingServices {
    Device(DeviceServiceRequestStream),
}

async fn serve_fidl(
    cfg: ServiceCfg,
    mut fs: ServiceFs<ServiceObjLocal<'_, IncomingServices>>,
    phys: Arc<PhyMap>,
    ifaces: Arc<IfaceMap>,
    watcher_service: WatcherService<PhyDevice, IfaceDevice>,
    inspect_tree: Arc<inspect::WlanstackTree>,
    cobalt_sender: CobaltSender,
) -> Result<(), Error> {
    fs.take_and_serve_directory_handle()?;

    let fdio_server = fs.for_each_concurrent(CONCURRENT_LIMIT, move |s| {
        let phys = phys.clone();
        let ifaces = ifaces.clone();
        let watcher_service = watcher_service.clone();
        let cobalt_sender = cobalt_sender.clone();
        let cfg = cfg.clone();
        let inspect_tree = inspect_tree.clone();
        async move {
            match s {
                IncomingServices::Device(stream) => service::serve_device_requests(
                    service::IfaceCounter::new(),
                    cfg,
                    phys,
                    ifaces,
                    watcher_service,
                    stream,
                    inspect_tree,
                    cobalt_sender,
                )
                .unwrap_or_else(|e| println!("{:?}", e)).await,
            }
        }
    });
    fdio_server.await;
    Ok(())
}

#[cfg(test)]
mod tests {
    use {super::*, std::panic};

    #[test]
    fn parse_svc_cfg_wep() {
        let cfg = ServiceCfg::from_iter(vec!["bin/app", "--wep_supported"].iter());
        assert!(cfg.wep_supported);
    }

    #[test]
    fn parse_svc_cfg_default() {
        let cfg = ServiceCfg::from_iter(vec!["bin/app"].iter());
        assert!(!cfg.wep_supported);
    }

    #[test]
    fn svc_to_sme_cfg() {
        let svc_cfg = ServiceCfg::from_iter(vec!["bin/app"].iter());
        let sme_cfg: wlan_sme::Config = svc_cfg.into();
        assert!(!sme_cfg.wep_supported);

        let svc_cfg = ServiceCfg::from_iter(vec!["bin/app", "--wep_supported"].iter());
        let sme_cfg: wlan_sme::Config = svc_cfg.into();
        assert!(sme_cfg.wep_supported);
    }
}
