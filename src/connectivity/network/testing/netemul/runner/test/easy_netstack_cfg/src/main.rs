// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_net_stack::StackMarker,
    fidl_fuchsia_netemul_sync::{BusMarker, BusProxy, SyncManagerMarker},
    fuchsia_async as fasync,
    fuchsia_component::client,
    fuchsia_syslog::fx_log_info,
    std::io::{Read, Write},
    std::net::{SocketAddr, TcpListener, TcpStream},
    structopt::StructOpt,
};

const BUS_NAME: &str = "test-bus";
const SERVER_NAME: &str = "server";
const CLIENT_NAME: &str = "client";
const HELLO_MSG_REQ: &str = "Hello World from Client!";
const HELLO_MSG_RSP: &str = "Hello World from Server!";
const SERVER_IP: &str = "192.168.0.1";
const PORT: i32 = 8080;

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

    pub async fn wait_for_client(&mut self, expect: &'static str) -> Result<(), Error> {
        let _ = self.bus.wait_for_clients(&mut vec![expect].drain(..), 0).await?;
        Ok(())
    }
}

async fn run_server() -> Result<(), Error> {
    let listener =
        TcpListener::bind(&format!("{}:{}", SERVER_IP, PORT)).context("Can't bind to address")?;
    fx_log_info!("Waiting for connections...");

    let _bus = BusConnection::new(SERVER_NAME)?;

    let (mut stream, remote) = listener.accept().context("Accept failed")?;
    fx_log_info!("Accepted connection from {}", remote);
    let mut buffer = [0; 512];
    let rd = stream.read(&mut buffer).context("read failed")?;

    let req = String::from_utf8_lossy(&buffer[0..rd]);
    if req != HELLO_MSG_REQ {
        return Err(format_err!("Got unexpected request from client: {}", req));
    }
    fx_log_info!("Got request {}", req);
    stream.write(HELLO_MSG_RSP.as_bytes()).context("write failed")?;
    stream.flush().context("flush failed")?;
    Ok(())
}

async fn run_client(gateway: Option<String>) -> Result<(), Error> {
    if let Some(gateway) = gateway {
        let gw_addr: fidl_fuchsia_net::IpAddress = fidl_fuchsia_net_ext::IpAddress(
            gateway.parse::<std::net::IpAddr>().context("failed to parse gateway address")?,
        )
        .into();
        test_gateway(gw_addr).await.context("test_gateway failed")?;
    }

    fx_log_info!("Waiting for server...");
    let mut bus = BusConnection::new(CLIENT_NAME)?;
    let () = bus.wait_for_client(SERVER_NAME).await?;
    fx_log_info!("Connecting to server...");
    let addr: SocketAddr = format!("{}:{}", SERVER_IP, PORT).parse()?;
    let mut stream = TcpStream::connect(&addr).context("Tcp connection failed")?;
    let request = HELLO_MSG_REQ.as_bytes();
    stream.write(request)?;
    stream.flush()?;

    let mut buffer = [0; 512];
    let rd = stream.read(&mut buffer)?;
    let rsp = String::from_utf8_lossy(&buffer[0..rd]);
    fx_log_info!("Got response {}", rsp);
    if rsp != HELLO_MSG_RSP {
        return Err(format_err!("Got unexpected echo from server: {}", rsp));
    }
    Ok(())
}

async fn test_gateway(gw_addr: fidl_fuchsia_net::IpAddress) -> Result<(), Error> {
    let stack =
        client::connect_to_service::<StackMarker>().context("failed to connect to netstack")?;
    let response =
        stack.get_forwarding_table().await.context("failed to call get_forwarding_table")?;
    let found = response.iter().any(|entry| {
        let fidl_fuchsia_net_ext::IpAddress(entry_addr) = entry.subnet.addr.into();
        if let fidl_fuchsia_net_stack::ForwardingDestination::NextHop(gw) = entry.destination {
            entry_addr.is_unspecified() && entry.subnet.prefix_len == 0 && gw == gw_addr
        } else {
            false
        }
    });
    if found {
        fx_log_info!("Found default route for gateway");
        Ok(())
    } else {
        let fidl_fuchsia_net_ext::IpAddress(gw) = gw_addr.into();
        let unspecified = match gw {
            std::net::IpAddr::V4(_) => std::net::IpAddr::V4(std::net::Ipv4Addr::UNSPECIFIED),
            std::net::IpAddr::V6(_) => std::net::IpAddr::V6(std::net::Ipv6Addr::UNSPECIFIED),
        };
        Err(format_err!("could not find {}/0 next hop {} in {:?}", unspecified, gw, response))
    }
}

#[derive(StructOpt, Debug)]
struct Opt {
    #[structopt(short = "c")]
    is_child: bool,
    #[structopt(short = "g")]
    gateway: Option<String>,
}

fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["easy-netstack-cfg"])?;
    fx_log_info!("Started");

    let opt = Opt::from_args();
    let mut executor = fasync::Executor::new().context("Error creating executor")?;
    executor.run_singlethreaded(async {
        if opt.is_child {
            run_client(opt.gateway).await
        } else {
            run_server().await
        }
    })
}
