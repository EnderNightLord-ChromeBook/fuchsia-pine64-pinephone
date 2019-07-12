// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{simulation_tests::*, *},
    fidl_fuchsia_wlan_service as fidl_wlan_service, fidl_fuchsia_wlan_tap as wlantap,
    fuchsia_async as fasync, fuchsia_component as app,
    futures::{channel::mpsc, poll},
    pin_utils::pin_mut,
    std::collections::HashMap,
    std::task::Poll,
    wlan_common::{appendable::Appendable, big_endian::BigEndianU16, mac},
};

// Remedy for FLK-24 (DNO-389)
// Refer to |KMinstrelUpdateIntervalForHwSim| in //src/connectivity/wlan/drivers/wlan/device.cpp
const DATA_FRAME_INTERVAL_NANOS: i64 = 4_000_000;

const BSS_MINSTL: [u8; 6] = [0x6d, 0x69, 0x6e, 0x73, 0x74, 0x0a];
const SSID_MINSTREL: &[u8] = b"minstrel";

pub fn test_rate_selection() {
    let mut exec = fasync::Executor::new().expect("error creating executor");
    let wlan_service = app::client::connect_to_service::<fidl_wlan_service::WlanMarker>()
        .expect("Error connecting to wlan service");
    let mut helper = test_utils::TestHelper::begin_test(
        &mut exec,
        wlantap::WlantapPhyConfig { quiet: true, ..create_wlantap_config() },
    );
    loop_until_iface_is_found(&mut exec);

    let phy = helper.proxy();
    connect(&mut exec, &wlan_service, &phy, &mut helper, SSID_MINSTREL, &BSS_MINSTL);

    let (sender, mut receiver) = mpsc::channel(1);
    let eth_and_beacon_sender_fut = eth_and_beacon_sender(&mut receiver, &phy);
    pin_mut!(eth_and_beacon_sender_fut);

    // Simulated hardware supports 8 ERP tx vectors with idx 129 to 136, both inclusive.
    // (see `fn send_association_response(...)`)
    const MUST_USE_IDX: &[u16] = &[129, 130, 131, 132, 133, 134, 136]; // Missing 135 is OK.
    const ALL_SUPPORTED_IDX: &[u16] = &[129, 130, 131, 132, 133, 134, 135, 136];
    const ERP_STARTING_IDX: u16 = 129;
    const MAX_SUCCESSFUL_IDX: u16 = 130;

    // Only the lowest ones can succeed in the simulated environment.
    let will_succeed = |idx| ERP_STARTING_IDX <= idx && idx <= MAX_SUCCESSFUL_IDX;

    let mut tx_vec_count_map = HashMap::new();
    let mut max_key_prev = std::u16::MAX;
    let mut second_max_key_prev = max_key_prev;
    let mut max_val_prev = 0u64;
    let mut second_max_val_prev = 0u64;

    // Check for convergence by taking snapshots. A snapshot is taken when "100
    // frames (~4 minstrel cycles) are sent with the same tx vector index". we
    // check the most used and the second most used tx vectors (aka data rates)
    // since last snapshot.
    //
    // Once Minstrel converges, the optimal tx vector will remain unchanged for
    // the rest of the time. And due to its "probing" mechanism, the second most
    // used tx vector will also be used regularly (but much less frequently) to
    // make sure it is still viable.
    let mut is_converged = |hm: &HashMap<u16, u64>| {
        // Due to its randomness, Minstrel may skip 135. But the other 7 must be present.
        if hm.keys().len() < MUST_USE_IDX.len() {
            return false;
        }
        // safe to unwrap below because there are at least 7 entries
        let max_key = hm.keys().max_by_key(|k| hm[&k]).unwrap();
        let (second_max_key, second_max_val) =
            hm.iter().filter(|(k, _v)| k != &max_key).max_by_key(|(_k, v)| *v).unwrap();
        let max_val = hm[&max_key];

        let is_converged = if !(max_key == &max_key_prev && second_max_key == &second_max_key_prev)
        {
            // optimal values changed, not stabilized yet
            false
        } else {
            // One tx vector has become dominant as the number of data frames transmitted with it
            // is at least 15 times as large as anyone else. 15 is the number of non-probing data
            // frames between 2 consecutive probing data frames.
            const NORMAL_FRAMES_PER_PROBE: u64 = 15;
            let diff_max = max_val - max_val_prev;
            let diff_second_max = second_max_val - second_max_val_prev;
            // second_max is used regularly (> 1) but still much less frequently than the dominant.
            (diff_second_max > 1) && (diff_max >= NORMAL_FRAMES_PER_PROBE * diff_second_max)
        };

        // Take a snapshot every once in a while to reduce the effect of early fluctuations.
        // This makes determining convergence more reliable.
        if max_val % 100 == 0 {
            println!("{:?}", hm);
            max_key_prev = *max_key;
            second_max_key_prev = *second_max_key;
            max_val_prev = max_val;
            second_max_val_prev = *second_max_val;
        }
        is_converged
    };
    helper
        .run(
            &mut exec,
            30.seconds(),
            "verify rate selection converges to 130",
            |event| {
                handle_rate_selection_event(
                    event,
                    &phy,
                    &BSS_MINSTL,
                    &mut tx_vec_count_map,
                    will_succeed,
                    &mut is_converged,
                    sender.clone(),
                );
            },
            eth_and_beacon_sender_fut,
        )
        .expect("running main future");

    let total = tx_vec_count_map.values().sum::<u64>();
    println!("final tx vector counts:\n{:#?}\ntotal: {}", tx_vec_count_map, total);
    assert!(tx_vec_count_map.contains_key(&MAX_SUCCESSFUL_IDX));
    let others = total - tx_vec_count_map[&MAX_SUCCESSFUL_IDX];
    println!("others: {}", others);
    let mut tx_vec_idx_seen: Vec<_> = tx_vec_count_map.keys().cloned().collect();
    tx_vec_idx_seen.sort();
    if tx_vec_idx_seen.len() == MUST_USE_IDX.len() {
        // 135 may not be attempted due to randomness and it is OK.
        assert_eq!(&tx_vec_idx_seen[..], MUST_USE_IDX);
    } else {
        assert_eq!(&tx_vec_idx_seen[..], ALL_SUPPORTED_IDX);
    }
    println!(
        "If the test fails due to QEMU slowness outside of the scope of WLAN (See FLK-24, \
         DNO-389). Try increasing |DATA_FRAME_INTERVAL_NANOS| above."
    );
    assert_eq!(max_key_prev, MAX_SUCCESSFUL_IDX);
}

fn handle_rate_selection_event<F, G>(
    event: wlantap::WlantapPhyEvent,
    phy: &wlantap::WlantapPhyProxy,
    bssid: &[u8; 6],
    hm: &mut HashMap<u16, u64>,
    should_succeed: F,
    is_converged: &mut G,
    mut sender: mpsc::Sender<bool>,
) where
    F: Fn(u16) -> bool,
    G: FnMut(&HashMap<u16, u64>) -> bool,
{
    match event {
        wlantap::WlantapPhyEvent::Tx { args } => {
            if let Some(mac::MacFrame::Data { .. }) =
                mac::MacFrame::parse(&args.packet.data[..], false)
            {
                let tx_vec_idx = args.packet.info.tx_vector_idx;
                send_tx_status_report(*bssid, tx_vec_idx, should_succeed(tx_vec_idx), phy)
                    .expect("Error sending tx_status report");
                let count = hm.entry(tx_vec_idx).or_insert(0);
                *count += 1;
                if *count == 1 {
                    println!("new tx_vec_idx: {} at #{}", tx_vec_idx, hm.values().sum::<u64>());
                }
                sender.try_send(is_converged(hm)).expect("sending message to ethernet sender");
            }
        }
        _ => {}
    }
}

async fn eth_and_beacon_sender<'a>(
    receiver: &'a mut mpsc::Receiver<bool>,
    phy: &'a wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    let mut client = await!(create_eth_client(&HW_MAC_ADDR))
        .expect("cannot create ethernet client")
        .expect(&format!("ethernet client not found {:?}", &HW_MAC_ADDR));

    let mut buf: Vec<u8> = vec![];
    buf.append_value(&mac::EthernetIIHdr {
        da: BSSID,
        sa: HW_MAC_ADDR,
        ether_type: BigEndianU16::from_native(mac::ETHER_TYPE_IPV4),
    })
    .expect("error creating fake ethernet header");

    let mut timer_stream = fasync::Interval::new(DATA_FRAME_INTERVAL_NANOS.nanos());
    let mut intervals_since_last_beacon: i64 = i64::max_value() / DATA_FRAME_INTERVAL_NANOS;
    let mut client_stream = client.get_stream();
    loop {
        await!(timer_stream.next());

        // auto-deauthentication timeout is 10.24 seconds.
        // Send a beacon before that to stay connected.
        if (intervals_since_last_beacon * DATA_FRAME_INTERVAL_NANOS).nanos() >= 8765.millis() {
            intervals_since_last_beacon = 0;
            send_beacon(&mut vec![], &CHANNEL, &BSS_MINSTL, SSID_MINSTREL, &phy).unwrap();
        }
        intervals_since_last_beacon += 1;

        client.send(&buf);
        if let Poll::Ready(Some(Ok(ethernet::Event::StatusChanged))) = poll!(client_stream.next()) {
            println!("status changed to: {:?}", await!(client.get_status())?);
            // There was an event waiting, ethernet frames have NOT been sent on the previous poll.
            let _ = poll!(client_stream.next());
        }
        let converged = await!(receiver.next()).expect("error receiving channel message");
        if converged {
            break;
        }
    }
    Ok(())
}

fn create_wlan_tx_status_entry(tx_vec_idx: u16) -> wlantap::WlanTxStatusEntry {
    fidl_fuchsia_wlan_tap::WlanTxStatusEntry { tx_vec_idx: tx_vec_idx, attempts: 1 }
}

fn send_tx_status_report(
    bssid: [u8; 6],
    tx_vec_idx: u16,
    is_successful: bool,
    proxy: &wlantap::WlantapPhyProxy,
) -> Result<(), failure::Error> {
    use fidl_fuchsia_wlan_tap::WlanTxStatus;

    let mut ts = WlanTxStatus {
        peer_addr: bssid,
        success: is_successful,
        tx_status_entries: [
            create_wlan_tx_status_entry(tx_vec_idx),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
            create_wlan_tx_status_entry(0),
        ],
    };
    proxy.report_tx_status(0, &mut ts)?;
    Ok(())
}
