// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::common::{BusConnection, SERVER_READY},
    failure::{format_err, Error, ResultExt},
    fidl_fuchsia_net,
    fidl_fuchsia_netemul_network::{EndpointManagerMarker, NetworkContextMarker},
    fidl_fuchsia_netstack::{InterfaceConfig, IpAddressConfig, NetstackMarker},
    fuchsia_component::client,
    futures::TryStreamExt,
    std::io::{Read, Write},
    std::net::{SocketAddr, TcpListener, TcpStream},
    std::str::FromStr,
};

const PORT: i32 = 8080;
const HELLO_MSG_REQ: &str = "Hello World from Client!";
const HELLO_MSG_RSP: &str = "Hello World from Server!";
const IGNORED_IP_ADDRESS_CONFIG: IpAddressConfig = IpAddressConfig::Dhcp(true);
const DEFAULT_METRIC: u32 = 100;

pub struct ChildOptions {
    pub endpoint: String,
    pub ip: String,
    pub connect_ip: Option<String>,
}

fn publish_server_ready() -> Result<(), Error> {
    let bc = BusConnection::new("server")?;
    bc.publish_code(SERVER_READY)
}

fn run_server(ip: &str) -> Result<(), Error> {
    let addr = format!("{}:{}", ip, PORT);
    let listener = TcpListener::bind(&addr).context(format!("Can't bind to address: {}", addr))?;
    println!("Waiting for connections...");
    let () = publish_server_ready()?;
    let (mut stream, remote) = listener.accept().context("Accept failed")?;
    println!("Accepted connection from {}", remote);
    let mut buffer = [0; 512];
    let rd = stream.read(&mut buffer).context("read failed")?;

    let req = String::from_utf8_lossy(&buffer[0..rd]);
    if req != HELLO_MSG_REQ {
        return Err(format_err!("Got unexpected request from client: {}", req));
    }
    println!("Got request {}", req);
    stream.write(HELLO_MSG_RSP.as_bytes()).context("write failed")?;
    stream.flush().context("flush failed")?;
    Ok(())
}

fn run_client(server_ip: &str) -> Result<(), Error> {
    println!("Connecting to server...");
    let addr = SocketAddr::from_str(&format!("{}:{}", server_ip, PORT))?;
    let mut stream = TcpStream::connect(&addr).context("Tcp connection failed")?;
    let request = HELLO_MSG_REQ.as_bytes();
    stream.write(request)?;
    stream.flush()?;

    let mut buffer = [0; 512];
    let rd = stream.read(&mut buffer)?;
    let rsp = String::from_utf8_lossy(&buffer[0..rd]);
    println!("Got response {}", rsp);
    if rsp != HELLO_MSG_RSP {
        return Err(format_err!("Got unexpected echo from server: {}", rsp));
    }
    Ok(())
}

pub async fn run_child(opt: ChildOptions) -> Result<(), Error> {
    println!("Running child with endpoint '{}'", opt.endpoint);

    // get the network context service:
    let netctx = client::connect_to_service::<NetworkContextMarker>()?;
    // get the endpoint manager
    let (epm, epmch) = fidl::endpoints::create_proxy::<EndpointManagerMarker>()?;
    netctx.get_endpoint_manager(epmch)?;

    // retrieve the created endpoint:
    let ep = epm.get_endpoint(&opt.endpoint).await?;
    let ep = ep.ok_or_else(|| format_err!("can't find endpoint {}", opt.endpoint))?.into_proxy()?;
    // and the ethernet device:
    let eth = ep.get_ethernet_device().await?;

    let if_name = format!("eth-{}", opt.endpoint);
    // connect to netstack:
    let netstack = client::connect_to_service::<NetstackMarker>()?;
    let static_ip =
        opt.ip.parse::<fidl_fuchsia_net_ext::Subnet>().expect("must be able to parse ip");
    println!("static ip = {:?}", static_ip);

    let use_ip = match opt.ip.split("/").next() {
        Some(v) => String::from(v),
        None => opt.ip,
    };

    let mut cfg = InterfaceConfig {
        name: if_name.to_string(),
        filepath: "[TBD]".to_string(),
        metric: DEFAULT_METRIC,
        ip_address_config: IGNORED_IP_ADDRESS_CONFIG,
    };
    let mut if_changed = netstack.take_event_stream().try_filter_map(
        |fidl_fuchsia_netstack::NetstackEvent::OnInterfacesChanged { interfaces }| {
            let iface = interfaces.iter().filter(|iface| iface.name == if_name).next();
            match iface {
                None => futures::future::ok(None),
                Some(a) => {
                    if a.flags & fidl_fuchsia_netstack::NET_INTERFACE_FLAG_UP != 0 {
                        futures::future::ok(Some((a.id, a.hwaddr.clone())))
                    } else {
                        println!("Found interface, but it's down. waiting.");
                        futures::future::ok(None)
                    }
                }
            }
        },
    );
    let nicid =
        netstack.add_ethernet_device(&format!("/vdev/{}", opt.endpoint), &mut cfg, eth).await
            .context("can't add ethernet device")?;
    let () = netstack.set_interface_status(nicid as u32, true)?;
    let fidl_fuchsia_net::Subnet { mut addr, prefix_len } = static_ip.clone().into();
    let _ = netstack.set_interface_address(nicid as u32, &mut addr, prefix_len).await?;

    let (if_id, hwaddr) = if_changed.try_next().await
        .context("wait for interfaces")?
        .ok_or_else(|| format_err!("interface added"))?;

    println!("Found ethernet with id {} {:?}", if_id, hwaddr);

    if let Some(remote) = opt.connect_ip {
        run_client(&remote)
    } else {
        run_server(&use_ip)
    }
}
