// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

// Explicitly added due to conflict using custom_attribute and async_await above.
#[macro_use]
extern crate serde_derive;

mod opts;

use {
    crate::opts::Opt,
    connectivity_testing::http_service_util,
    connectivity_testing::net_stack_util::netstack_did_get_dhcp,
    connectivity_testing::wlan_service_util,
    failure::{bail, format_err, Error, ResultExt},
    fidl_fuchsia_net_oldhttp::{HttpServiceMarker, HttpServiceProxy},
    fidl_fuchsia_net_stack::StackMarker,
    fidl_fuchsia_wlan_device_service::{DeviceServiceMarker, DeviceServiceProxy},
    fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_async::{DurationExt, Executor, TimeoutExt},
    fuchsia_component::client::connect_to_service,
    fuchsia_syslog::{self as syslog, fx_log_info, fx_log_warn},
    fuchsia_zircon::DurationNum,
    std::collections::HashMap,
    std::process,
    std::{thread, time},
    structopt::StructOpt,
};

#[allow(dead_code)]
type WlanService = DeviceServiceProxy;

// Until we have the URL as an optional parameter for the test, use this.
const URL_STRING: &str = &"http://ovh.net/files/1Mb.dat";
const WLAN_CONNECT_TIMEOUT_SECONDS: i64 = 30;

fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["wlan-smoke-test"]).expect("should not fail");

    let opt = Opt::from_args();
    fx_log_info!("{:?}", opt);

    // create objects to hold test objects and results
    let mut test_results: TestResults = Default::default();

    let mut test_pass = true;
    if let Err(e) = run_test(opt, &mut test_results) {
        test_pass = false;
        test_results.error_message = e.to_string();
    }

    report_results(&mut test_results);

    if !test_pass {
        process::exit(1);
    }

    Ok(())
}

fn run_test(opt: Opt, test_results: &mut TestResults) -> Result<(), Error> {
    let mut test_pass = false;
    let mut exec = Executor::new().context("error creating event loop")?;
    let wlan_svc =
        connect_to_service::<DeviceServiceMarker>().context("Failed to connect to wlan_service")?;
    test_results.connect_to_wlan_service = true;

    let http_svc = connect_to_service::<HttpServiceMarker>()?;
    test_results.connect_to_http_service = true;

    let network_svc = connect_to_service::<StackMarker>()?;
    test_results.connect_to_netstack_service = true;

    let fut = async {
        let wlan_iface_ids = wlan_service_util::get_iface_list(&wlan_svc)
            .await
            .context("wlan-smoke-test: failed to query wlanservice iface list")?;
        test_results.query_wlan_service_iface_list = true;

        if wlan_iface_ids.is_empty() {
            bail!("Did not find wlan interfaces");
        };
        test_results.wlan_discovered = true;
        // note: interface discovery is marked false at the time of failure
        test_results.interface_status = true;

        for iface in wlan_iface_ids {
            let sme_proxy = wlan_service_util::get_iface_sme_proxy(&wlan_svc, iface).await?;
            let status_response = match sme_proxy.status().await {
                Ok(status) => status,
                Err(_) => {
                    test_results.interface_status = false;
                    continue;
                }
            };

            let iface_object = WlanIface::new(sme_proxy, status_response);

            test_results.iface_objects.insert(iface, iface_object);
        }

        // now that we have interfaces...  let's try to use them!
        for (iface_id, wlan_iface) in test_results.iface_objects.iter_mut() {
            // first check if we can get scan results
            let scan_result = wlan_service_util::perform_scan(&wlan_iface.sme_proxy).await;
            match scan_result {
                Ok(results) => {
                    wlan_iface.scan_success = true;
                    for entry in results.into_iter() {
                        if entry.best_bss.ssid == opt.target_ssid.as_bytes().to_vec() {
                            wlan_iface.scan_found_target_ssid = true;
                        }
                    }
                }
                _ => fx_log_warn!("scan failed"),
            };

            let mut requires_disconnect = false;
            // first check if we are connected to the target network already
            if is_connect_to_target_network_needed(
                opt.stay_connected,
                &opt.target_ssid,
                &wlan_iface.initial_status,
            ) {
                let connect_result = wlan_service_util::connect_to_network(
                    &wlan_iface.sme_proxy,
                    opt.target_ssid.as_bytes().to_vec(),
                    opt.target_pwd.as_bytes().to_vec(),
                )
                // TODO(WLAN-1268): when this bug is fixed, consider removing this timeout
                .on_timeout(WLAN_CONNECT_TIMEOUT_SECONDS.seconds().after_now(), || {
                    Err(format_err!("connect did not complete in time"))
                })
                .await;

                match connect_result {
                    Ok(true) => {
                        wlan_iface.connection_success = true;
                        requires_disconnect = true;
                    }
                    Err(e) => {
                        fx_log_warn!("error connecting: {}", e);
                        continue;
                    }
                    _ => continue,
                };
            } else {
                // connection already established, mark as successful
                wlan_iface.connection_success = true;
            }

            let mut dhcp_check_attempts = 0;
            let mac_addr = match wlan_service_util::get_wlan_mac_addr(&wlan_svc, *iface_id).await {
                Ok(addr) => addr,
                Err(e) => {
                    fx_log_warn!("error getting mac address: {}", e);
                    continue;
                }
            };

            while dhcp_check_attempts < 3 && !wlan_iface.dhcp_success {
                wlan_iface.dhcp_success = match netstack_did_get_dhcp(&network_svc, &mac_addr).await
                {
                    Ok(result) => result,
                    Err(e) => {
                        fx_log_warn!("error getting iface address from netstack: {}", e);
                        continue;
                    }
                };
                if !wlan_iface.dhcp_success {
                    // dhcp takes some time...  loop again to give it a chance
                    dhcp_check_attempts += 1;
                    thread::sleep(time::Duration::from_millis(4000));
                }
            }

            // if we got an ip addr, go ahead and check a download
            if wlan_iface.dhcp_success {
                // TODO(NET-1095): add ping check to verify connectivity
                for _ in 0..3 {
                    wlan_iface.data_transfer =
                        can_download_data(&http_svc).await || wlan_iface.data_transfer;
                    thread::sleep(time::Duration::from_secs(1));
                }
            }

            // after testing, check if we need to disconnect
            if requires_disconnect {
                match wlan_service_util::disconnect_from_network(&wlan_iface.sme_proxy).await {
                    Err(_) => wlan_iface.disconnect_success = false,
                    _ => wlan_iface.disconnect_success = true,
                };
            } else {
                wlan_iface.disconnect_success = true;
            }

            // if any of the checks failed, throw an error to indicate a part of
            // the test failure
            if wlan_iface.connection_success
                && wlan_iface.dhcp_success
                && wlan_iface.data_transfer
                && wlan_iface.disconnect_success
            {
                // note: failures are logged at the point of the failure,
                // simply checking here to return overall test status
                test_pass = true;
            } else {
                test_pass = false;
            }
        }

        // Now test a download over the underlying connection - may be ethernet or an
        // existing wlan connection.
        // TODO(WLAN-1271): The test will currently report failure if there is not an
        // operational base connection available.  This should be switched to a conditional
        // check based on the result of a reachability check after the wlan interfaces
        // are tested.
        test_results.base_data_transfer = can_download_data(&http_svc).await;

        Ok(())
    };
    exec.run_singlethreaded(fut)?;

    if !test_pass {
        bail!("Saw a failure on at least one interface");
    }

    Ok(())
}

// Object to hold overall test status
#[derive(Default, Serialize)]
struct TestResults {
    connect_to_wlan_service: bool,
    connect_to_http_service: bool,
    connect_to_netstack_service: bool,
    query_wlan_service_iface_list: bool,
    wlan_discovered: bool,
    interface_status: bool,
    base_data_transfer: bool,

    #[serde(flatten)]
    iface_objects: HashMap<u16, WlanIface>,

    error_message: String,
}

// Object to hold test specific status
#[derive(Serialize)]
struct WlanIface {
    #[serde(skip_serializing)]
    sme_proxy: fidl_sme::ClientSmeProxy,

    #[serde(skip_serializing)]
    initial_status: fidl_sme::ClientStatusResponse,

    scan_success: bool,

    scan_found_target_ssid: bool,

    connection_success: bool,

    dhcp_success: bool,

    data_transfer: bool,

    disconnect_success: bool,
}

impl WlanIface {
    pub fn new(
        sme_proxy: fidl_sme::ClientSmeProxy,
        status: fidl_sme::ClientStatusResponse,
    ) -> WlanIface {
        WlanIface {
            sme_proxy: sme_proxy,
            initial_status: status,
            scan_success: false,
            scan_found_target_ssid: false,
            connection_success: false,
            disconnect_success: false,
            dhcp_success: false,
            data_transfer: false,
        }
    }
}

fn report_results(test_results: &TestResults) {
    println!("{}", serde_json::to_string_pretty(&test_results).unwrap());
}

fn is_connect_to_target_network_needed<T: AsRef<[u8]>>(
    stay_connected: bool,
    target_ssid: T,
    status: &fidl_sme::ClientStatusResponse,
) -> bool {
    if !stay_connected {
        // doesn't matter if we are connected, we will force a reconnection
        return true;
    }
    // are we already connected?  if so, check the current ssid
    match status.connected_to {
        Some(ref bss) if bss.ssid.as_slice() == target_ssid.as_ref() => false,
        _ => true,
    }
}

fn is_successful_download(result: &Result<http_service_util::IndividualDownload, Error>) -> bool {
    match result {
        Ok(result) if result.bytes > 0 => {
            fx_log_info!(
                "Received {} bytes in {} ns (goodput_mbps = {})",
                result.bytes,
                result.nanos,
                result.goodput_mbps
            );
            true
        }
        Ok(_) => {
            fx_log_warn!("Received 0 bytes for download check");
            false
        }
        Err(e) => {
            fx_log_warn!("Error in download check: {}", e);
            false
        }
    }
}

async fn can_download_data(http_svc: &HttpServiceProxy) -> bool {
    let url_request = http_service_util::create_url_request(URL_STRING);
    let result = http_service_util::fetch_and_discard_url(&http_svc, url_request).await;
    is_successful_download(&result)
}

#[cfg(test)]
mod tests {
    use {super::*, http_service_util::IndividualDownload};

    /// Test to verify a connection will be triggered for an SSID that is not already connected.
    /// This is called with stay connected true and a different target SSID.
    #[test]
    fn test_target_network_needs_connection_targets_different_network() {
        let stay_connected = true;
        let target_ssid = "target_ssid";
        let current_ssid = "current_ssid";
        let connected_to_bss_info = create_bssinfo_using_ssid(Some(current_ssid));

        let current_status = fidl_sme::ClientStatusResponse {
            connected_to: connected_to_bss_info,
            connecting_to_ssid: vec![],
        };

        assert!(is_connect_to_target_network_needed(
            stay_connected,
            target_ssid.as_bytes().to_vec(),
            &current_status
        ));
    }

    /// Test to verify a connection will be triggered for an SSID that is already connected.  This
    /// test is called with the target network already connected and stay_connected false.
    #[test]
    fn test_target_network_needs_connection_stay_connected_false() {
        let stay_connected = false;
        let target_ssid = "target_ssid";
        let connected_to_bss_info = create_bssinfo_using_ssid(Some(target_ssid));

        let current_status = fidl_sme::ClientStatusResponse {
            connected_to: connected_to_bss_info,
            connecting_to_ssid: vec![],
        };

        assert!(is_connect_to_target_network_needed(
            stay_connected,
            target_ssid.as_bytes().to_vec(),
            &current_status
        ));
    }

    /// Test to verify a connection will not be triggered for an SSID that is already connected.
    /// This is called with stay connected true with the target network already connected.
    #[test]
    fn test_target_network_does_not_need_connection() {
        let stay_connected = true;
        let target_ssid = "target_ssid";
        let connected_to_bss_info = create_bssinfo_using_ssid(Some(target_ssid));

        let current_status = fidl_sme::ClientStatusResponse {
            connected_to: connected_to_bss_info,
            connecting_to_ssid: vec![],
        };

        assert_eq!(
            is_connect_to_target_network_needed(
                stay_connected,
                target_ssid.as_bytes().to_vec(),
                &current_status
            ),
            false
        );
    }

    fn create_bssinfo_using_ssid<S: ToString>(ssid: Option<S>) -> Option<Box<fidl_sme::BssInfo>> {
        ssid.map(|s| {
            let bss_info: fidl_sme::BssInfo = fidl_sme::BssInfo {
                bssid: [0, 1, 2, 3, 4, 5],
                ssid: s.to_string().as_bytes().to_vec(),
                rx_dbm: -30,
                channel: 1,
                protected: true,
                compatible: true,
            };
            Box::new(bss_info)
        })
    }

    /// Test verifying that a populated IndividualDownload result correctly returns success for the
    /// download check.
    #[test]
    fn test_successful_download_with_bytes_passes_download_check() {
        let individual_download =
            IndividualDownload { goodput_mbps: 1.11, bytes: 125000, nanos: 900641959 };

        assert!(is_successful_download(&Ok(individual_download)));
    }

    /// Test verifying that a populated but zero byte IndividualDownload result fails the
    /// download check.
    #[test]
    fn test_successful_download_with_zero_bytes_fails_download_check() {
        let individual_download =
            IndividualDownload { goodput_mbps: 1.11, bytes: 0, nanos: 900641959 };

        assert_eq!(is_successful_download(&Ok(individual_download)), false);
    }

    /// Test verifying that an error returned for the download fails the download check.
    #[test]
    fn test_error_download_fails_download_check() {
        let error = Err(failure::format_err!("this is a failure"));
        assert_eq!(is_successful_download(&error), false);
    }

}
