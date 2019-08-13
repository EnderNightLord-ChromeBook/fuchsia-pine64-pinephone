// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![recursion_limit = "128"]

use {
    fidl_fuchsia_wlan_common as fidl_common, fidl_fuchsia_wlan_device as fidl_device,
    fidl_fuchsia_wlan_tap as wlantap, fuchsia_async as fasync,
    fuchsia_zircon::prelude::*,
    futures::future::join,
    futures::prelude::*,
    std::sync::{Arc, Mutex},
    wlan_common::{appendable::Appendable, ie, mac, mgmt_writer},
    wlantap_client::Wlantap,
};

mod ap;
mod config;

#[cfg(test)]
mod eth_helper;
#[cfg(test)]
mod minstrel;
#[cfg(test)]
mod test_utils;

const HW_MAC_ADDR: [u8; 6] = [0x67, 0x62, 0x6f, 0x6e, 0x69, 0x6b];
const BSSID: [u8; 6] = [0x62, 0x73, 0x73, 0x62, 0x73, 0x73];

fn create_wlantap_config() -> wlantap::WlantapPhyConfig {
    config::create_wlantap_config(HW_MAC_ADDR, fidl_device::MacRole::Client)
}

fn create_wpa2_psk_rsne() -> wlan_common::ie::rsn::rsne::Rsne {
    use wlan_common::ie::rsn::{akm, cipher, rsne};
    let mut rsne = rsne::Rsne::new();
    rsne.group_data_cipher_suite = Some(cipher::Cipher::new_dot11(cipher::CCMP_128));
    rsne.pairwise_cipher_suites = vec![cipher::Cipher::new_dot11(cipher::CCMP_128)];
    rsne.akm_suites = vec![akm::Akm::new_dot11(akm::PSK)];
    rsne.rsn_capabilities = Some(rsne::RsnCapabilities(0));
    rsne
}

struct State {
    current_channel: fidl_common::WlanChan,
    frame_buf: Vec<u8>,
    is_associated: bool,
}

impl State {
    fn new() -> Self {
        Self {
            current_channel: fidl_common::WlanChan {
                primary: 0,
                cbw: fidl_common::Cbw::Cbw20,
                secondary80: 0,
            },
            frame_buf: vec![],
            is_associated: false,
        }
    }
}

fn send_beacon(
    frame_buf: &mut Vec<u8>,
    channel: &fidl_common::WlanChan,
    bss_id: &[u8; 6],
    ssid: &[u8],
    // TODO(eyw): replace with wlan-common::Protection once it is available.
    use_wpa2_psk: bool,
    proxy: &wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    frame_buf.clear();

    let frame_ctrl = mac::FrameControl(0)
        .with_frame_type(mac::FrameType::MGMT)
        .with_mgmt_subtype(mac::MgmtSubtype::BEACON);
    let seq_ctrl = mac::SequenceControl(0).with_seq_num(123);
    const BROADCAST_ADDR: mac::MacAddr = [0xff; 6];
    mgmt_writer::write_mgmt_hdr(
        frame_buf,
        mgmt_writer::mgmt_hdr_from_ap(frame_ctrl, BROADCAST_ADDR, *bss_id, seq_ctrl),
        None,
    )?;

    frame_buf.append_value(&mac::BeaconHdr {
        timestamp: 0,
        beacon_interval: 100,
        capabilities: mac::CapabilityInfo(0).with_privacy(use_wpa2_psk),
    })?;

    ie::write_ssid(frame_buf, ssid)?;
    ie::write_supported_rates(frame_buf, &[0x82, 0x84, 0x8b, 0x0c, 0x12, 0x96, 0x18, 0x24])?;
    ie::write_dsss_param_set(frame_buf, &ie::DsssParamSet { current_chan: channel.primary })?;
    if use_wpa2_psk {
        create_wpa2_psk_rsne().write_into(frame_buf)?;
    }

    proxy.rx(0, &mut frame_buf.iter().cloned(), &mut create_rx_info(channel))?;
    Ok(())
}

fn send_authentication(
    frame_buf: &mut Vec<u8>,
    channel: &fidl_common::WlanChan,
    bss_id: &[u8; 6],
    proxy: &wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    frame_buf.clear();

    let frame_ctrl = mac::FrameControl(0)
        .with_frame_type(mac::FrameType::MGMT)
        .with_mgmt_subtype(mac::MgmtSubtype::AUTH);
    let seq_ctrl = mac::SequenceControl(0).with_seq_num(123);
    mgmt_writer::write_mgmt_hdr(
        frame_buf,
        mgmt_writer::mgmt_hdr_from_ap(frame_ctrl, HW_MAC_ADDR, *bss_id, seq_ctrl),
        None,
    )?;

    frame_buf.append_value(&mac::AuthHdr {
        auth_alg_num: mac::AuthAlgorithmNumber::OPEN,
        auth_txn_seq_num: 2,
        status_code: mac::StatusCode::SUCCESS,
    })?;

    proxy.rx(0, &mut frame_buf.iter().cloned(), &mut create_rx_info(channel))?;
    Ok(())
}

fn send_association_response(
    frame_buf: &mut Vec<u8>,
    channel: &fidl_common::WlanChan,
    bss_id: &[u8; 6],
    proxy: &wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    frame_buf.clear();

    let frame_ctrl = mac::FrameControl(0)
        .with_frame_type(mac::FrameType::MGMT)
        .with_mgmt_subtype(mac::MgmtSubtype::ASSOC_RESP);
    let seq_ctrl = mac::SequenceControl(0).with_seq_num(123);
    mgmt_writer::write_mgmt_hdr(
        frame_buf,
        mgmt_writer::mgmt_hdr_from_ap(frame_ctrl, HW_MAC_ADDR, *bss_id, seq_ctrl),
        None,
    )?;

    let cap_info = mac::CapabilityInfo(0).with_ess(true).with_short_preamble(true);
    frame_buf.append_value(&mac::AssocRespHdr {
        capabilities: cap_info,
        status_code: mac::StatusCode::SUCCESS,
        aid: 2, // does not matter
    })?;

    // These rates will be captured in assoc_ctx to initialize Minstrel. 11b rates are ignored.
    // tx_vec_idx:                            _     _     _   129   130     _   131   132
    ie::write_supported_rates(frame_buf, &[0x82, 0x84, 0x8b, 0x0c, 0x12, 0x96, 0x18, 0x24])?;
    // tx_vec_idx:                            133 134 basic_135  136
    ie::write_ext_supported_rates(frame_buf, &[48, 72, 128 + 96, 108])?;

    proxy.rx(0, &mut frame_buf.iter().cloned(), &mut create_rx_info(channel))?;
    Ok(())
}

fn create_rx_info(channel: &fidl_common::WlanChan) -> wlantap::WlanRxInfo {
    wlantap::WlanRxInfo {
        rx_flags: 0,
        valid_fields: 0,
        phy: 0,
        data_rate: 0,
        chan: fidl_common::WlanChan {
            // TODO(FIDL-54): use clone()
            primary: channel.primary,
            cbw: channel.cbw,
            secondary80: channel.secondary80,
        },
        mcs: 0,
        rssi_dbm: 0,
        rcpi_dbmh: 0,
        snr_dbh: 0,
    }
}

fn handle_tx(args: wlantap::TxArgs, state: &mut State, proxy: &wlantap::WlantapPhyProxy) {
    if let Some(mac::MacFrame::Mgmt { mgmt_hdr, body, .. }) =
        mac::MacFrame::parse(&args.packet.data[..], false)
    {
        match mac::MgmtBody::parse({ mgmt_hdr.frame_ctrl }.mgmt_subtype(), body) {
            Some(mac::MgmtBody::Authentication { .. }) => {
                println!("authentication received.");
                send_authentication(&mut state.frame_buf, &state.current_channel, &BSSID, proxy)
                    .expect("Error sending fake authentication frame.");
                println!("Authentication sent.");
            }
            Some(mac::MgmtBody::AssociationReq { .. }) => {
                println!("Association Request received.");
                send_association_response(
                    &mut state.frame_buf,
                    &state.current_channel,
                    &BSSID,
                    proxy,
                )
                .expect("Error sending fake association response frame.");
                println!("Association Response sent.");
                state.is_associated = true;
            }
            _ => {}
        }
    }
}

fn main() -> Result<(), failure::Error> {
    let mut exec = fasync::Executor::new().expect("error creating executor");
    let wlantap = Wlantap::open().expect("error with Wlantap::open()");
    let state = Arc::new(Mutex::new(State::new()));
    let proxy = wlantap.create_phy(create_wlantap_config()).expect("error creating wlantap config");

    let event_listener = event_listener(state.clone(), proxy.clone());
    let beacon_timer = beacon_sender(state.clone(), proxy.clone());
    println!("Hardware simlulator started. Try to scan or connect to \"fakenet\"");

    exec.run_singlethreaded(join(event_listener, beacon_timer));
    Ok(())
}

async fn event_listener(state: Arc<Mutex<State>>, proxy: wlantap::WlantapPhyProxy) {
    let mut events = proxy.take_event_stream();
    while let Some(event) = events.try_next().await.unwrap() {
        match event {
            wlantap::WlantapPhyEvent::SetChannel { args } => {
                let mut state = state.lock().unwrap();
                state.current_channel = args.chan;
                println!("setting channel to {:?}", state.current_channel);
            }
            wlantap::WlantapPhyEvent::Tx { args } => {
                let mut state = state.lock().unwrap();
                handle_tx(args, &mut state, &proxy);
            }
            _ => {}
        }
    }
}

async fn beacon_sender(state: Arc<Mutex<State>>, proxy: wlantap::WlantapPhyProxy) {
    let mut beacon_timer_stream = fasync::Interval::new(102_400_000.nanos());
    while let Some(_) = beacon_timer_stream.next().await {
        let state = &mut *state.lock().unwrap();
        if state.current_channel.primary == 6 {
            if !state.is_associated {
                eprintln!("sending beacon!");
            }
            send_beacon(
                &mut state.frame_buf,
                &state.current_channel,
                &BSSID,
                "fakenet".as_bytes(),
                false, // use_wpa2_psk
                &proxy,
            )
            .unwrap();
        }
    }
}

#[cfg(test)]
mod simulation_tests {
    use {
        super::*,
        crate::{ap, eth_helper::create_eth_client, minstrel},
        failure::ensure,
        fidl_fuchsia_wlan_device_service as wlanstack_dev_svc,
        fidl_fuchsia_wlan_service as fidl_wlan_service,
        fuchsia_component::client::connect_to_service,
        fuchsia_zircon_sys,
        futures::channel::mpsc,
        pin_utils::pin_mut,
        std::panic,
        wlan_common::{big_endian::BigEndianU16, buffer_reader::BufferReader, data_writer},
    };

    const BSS_FOO: [u8; 6] = [0x62, 0x73, 0x73, 0x66, 0x6f, 0x6f];
    const SSID_FOO: &[u8] = b"foo";
    const BSS_BAR: [u8; 6] = [0x62, 0x73, 0x73, 0x62, 0x61, 0x72];
    const SSID_BAR: &[u8] = b"bar";
    const BSS_BAZ: [u8; 6] = [0x62, 0x73, 0x73, 0x62, 0x61, 0x7a];
    const SSID_BAZ: &[u8] = b"baz";

    pub const CHANNEL: fidl_common::WlanChan =
        fidl_common::WlanChan { primary: 1, secondary80: 0, cbw: fidl_common::Cbw::Cbw20 };

    // Temporary workaround to run tests synchronously. This is because wlan service only works with
    // one PHY, so having tests with multiple PHYs running in parallel make them flaky.
    #[test]
    fn test_client_and_ap() {
        let mut ok = true;
        // client tests
        ok = run_test("verify_ethernet", test_verify_ethernet) && ok;
        ok = run_test("set_country", test_set_country) && ok;
        ok = run_test("simulate_scan", test_simulate_scan) && ok;
        ok = run_test("connect_to_open_network", test_connect_open) && ok;
        ok = run_test("connect_to_wpa2_network", test_connect_wpa2) && ok;
        ok = run_test("ethernet_tx_rx", test_ethernet_tx_rx) && ok;
        ok = run_test("rate_selection", minstrel::test_rate_selection) && ok;

        // ap tests
        ok = run_test("open_ap_connect", ap::tests::test_open_ap_connect) && ok;
        assert!(ok);
    }

    fn test_verify_ethernet() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");
        // Make sure there is no existing ethernet device.
        let client = exec
            .run_singlethreaded(create_eth_client(&HW_MAC_ADDR))
            .expect(&format!("creating ethernet client: {:?}", &HW_MAC_ADDR));
        assert!(client.is_none());

        // Create wlan_tap device which will in turn create ethernet device.
        let _helper = test_utils::TestHelper::begin_test(&mut exec, create_wlantap_config());
        loop_until_iface_is_found(&mut exec);

        let mut retry = test_utils::RetryWithBackoff::new(5.seconds());
        loop {
            let client = exec
                .run_singlethreaded(create_eth_client(&HW_MAC_ADDR))
                .expect(&format!("creating ethernet client: {:?}", &HW_MAC_ADDR));
            if client.is_some() {
                break;
            }
            let slept = retry.sleep_unless_timed_out();
            assert!(slept, "No ethernet client with mac_addr {:?} found in time", &HW_MAC_ADDR);
        }
    }

    // Issue service.fidl:SetCountry() protocol to Wlanstack's service with a test country code.
    // Test two things:
    //  - If wlantap PHY device received the specified test country code
    //  - If the SetCountry() returned successfully (ZX_OK).
    fn test_set_country() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");
        let mut helper = test_utils::TestHelper::begin_test(&mut exec, create_wlantap_config());
        loop_until_iface_is_found(&mut exec);

        let (mut sender, mut receiver) = mpsc::channel(1);
        let svc = connect_to_service::<wlanstack_dev_svc::DeviceServiceMarker>()
            .expect("Failed to connect to wlanstack_dev_svc");

        let resp = helper
            .run(&mut exec, 1.seconds(), "wlanstack_dev_svc set_country", |_| {}, svc.list_phys())
            .unwrap();
        assert!(resp.phys.len() > 0, "WLAN PHY device is created but ListPhys returned empty.");
        let phy_id = resp.phys[0].phy_id;
        let alpha2 = fake_alpha2();
        let mut req = wlanstack_dev_svc::SetCountryRequest { phy_id, alpha2: alpha2.clone() };

        // Employ a future to make sure this test does not end before WlantanPhyEvent is captured.
        let set_country_fut = set_country_helper(&mut receiver, &svc, &mut req);
        pin_mut!(set_country_fut);

        let _result = helper
            .run(
                &mut exec,
                1.seconds(),
                "wlanstack_dev_svc set_country",
                |event| {
                    match event {
                        wlantap::WlantapPhyEvent::SetCountry { args } => {
                            //  Confirm what was sent down was what was received.
                            assert_eq!(args.alpha2, alpha2);
                            sender
                                .try_send(())
                                .expect("test_set_country confirmed matching alpha2 string");
                        }
                        _ => {}
                    }
                },
                set_country_fut,
            )
            .expect("set_country() failed");
    }

    async fn set_country_helper<'a>(
        receiver: &'a mut mpsc::Receiver<()>,
        svc: &'a wlanstack_dev_svc::DeviceServiceProxy,
        req: &'a mut wlanstack_dev_svc::SetCountryRequest,
    ) -> Result<(), failure::Error> {
        let status = svc.set_country(req).await;
        assert_eq!(status.unwrap(), fuchsia_zircon_sys::ZX_OK);
        receiver.next().await.expect("error receiving set_country_helper mpsc message");
        Ok(())
    }

    fn clear_ssid_and_ensure_iface_gone() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");
        let wlan_service = connect_to_service::<fidl_wlan_service::WlanMarker>()
            .expect("Failed to connect to wlan service");
        exec.run_singlethreaded(wlan_service.clear_saved_networks()).expect("Clearing SSID");

        let mut retry = test_utils::RetryWithBackoff::new(5.seconds());
        loop {
            let status = exec
                .run_singlethreaded(wlan_service.status())
                .expect("error getting status() from wlan_service");
            if status.error.code == fidl_wlan_service::ErrCode::NotFound {
                return;
            }
            let slept = retry.sleep_unless_timed_out();
            assert!(slept, "The interface was not removed in time");
        }
    }

    fn test_simulate_scan() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");
        let mut helper = test_utils::TestHelper::begin_test(&mut exec, create_wlantap_config());

        let wlan_service = connect_to_service::<fidl_wlan_service::WlanMarker>()
            .expect("Failed to connect to wlan service");
        loop_until_iface_is_found(&mut exec);

        let proxy = helper.proxy();
        let scan_result = scan(&mut exec, &wlan_service, &proxy, &mut helper);

        assert_eq!(
            fidl_wlan_service::ErrCode::Ok,
            scan_result.error.code,
            "The error message was: {}",
            scan_result.error.description
        );
        let mut aps: Vec<_> = scan_result
            .aps
            .expect("Got empty scan results")
            .into_iter()
            .map(|ap| (ap.ssid, ap.bssid))
            .collect();
        aps.sort();
        let mut expected_aps = [
            (String::from_utf8_lossy(SSID_FOO).to_string(), BSS_FOO.to_vec()),
            (String::from_utf8_lossy(SSID_BAR).to_string(), BSS_BAR.to_vec()),
            (String::from_utf8_lossy(SSID_BAZ).to_string(), BSS_BAZ.to_vec()),
        ];
        expected_aps.sort();
        assert_eq!(&expected_aps, &aps[..]);
    }

    pub fn loop_until_iface_is_found(exec: &mut fasync::Executor) {
        let wlan_service = connect_to_service::<fidl_wlan_service::WlanMarker>()
            .expect("connecting to wlan service");
        let mut retry = test_utils::RetryWithBackoff::new(5.seconds());
        loop {
            let status = status(exec, &wlan_service);
            if status.error.code != fidl_wlan_service::ErrCode::Ok {
                let slept = retry.sleep_unless_timed_out();
                assert!(slept, "Wlanstack did not recognize the interface in time");
            } else {
                return;
            }
        }
    }

    fn status(
        exec: &mut fasync::Executor,
        wlan_service: &fidl_wlan_service::WlanProxy,
    ) -> fidl_wlan_service::WlanStatus {
        exec.run_singlethreaded(wlan_service.status()).expect("status() should never fail")
    }

    fn scan(
        exec: &mut fasync::Executor,
        wlan_service: &fidl_wlan_service::WlanProxy,
        phy: &wlantap::WlantapPhyProxy,
        helper: &mut test_utils::TestHelper,
    ) -> fidl_wlan_service::ScanResult {
        helper
            .run(
                exec,
                10.seconds(),
                "receive a scan response",
                |event| {
                    if let wlantap::WlantapPhyEvent::SetChannel { args } = event {
                        println!("set channel to {:?}", args.chan);
                        let network = match args.chan.primary {
                            1 => Some((&BSS_FOO, SSID_FOO)),
                            6 => Some((&BSS_BAR, SSID_BAR)),
                            11 => Some((&BSS_BAZ, SSID_BAZ)),
                            _ => None,
                        };
                        if let Some((bssid, ssid)) = network {
                            send_beacon(&mut vec![], &args.chan, bssid, ssid, false, &phy).unwrap();
                        }
                    }
                },
                wlan_service.scan(&mut fidl_wlan_service::ScanRequest { timeout: 5 }),
            )
            .unwrap()
    }

    fn create_connect_config<S: ToString>(
        ssid: &[u8],
        passphrase: S,
    ) -> fidl_wlan_service::ConnectConfig {
        fidl_wlan_service::ConnectConfig {
            ssid: String::from_utf8_lossy(ssid).to_string(),
            pass_phrase: passphrase.to_string(),
            scan_interval: 5,
            bssid: "".to_string(), // BSSID is ignored by wlancfg
        }
    }

    fn create_wpa2_psk_authenticator(
        bssid: &[u8; 6],
        ssid: &[u8],
        passphrase: &str,
    ) -> wlan_rsn::Authenticator {
        use wlan_common::ie::rsn::cipher;
        let nonce_rdr = wlan_rsn::nonce::NonceReader::new(bssid).expect("creating nonce reader");
        let gtk_provider = wlan_rsn::GtkProvider::new(cipher::Cipher::new_dot11(cipher::CCMP_128))
            .expect("creating gtk provider");
        let psk = wlan_rsn::psk::compute(passphrase.as_bytes(), ssid).expect("computing PSK");
        let s_rsne = wlan_rsn::ProtectionInfo::Rsne(create_wpa2_psk_rsne());
        let a_rsne = wlan_rsn::ProtectionInfo::Rsne(create_wpa2_psk_rsne());
        wlan_rsn::Authenticator::new_wpa2psk_ccmp128(
            nonce_rdr,
            std::sync::Arc::new(std::sync::Mutex::new(gtk_provider)),
            psk,
            HW_MAC_ADDR,
            s_rsne,
            *bssid,
            a_rsne,
        )
        .expect("creating authenticator")
    }

    fn process_auth_update(
        updates: &mut wlan_rsn::rsna::UpdateSink,
        channel: &fidl_common::WlanChan,
        bssid: &[u8; 6],
        phy: &wlantap::WlantapPhyProxy,
    ) -> Result<(), failure::Error> {
        use wlan_rsn::rsna::SecAssocUpdate;
        for update in updates {
            if let SecAssocUpdate::TxEapolKeyFrame(frame) = update {
                rx_wlan_data_frame(
                    channel,
                    &HW_MAC_ADDR,
                    bssid,
                    bssid,
                    &frame[..],
                    mac::ETHER_TYPE_EAPOL,
                    phy,
                )?
            }
        }
        Ok(())
    }

    pub fn connect(
        exec: &mut fasync::Executor,
        wlan_service: &fidl_wlan_service::WlanProxy,
        phy: &wlantap::WlantapPhyProxy,
        helper: &mut test_utils::TestHelper,
        ssid: &[u8],
        bssid: &[u8; 6],
        passphrase: Option<&str>,
    ) {
        let mut connect_config = create_connect_config(ssid, passphrase.unwrap_or(&""));
        let mut authenticator = passphrase.map(|p| create_wpa2_psk_authenticator(bssid, ssid, p));
        let connect_fut = wlan_service.connect(&mut connect_config);
        let error = helper
            .run(
                exec,
                10.seconds(),
                &format!("connect to {}({:2x?})", String::from_utf8_lossy(ssid), bssid),
                |event| {
                    handle_connect_events(event, &phy, ssid, bssid, passphrase, &mut authenticator);
                },
                connect_fut,
            )
            .unwrap();
        assert_eq!(error.code, fidl_wlan_service::ErrCode::Ok, "connect failed: {:?}", error);
    }

    fn handle_connect_events(
        event: wlantap::WlantapPhyEvent,
        phy: &wlantap::WlantapPhyProxy,
        ssid: &[u8],
        bssid: &[u8; 6],
        passphrase: Option<&str>,
        authenticator: &mut Option<wlan_rsn::Authenticator>,
    ) {
        match event {
            wlantap::WlantapPhyEvent::SetChannel { args } => {
                println!("channel: {:?}", args.chan);
                if args.chan.primary == CHANNEL.primary {
                    send_beacon(&mut vec![], &args.chan, bssid, ssid, passphrase.is_some(), &phy)
                        .unwrap();
                }
            }
            wlantap::WlantapPhyEvent::Tx { args } => {
                match mac::MacFrame::parse(&args.packet.data[..], false) {
                    Some(mac::MacFrame::Mgmt { mgmt_hdr, body, .. }) => {
                        match mac::MgmtBody::parse({ mgmt_hdr.frame_ctrl }.mgmt_subtype(), body) {
                            Some(mac::MgmtBody::Authentication { .. }) => {
                                send_authentication(&mut vec![], &CHANNEL, bssid, &phy)
                                    .expect("Error sending fake authentication frame.");
                            }
                            Some(mac::MgmtBody::AssociationReq { .. }) => {
                                send_association_response(&mut vec![], &CHANNEL, bssid, &phy)
                                    .expect("Error sending fake association response frame.");
                                if let Some(authenticator) = authenticator {
                                    let mut updates = wlan_rsn::rsna::UpdateSink::default();
                                    authenticator
                                        .initiate(&mut updates)
                                        .expect("initiating authenticator");
                                    process_auth_update(&mut updates, &CHANNEL, bssid, &phy)
                                        .expect(
                                            "processing authenticator updates after initiation",
                                        );
                                }
                            }
                            _ => {}
                        }
                    }
                    // EAPOL frames are transmitted as data frames with LLC protocol being EAPOL
                    Some(mac::MacFrame::Data { .. }) => {
                        let msdus =
                            mac::MsduIterator::from_raw_data_frame(&args.packet.data[..], false)
                                .expect("reading msdu from data frame");
                        for mac::Msdu { llc_frame, .. } in msdus {
                            assert_eq!(
                                llc_frame.hdr.protocol_id.to_native(),
                                mac::ETHER_TYPE_EAPOL
                            );
                            if let Some(authenticator) = authenticator {
                                let mut updates = wlan_rsn::rsna::UpdateSink::default();
                                let mic_size = authenticator.get_negotiated_protection().mic_size;
                                let frame_rx =
                                    eapol::KeyFrameRx::parse(mic_size as usize, llc_frame.body)
                                        .expect("parsing EAPOL frame");
                                authenticator
                                    .on_eapol_frame(&mut updates, eapol::Frame::Key(frame_rx))
                                    .expect("sending EAPOL frame to authenticator");
                                process_auth_update(&mut updates, &CHANNEL, bssid, &phy)
                                    .expect("processing authenticator updates after EAPOL frame");
                            }
                        }
                    }
                    _ => {}
                }
            }
            _ => {}
        }
    }

    fn assert_associated_state(
        status: fidl_wlan_service::WlanStatus,
        bssid: &[u8; 6],
        ssid: &[u8],
        channel: &fidl_common::WlanChan,
        is_secure: bool,
    ) {
        assert_eq!(status.error.code, fidl_wlan_service::ErrCode::Ok);
        assert_eq!(status.state, fidl_wlan_service::State::Associated);
        let ap = status.current_ap.expect("expect to be associated to an AP");
        assert_eq!(ap.bssid, bssid.to_vec());
        assert_eq!(ap.ssid, String::from_utf8_lossy(ssid).to_string());
        assert_eq!(ap.chan, *channel);
        assert!(ap.is_compatible);
        assert_eq!(ap.is_secure, is_secure);
    }

    fn test_connect_open() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");
        let mut helper = test_utils::TestHelper::begin_test(&mut exec, create_wlantap_config());
        loop_until_iface_is_found(&mut exec);

        let wlan_service = connect_to_service::<fidl_wlan_service::WlanMarker>()
            .expect("Failed to connect to wlan service");
        let proxy = helper.proxy();

        connect(&mut exec, &wlan_service, &proxy, &mut helper, SSID_FOO, &BSS_FOO, None);
        let status = status(&mut exec, &wlan_service);
        assert_associated_state(status, &BSS_FOO, SSID_FOO, &CHANNEL, false)
    }

    fn test_connect_wpa2() {
        const BSS_WPA2: &[u8; 6] = b"wpa2ok";
        const SSID_WPA2: &[u8] = b"wpa2ssid";
        let mut exec = fasync::Executor::new().expect("Create executor");
        let wlan_service =
            connect_to_service::<fidl_wlan_service::WlanMarker>().expect("Connect to WLAN service");
        let mut helper =
            test_utils::TestHelper::begin_test(&mut exec, super::create_wlantap_config());
        loop_until_iface_is_found(&mut exec);

        let phy = helper.proxy();
        connect(
            &mut exec,
            &wlan_service,
            &phy,
            &mut helper,
            SSID_WPA2,
            BSS_WPA2,
            Some(&"wpa2good"),
        );

        let status = status(&mut exec, &wlan_service);
        assert_associated_state(status, &BSS_WPA2, SSID_WPA2, &CHANNEL, true);
    }

    const BSS_ETHNET: [u8; 6] = [0x65, 0x74, 0x68, 0x6e, 0x65, 0x74];
    const SSID_ETHERNET: &[u8] = b"ethernet";
    const PAYLOAD: &[u8] = &[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];

    fn test_ethernet_tx_rx() {
        let mut exec = fasync::Executor::new().expect("Failed to create an executor");
        let mut helper = test_utils::TestHelper::begin_test(&mut exec, create_wlantap_config());
        loop_until_iface_is_found(&mut exec);

        let wlan_service = connect_to_service::<fidl_wlan_service::WlanMarker>()
            .expect("Failed to connect to wlan service");

        let proxy = helper.proxy();
        connect(&mut exec, &wlan_service, &proxy, &mut helper, SSID_ETHERNET, &BSS_ETHNET, None);

        let mut client = exec
            .run_singlethreaded(create_eth_client(&HW_MAC_ADDR))
            .expect("cannot create ethernet client")
            .expect(&format!("ethernet client not found {:?}", &HW_MAC_ADDR));

        verify_tx_and_rx(&mut client, &mut exec, &mut helper);
    }

    fn verify_tx_and_rx(
        client: &mut ethernet::Client,
        exec: &mut fasync::Executor,
        helper: &mut test_utils::TestHelper,
    ) {
        let mut buf: Vec<u8> = Vec::new();
        buf.append_value(&mac::EthernetIIHdr {
            da: BSSID,
            sa: HW_MAC_ADDR,
            ether_type: BigEndianU16::from_native(mac::ETHER_TYPE_IPV4),
        })
        .expect("error creating fake ethernet header");
        buf.append_bytes(PAYLOAD).expect("buffer too small for ethernet payload");

        let eth_tx_rx_fut = send_and_receive(client, &buf);
        pin_mut!(eth_tx_rx_fut);

        let phy = helper.proxy();
        let mut actual = Vec::new();
        let (header, payload) = helper
            .run(
                exec,
                5.seconds(),
                "verify ethernet_tx_rx",
                |event| {
                    handle_eth_tx(event, &mut actual, &phy);
                },
                eth_tx_rx_fut,
            )
            .expect("send and receive eth");
        assert_eq!(&actual[..], PAYLOAD);
        assert_eq!(header.da, HW_MAC_ADDR);
        assert_eq!(header.sa, BSSID);
        assert_eq!(header.ether_type.to_native(), mac::ETHER_TYPE_IPV4);
        assert_eq!(&payload[..], PAYLOAD);
    }

    async fn send_and_receive<'a>(
        client: &'a mut ethernet::Client,
        buf: &'a Vec<u8>,
    ) -> Result<(mac::EthernetIIHdr, Vec<u8>), failure::Error> {
        let mut client_stream = client.get_stream();
        client.send(&buf);
        loop {
            let event = client_stream.next().await.expect("receiving ethernet event")?;
            match event {
                ethernet::Event::StatusChanged => {
                    client.get_status().await.expect("getting status");
                }
                ethernet::Event::Receive(rx_buffer, flags) => {
                    ensure!(flags.intersects(ethernet::EthernetQueueFlags::RX_OK), "RX_OK not set");
                    let mut buf = vec![0; rx_buffer.len()];
                    rx_buffer.read(&mut buf);
                    let mut buf_reader = BufferReader::new(&buf[..]);
                    let header = buf_reader
                        .read::<mac::EthernetIIHdr>()
                        .expect("bytes received too short for ethernet header");
                    let payload = buf_reader.into_remaining().to_vec();
                    return Ok((*header, payload));
                }
            }
        }
    }

    fn handle_eth_tx(
        event: wlantap::WlantapPhyEvent,
        actual: &mut Vec<u8>,
        phy: &wlantap::WlantapPhyProxy,
    ) {
        if let wlantap::WlantapPhyEvent::Tx { args } = event {
            if let Some(msdus) =
                mac::MsduIterator::from_raw_data_frame(&args.packet.data[..], false)
            {
                for mac::Msdu { dst_addr, src_addr, llc_frame } in msdus {
                    if dst_addr == BSSID && src_addr == HW_MAC_ADDR {
                        assert_eq!(llc_frame.hdr.protocol_id.to_native(), mac::ETHER_TYPE_IPV4);
                        actual.clear();
                        actual.extend_from_slice(llc_frame.body);
                        rx_wlan_data_frame(
                            &CHANNEL,
                            &HW_MAC_ADDR,
                            &BSS_ETHNET,
                            &BSSID,
                            &PAYLOAD,
                            mac::ETHER_TYPE_IPV4,
                            phy,
                        )
                        .expect("sending wlan data frame");
                    }
                }
            }
        }
    }

    fn rx_wlan_data_frame(
        channel: &fidl_common::WlanChan,
        addr1: &[u8; 6],
        addr2: &[u8; 6],
        addr3: &[u8; 6],
        payload: &[u8],
        ether_type: u16,
        phy: &wlantap::WlantapPhyProxy,
    ) -> Result<(), failure::Error> {
        let buf: &mut Vec<u8> = &mut vec![];

        let frame_ctrl = mac::FrameControl(0)
            .with_frame_type(mac::FrameType::DATA)
            .with_data_subtype(mac::DataSubtype(0))
            .with_from_ds(true);
        let seq_ctrl = mac::SequenceControl(0).with_seq_num(3);

        data_writer::write_data_hdr(
            buf,
            mac::FixedDataHdrFields {
                frame_ctrl,
                duration: 0,
                addr1: *addr1,
                addr2: *addr2,
                addr3: *addr3,
                seq_ctrl,
            },
            mac::OptionalDataHdrFields::none(),
        )?;

        data_writer::write_snap_llc_hdr(buf, ether_type)?;
        buf.append_bytes(payload)?;

        phy.rx(0, &mut buf.iter().cloned(), &mut create_rx_info(channel))?;
        Ok(())
    }

    fn fake_alpha2() -> [u8; 2] {
        let mut alpha2: [u8; 2] = [0, 0];
        alpha2.copy_from_slice("RS".as_bytes());
        alpha2
    }

    fn run_test<F>(name: &str, f: F) -> bool
    where
        F: FnOnce() + panic::UnwindSafe,
    {
        println!("\nTest `{}` started\n", name);
        let result = panic::catch_unwind(f);
        clear_ssid_and_ensure_iface_gone();
        match result {
            Ok(_) => {
                println!("\nTest `{}` passed\n", name);
                true
            }
            Err(_) => {
                println!("\nTest `{}` failed\n", name);
                false
            }
        }
    }
}
