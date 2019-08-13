// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{format_err, Error, ResultExt};
use fidl::endpoints;
use fidl_fuchsia_wlan_common as fidl_common;
use fidl_fuchsia_wlan_device_service::DeviceServiceProxy;
use fidl_fuchsia_wlan_sme as fidl_sme;
use fuchsia_zircon as zx;

type WlanService = DeviceServiceProxy;

pub async fn get_iface_ap_sme_proxy(
    wlan_svc: &WlanService,
    iface_id: u16,
) -> Result<fidl_sme::ApSmeProxy, Error> {
    let (sme_proxy, sme_remote) = endpoints::create_proxy()?;
    let status = wlan_svc.get_ap_sme(iface_id, sme_remote).await
        .context("error sending GetApSme request")?;
    if status == zx::sys::ZX_OK {
        Ok(sme_proxy)
    } else {
        Err(format_err!("Invalid interface id {}", iface_id))
    }
}

pub async fn stop_ap(iface_sme_proxy: &fidl_sme::ApSmeProxy) -> Result<(), Error> {
    let stop_ap_result_code = iface_sme_proxy.stop().await;

    match stop_ap_result_code {
        Ok(result_code) => Ok(result_code),
        _ => Err(format_err!("AP stop failure: {:?}", stop_ap_result_code)),
    }
}

pub async fn start_ap(
    iface_sme_proxy: &fidl_sme::ApSmeProxy,
    target_ssid: Vec<u8>,
    target_pwd: Vec<u8>,
    channel: u8,
) -> Result<fidl_sme::StartApResultCode, Error> {
    // create ConnectRequest holding network info
    let mut config = fidl_sme::ApConfig {
        ssid: target_ssid,
        password: target_pwd,
        radio_cfg: fidl_sme::RadioConfig {
            override_phy: false,
            phy: fidl_common::Phy::Ht,
            override_cbw: false,
            cbw: fidl_common::Cbw::Cbw20,
            override_primary_chan: true,
            primary_chan: channel,
        },
    };
    let start_ap_result_code = iface_sme_proxy.start(&mut config).await;

    match start_ap_result_code {
        Ok(result_code) => Ok(result_code),
        _ => Err(format_err!("AP start failure: {:?}", start_ap_result_code)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_wlan_sme::ApSmeMarker;
    use fidl_fuchsia_wlan_sme::StartApResultCode;
    use fidl_fuchsia_wlan_sme::{ApSmeRequest, ApSmeRequestStream};
    use fuchsia_async as fasync;
    use futures::stream::{StreamExt, StreamFuture};
    use futures::task::Poll;
    use pin_utils::pin_mut;

    #[test]
    fn start_ap_success_returns_true() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::Success);
        assert!(start_ap_result == StartApResultCode::Success);
    }

    #[test]
    fn start_ap_already_started_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::AlreadyStarted);
        assert!(start_ap_result == StartApResultCode::AlreadyStarted);
    }

    #[test]
    fn start_ap_internal_error_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::InternalError);
        assert!(start_ap_result == StartApResultCode::InternalError);
    }

    #[test]
    fn start_ap_canceled_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::Canceled);
        assert!(start_ap_result == StartApResultCode::Canceled);
    }

    #[test]
    fn start_ap_timedout_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::TimedOut);
        assert!(start_ap_result == StartApResultCode::TimedOut);
    }

    #[test]
    fn start_ap_in_progress_returns_false() {
        let start_ap_result =
            test_ap_start("TestAp", "", 6, StartApResultCode::PreviousStartInProgress);
        assert!(start_ap_result == StartApResultCode::PreviousStartInProgress);
    }

    fn test_ap_start(
        ssid: &str,
        password: &str,
        channel: u8,
        result_code: StartApResultCode,
    ) -> StartApResultCode {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let (ap_sme, server) = create_ap_sme_proxy();
        let mut ap_sme_req = server.into_future();
        let target_ssid = ssid.as_bytes().to_vec();
        let target_password = password.as_bytes().to_vec();

        let config = fidl_sme::ApConfig {
            ssid: target_ssid.to_vec(),
            password: target_password.to_vec(),
            radio_cfg: fidl_sme::RadioConfig {
                override_phy: false,
                phy: fidl_common::Phy::Ht,
                override_cbw: false,
                cbw: fidl_common::Cbw::Cbw20,
                override_primary_chan: true,
                primary_chan: channel,
            },
        };

        let fut = start_ap(&ap_sme, target_ssid, target_password, channel);
        pin_mut!(fut);
        assert!(exec.run_until_stalled(&mut fut).is_pending());

        send_start_ap_response(&mut exec, &mut ap_sme_req, config, result_code);

        let complete = exec.run_until_stalled(&mut fut);

        let ap_start_result = match complete {
            Poll::Ready(result) => result,
            _ => panic!("Expected a start response"),
        };

        let returned_start_ap_code = match ap_start_result {
            Ok(response) => response,
            _ => panic!("Expected a valid start result"),
        };

        returned_start_ap_code
    }

    fn create_ap_sme_proxy() -> (fidl_sme::ApSmeProxy, ApSmeRequestStream) {
        let (proxy, server) =
            endpoints::create_proxy::<ApSmeMarker>().expect("failed to create sme ap channel");
        let server = server.into_stream().expect("failed to create ap sme response stream");
        (proxy, server)
    }

    fn send_start_ap_response(
        exec: &mut fasync::Executor,
        server: &mut StreamFuture<ApSmeRequestStream>,
        expected_config: fidl_sme::ApConfig,
        result_code: StartApResultCode,
    ) {
        let rsp = match poll_ap_sme_request(exec, server) {
            Poll::Ready(ApSmeRequest::Start { config, responder }) => {
                assert_eq!(expected_config, config);
                responder
            }
            Poll::Pending => panic!("Expected AP Start Request"),
            _ => panic!("Expected AP Start Request"),
        };

        rsp.send(result_code).expect("Failed to send AP start response.");
    }

    fn poll_ap_sme_request(
        exec: &mut fasync::Executor,
        next_ap_sme_req: &mut StreamFuture<ApSmeRequestStream>,
    ) -> Poll<ApSmeRequest> {
        exec.run_until_stalled(next_ap_sme_req).map(|(req, stream)| {
            *next_ap_sme_req = stream.into_future();
            req.expect("did not expect the ApSmeRequestStream to end")
                .expect("error polling ap sme request stream")
        })
    }
}
