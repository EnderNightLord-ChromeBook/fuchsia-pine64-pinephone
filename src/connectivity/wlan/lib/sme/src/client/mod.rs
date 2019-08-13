// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod bss;
mod event;
pub mod info;
mod inspect;
mod rsn;
mod scan;
mod state;
mod wpa;

#[cfg(test)]
pub mod test_utils;

use failure::{bail, format_err, Fail};
use fidl_fuchsia_wlan_common as fidl_common;
use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, BssDescription, MlmeEvent, ScanRequest};
use fidl_fuchsia_wlan_sme as fidl_sme;
use fuchsia_inspect_contrib::{inspect_insert, inspect_log, log::InspectListClosure};
use futures::channel::{mpsc, oneshot};
use log::error;
use std::sync::Arc;
use wep_deprecated;
use wlan_common::{self, bss::BssDescriptionExt, format::MacFmt, RadioConfig};
use wlan_inspect::wrappers::InspectWlanChan;

use super::{DeviceInfo, InfoStream, MlmeRequest, MlmeStream, Ssid};

use self::event::Event;
use self::info::InfoReporter;
use self::rsn::get_rsna;
use self::scan::{DiscoveryScan, JoinScan, ScanScheduler};
use self::state::{ConnectCommand, Protection, State};
use self::wpa::get_legacy_wpa_association;

use crate::clone_utils::clone_bss_desc;
use crate::responder::Responder;
use crate::sink::{InfoSink, MlmeSink};
use crate::timer::{self, TimedEvent};

pub use self::bss::{BssInfo, ClientConfig, EssInfo};
pub use self::info::{InfoEvent, ScanResult};

// This is necessary to trick the private-in-public checker.
// A private module is not allowed to include private types in its interface,
// even though the module itself is private and will never be exported.
// As a workaround, we add another private module with public types.
mod internal {
    use std::sync::Arc;

    use crate::client::{event::Event, info::InfoReporter, inspect, ConnectionAttemptId};
    use crate::sink::MlmeSink;
    use crate::timer::Timer;
    use crate::DeviceInfo;

    pub struct Context {
        pub device_info: Arc<DeviceInfo>,
        pub mlme_sink: MlmeSink,
        pub(crate) timer: Timer<Event>,
        pub att_id: ConnectionAttemptId,
        pub(crate) inspect: Arc<inspect::SmeTree>,
        pub(crate) info: InfoReporter,
    }
}

use self::internal::*;

pub type TimeStream = timer::TimeStream<Event>;

pub struct ConnectConfig {
    responder: Responder<ConnectResult>,
    credential: fidl_sme::Credential,
    radio_cfg: RadioConfig,
}

// An automatically increasing sequence number that uniquely identifies a logical
// connection attempt. For example, a new connection attempt can be triggered
// by a DisassociateInd message from the MLME.
pub type ConnectionAttemptId = u64;

pub type ScanTxnId = u64;

pub struct ClientSme {
    cfg: ClientConfig,
    state: Option<State>,
    scan_sched: ScanScheduler<Responder<EssDiscoveryResult>, ConnectConfig>,
    context: Context,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ConnectResult {
    Success,
    Canceled,
    Failed(ConnectFailure),
}

impl<T: Into<ConnectFailure>> From<T> for ConnectResult {
    fn from(failure: T) -> Self {
        ConnectResult::Failed(failure.into())
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum ConnectFailure {
    SelectNetwork(SelectNetworkFailure),
    ScanFailure(fidl_mlme::ScanResultCodes),
    JoinFailure(fidl_mlme::JoinResultCodes),
    AuthenticationFailure(fidl_mlme::AuthenticateResultCodes),
    AssociationFailure(fidl_mlme::AssociateResultCodes),
    RsnaTimeout,
    EstablishRsna,
}

impl ConnectFailure {
    pub fn is_timeout(&self) -> bool {
        // Note: we don't return true for JoinFailureTimeout because it's the only join failure
        //       type, so in practice it's returned whether there's a timeout or not.
        //       For association, we don't have a failure type for timeout, so cannot deduce
        //       whether an association failure is due to timeout.
        //
        // TODO(WLAN-1286): Change JOIN_FAILURE_TIMEOUT -> JOIN_FAILURE
        match self {
            ConnectFailure::AuthenticationFailure(failure) => match failure {
                fidl_mlme::AuthenticateResultCodes::AuthFailureTimeout => true,
                _ => false,
            },
            ConnectFailure::RsnaTimeout => true,
            _ => false,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum SelectNetworkFailure {
    NoScanResultWithSsid,
    NoCompatibleNetwork,
    InvalidPasswordArg,
    InternalError,
}

impl From<SelectNetworkFailure> for ConnectFailure {
    fn from(failure: SelectNetworkFailure) -> Self {
        ConnectFailure::SelectNetwork(failure)
    }
}

pub type EssDiscoveryResult = Result<Vec<EssInfo>, fidl_mlme::ScanResultCodes>;

#[derive(Clone, Debug, PartialEq)]
pub struct Status {
    pub connected_to: Option<BssInfo>,
    pub connecting_to: Option<Ssid>,
}

impl ClientSme {
    pub fn new(
        cfg: ClientConfig,
        info: DeviceInfo,
        iface_tree_holder: Arc<wlan_inspect::iface_mgr::IfaceTreeHolder>,
    ) -> (Self, MlmeStream, InfoStream, TimeStream) {
        let device_info = Arc::new(info);
        let (mlme_sink, mlme_stream) = mpsc::unbounded();
        let (info_sink, info_stream) = mpsc::unbounded();
        let (timer, time_stream) = timer::create_timer();
        let inspect = Arc::new(inspect::SmeTree::new(&iface_tree_holder.node));
        iface_tree_holder.place_iface_subtree(inspect.clone());
        (
            ClientSme {
                cfg,
                state: Some(State::Idle { cfg }),
                scan_sched: ScanScheduler::new(Arc::clone(&device_info)),
                context: Context {
                    mlme_sink: MlmeSink::new(mlme_sink),
                    device_info,
                    timer,
                    att_id: 0,
                    inspect,
                    info: InfoReporter::new(InfoSink::new(info_sink)),
                },
            },
            mlme_stream,
            info_stream,
            time_stream,
        )
    }

    pub fn on_connect_command(
        &mut self,
        req: fidl_sme::ConnectRequest,
    ) -> oneshot::Receiver<ConnectResult> {
        let (responder, receiver) = Responder::new();
        // Cancel any ongoing connect attempt
        self.state = self.state.take().map(|state| state.cancel_ongoing_connect(&mut self.context));

        let ssid = req.ssid;
        let (canceled_token, req) = self.scan_sched.enqueue_scan_to_join(JoinScan {
            ssid: ssid.clone(),
            token: ConnectConfig {
                responder,
                credential: req.credential,
                radio_cfg: RadioConfig::from_fidl(req.radio_cfg),
            },
            scan_type: req.scan_type,
        });
        // If the new scan replaced an existing pending JoinScan, notify the existing transaction
        if let Some(token) = canceled_token {
            report_connect_finished(
                Some(token.responder),
                &mut self.context,
                ConnectResult::Canceled,
            );
        }

        self.context.info.report_connect_started(ssid);
        self.send_scan_request(req);
        receiver
    }

    pub fn on_disconnect_command(&mut self) {
        self.state = self.state.take().map(|state| state.disconnect(&mut self.context));
    }

    pub fn on_scan_command(
        &mut self,
        scan_type: fidl_common::ScanType,
    ) -> oneshot::Receiver<EssDiscoveryResult> {
        let (responder, receiver) = Responder::new();
        let scan = DiscoveryScan::new(responder, scan_type);
        let req = self.scan_sched.enqueue_scan_to_discover(scan);
        self.send_scan_request(req);
        receiver
    }

    pub fn status(&self) -> Status {
        let status = self.state.as_ref().expect("expected state to be always present").status();
        if status.connecting_to.is_some() {
            status
        } else {
            // If the association machine is not connecting to a network, but the scanner
            // has a queued 'JoinScan', include the SSID we are trying to connect to
            Status {
                connecting_to: self.scan_sched.get_join_scan().map(|s| s.ssid.clone()),
                ..status
            }
        }
    }

    fn send_scan_request(&mut self, req: Option<ScanRequest>) {
        if let Some(req) = req {
            let is_join_scan = self.scan_sched.is_scanning_to_join();
            let is_connected = self.status().connected_to.is_some();
            self.context.info.report_scan_started(req.clone(), is_join_scan, is_connected);
            self.context.mlme_sink.send(MlmeRequest::Scan(req));
        }
    }
}

impl super::Station for ClientSme {
    type Event = Event;

    fn on_mlme_event(&mut self, event: MlmeEvent) {
        match event {
            MlmeEvent::OnScanResult { result } => {
                self.scan_sched.on_mlme_scan_result(result);
            }
            MlmeEvent::OnScanEnd { end } => {
                let txn_id = end.txn_id;
                let (result, request) = self.scan_sched.on_mlme_scan_end(end);
                // Finalize stats for previous scan first before sending scan request for the next
                // one, which would also start stats collection for new scan scan.
                self.context.info.report_scan_ended(txn_id, &result, &self.cfg);
                self.send_scan_request(request);
                match result {
                    scan::ScanResult::None => (),
                    scan::ScanResult::JoinScanFinished { token, result: Ok(bss_list) } => {
                        let mut inspect_msg: Option<String> = None;
                        let best_bss = self.cfg.get_best_bss(&bss_list);
                        if let Some(ref best_bss) = best_bss {
                            self.context.info.report_candidate_network(clone_bss_desc(best_bss));
                        }
                        match best_bss {
                            // BSS found and compatible.
                            Some(best_bss) if self.cfg.is_bss_compatible(best_bss) => {
                                match get_protection(
                                    &self.context.device_info,
                                    &token.credential,
                                    &best_bss,
                                ) {
                                    Ok(protection) => {
                                        inspect_msg.replace("attempt to connect".to_string());
                                        let cmd = ConnectCommand {
                                            bss: Box::new(clone_bss_desc(&best_bss)),
                                            responder: Some(token.responder),
                                            protection,
                                            radio_cfg: token.radio_cfg,
                                        };
                                        self.state = self
                                            .state
                                            .take()
                                            .map(|state| state.connect(cmd, &mut self.context));
                                    }
                                    Err(err) => {
                                        inspect_msg.replace(format!("cannot join: {}", err));
                                        error!(
                                            "cannot join '{}' ({}): {}",
                                            String::from_utf8_lossy(&best_bss.ssid[..]),
                                            best_bss.bssid.to_mac_str(),
                                            err
                                        );
                                        let f = match err.downcast::<InvalidPasswordArgError>() {
                                            Ok(_) => SelectNetworkFailure::InvalidPasswordArg,
                                            Err(_) => SelectNetworkFailure::InternalError,
                                        };
                                        report_connect_finished(
                                            Some(token.responder),
                                            &mut self.context,
                                            f.into(),
                                        );
                                    }
                                }
                            }
                            // Incompatible network
                            Some(incompatible_bss) => {
                                inspect_msg
                                    .replace(format!("incompatible BSS: {:?}", &incompatible_bss));
                                error!("incompatible BSS: {:?}", &incompatible_bss);
                                report_connect_finished(
                                    Some(token.responder),
                                    &mut self.context,
                                    SelectNetworkFailure::NoCompatibleNetwork.into(),
                                );
                            }
                            // No matching BSS found
                            None => {
                                inspect_msg.replace("no matching BSS".to_string());
                                error!("no matching BSS found");
                                report_connect_finished(
                                    Some(token.responder),
                                    &mut self.context,
                                    SelectNetworkFailure::NoScanResultWithSsid.into(),
                                );
                            }
                        };

                        inspect_log_join_scan(&mut self.context, &bss_list, inspect_msg);
                    }
                    scan::ScanResult::JoinScanFinished { token, result: Err(e) } => {
                        inspect_log!(
                            self.context.inspect.join_scan_events.lock(),
                            scan_failure: format!("{:?}", e),
                        );
                        error!("cannot join network because scan failed: {:?}", e);
                        let result = ConnectResult::Failed(ConnectFailure::ScanFailure(e));
                        report_connect_finished(Some(token.responder), &mut self.context, result);
                    }
                    scan::ScanResult::DiscoveryFinished { tokens, result } => {
                        let result = result.map(|bss_list| self.cfg.group_networks(&bss_list));
                        for responder in tokens {
                            responder.respond(result.clone());
                        }
                    }
                }
            }
            other => {
                self.state =
                    self.state.take().map(|state| state.on_mlme_event(other, &mut self.context));
            }
        };
    }

    fn on_timeout(&mut self, timed_event: TimedEvent<Event>) {
        self.state = self.state.take().map(|state| match timed_event.event {
            event @ Event::EstablishingRsnaTimeout(..)
            | event @ Event::KeyFrameExchangeTimeout(..)
            | event @ Event::ConnectionMilestone(..) => {
                state.handle_timeout(timed_event.id, event, &mut self.context)
            }
        });
    }
}

fn inspect_log_join_scan(
    ctx: &mut Context,
    bss_list: &[fidl_mlme::BssDescription],
    result_msg: Option<String>,
) {
    let inspect_bss = InspectListClosure(&bss_list, |node_writer, key, bss| {
        let ssid = String::from_utf8_lossy(&bss.ssid[..]);
        inspect_insert!(node_writer, var key: {
            bssid: bss.bssid.to_mac_str(),
            ssid: ssid.as_ref(),
            channel: InspectWlanChan(&bss.chan),
            rcpi_dbm: bss.rcpi_dbmh / 2,
            rsni_db: bss.rsni_dbh / 2,
            rssi_dbm: bss.rssi_dbm,
        });
    });
    inspect_log!(ctx.inspect.join_scan_events.lock(), bss_list: inspect_bss, result?: result_msg);
}

fn report_connect_finished(
    responder: Option<Responder<ConnectResult>>,
    context: &mut Context,
    result: ConnectResult,
) {
    if let Some(responder) = responder {
        responder.respond(result.clone());
        context.info.report_connect_finished(result);
    }
}

#[derive(Fail, Debug)]
#[fail(display = "{}", _0)]
pub(crate) struct InvalidPasswordArgError(&'static str);

pub fn get_protection(
    device_info: &DeviceInfo,
    credential: &fidl_sme::Credential,
    bss: &BssDescription,
) -> Result<Protection, failure::Error> {
    match bss.get_protection() {
        wlan_common::bss::Protection::Open => match credential {
            fidl_sme::Credential::None(_) => Ok(Protection::Open),
            _ => Err(InvalidPasswordArgError(
                "password provided for open network, but none expected",
            )
            .into()),
        },
        wlan_common::bss::Protection::Wep => match credential {
            fidl_sme::Credential::Password(pwd) => wep_deprecated::derive_key(&pwd[..])
                .map(Protection::Wep)
                .map_err(|e| format_err!("error deriving WEP key from input: {}", e)),
            _ => Err(InvalidPasswordArgError("unsupported credential type").into()),
        },
        wlan_common::bss::Protection::Wpa1 => {
            get_legacy_wpa_association(device_info, credential, bss)
        }
        wlan_common::bss::Protection::Wpa1Wpa2Personal
        | wlan_common::bss::Protection::Wpa2Personal
        | wlan_common::bss::Protection::Wpa2Wpa3Personal => get_rsna(device_info, credential, bss),
        wlan_common::bss::Protection::Unknown => bail!("unable to deduce protection type of BSS"),
        other => bail!("unsupported BSS protection type {:?}", other),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Config as SmeConfig;
    use fidl_fuchsia_wlan_mlme as fidl_mlme;
    use fuchsia_inspect as finspect;
    use info::ConnectionMilestone;
    use maplit::hashmap;
    use wlan_common::{assert_variant, bss::Standard, RadioConfig};

    use super::info::DiscoveryStats;
    use super::test_utils::{
        create_assoc_conf, create_auth_conf, create_join_conf, expect_info_event,
        expect_stream_empty, fake_bss_with_rates, fake_protected_bss_description,
        fake_unprotected_bss_description, fake_wep_bss_description,
    };

    use crate::test_utils;
    use crate::Station;

    const CLIENT_ADDR: [u8; 6] = [0x7A, 0xE7, 0x76, 0xD9, 0xF2, 0x67];

    fn report_fake_scan_result(sme: &mut ClientSme, bss: fidl_mlme::BssDescription) {
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult { txn_id: 1, bss },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });
    }

    #[test]
    fn test_get_protection() {
        let dev_info = test_utils::fake_device_info(CLIENT_ADDR);

        // Open network without credentials:
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let bss = fake_unprotected_bss_description(b"unprotected".to_vec());
        let protection = get_protection(&dev_info, &credential, &bss);
        assert_variant!(protection, Ok(Protection::Open));

        // Open network with credentials:
        let credential = fidl_sme::Credential::Password(b"somepass".to_vec());
        let bss = fake_unprotected_bss_description(b"unprotected".to_vec());
        get_protection(&dev_info, &credential, &bss)
            .expect_err("unprotected network cannot use password");

        // RSN with user entered password:
        let credential = fidl_sme::Credential::Password(b"somepass".to_vec());
        let bss = fake_protected_bss_description(b"rsn".to_vec());
        let protection = get_protection(&dev_info, &credential, &bss);
        assert_variant!(protection, Ok(Protection::Rsna(_)));

        // RSN with user entered PSK:
        let credential = fidl_sme::Credential::Psk(vec![0xAC; 32]);
        let bss = fake_protected_bss_description(b"rsn".to_vec());
        let protection = get_protection(&dev_info, &credential, &bss);
        assert_variant!(protection, Ok(Protection::Rsna(_)));

        // RSN without credentials:
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let bss = fake_protected_bss_description(b"rsn".to_vec());
        get_protection(&dev_info, &credential, &bss)
            .expect_err("protected network requires password");
    }

    #[test]
    fn test_get_protection_wep() {
        let dev_info = test_utils::fake_device_info(CLIENT_ADDR);

        // WEP-40 with credentials:
        let credential = fidl_sme::Credential::Password(b"wep40".to_vec());
        let bss = fake_wep_bss_description(b"wep40".to_vec());
        let protection = get_protection(&dev_info, &credential, &bss);
        assert_variant!(protection, Ok(Protection::Wep(_)));

        // WEP-104 with credentials:
        let credential = fidl_sme::Credential::Password(b"superinsecure".to_vec());
        let bss = fake_wep_bss_description(b"wep104".to_vec());
        let protection = get_protection(&dev_info, &credential, &bss);
        assert_variant!(protection, Ok(Protection::Wep(_)));

        // WEP without credentials:
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let bss = fake_wep_bss_description(b"wep".to_vec());
        get_protection(&dev_info, &credential, &bss).expect_err("WEP network not supported");

        // WEP with invalid credentials:
        let credential = fidl_sme::Credential::Password(b"wep".to_vec());
        let bss = fake_wep_bss_description(b"wep".to_vec());
        get_protection(&dev_info, &credential, &bss)
            .expect_err("expected error for invalid WEP credentials");
    }

    #[test]
    fn status_connecting_to() {
        let (mut sme, _mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and expect the status to change appropriately.
        // We also check that the association machine state is still disconnected
        // to make sure that the status comes from the scanner.
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should still be connecting to "foo",
        // but the status should now come from the state machine and not from the scanner.
        report_fake_scan_result(&mut sme, fake_unprotected_bss_description(b"foo".to_vec()));
        assert_eq!(Some(b"foo".to_vec()), sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // As soon as connect command is issued for "bar", the status changes immediately
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv2 = sme.on_connect_command(connect_req(b"bar".to_vec(), credential));
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"bar".to_vec()) },
            sme.status()
        );
    }

    #[test]
    fn connecting_to_wep_network_supported() {
        let inspector = finspect::Inspector::new();
        let sme_root_node = inspector.root().create_child("sme");
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = ClientSme::new(
            ClientConfig::from_config(SmeConfig::default().with_wep()),
            test_utils::fake_device_info(CLIENT_ADDR),
            Arc::new(wlan_inspect::iface_mgr::IfaceTreeHolder::new(sme_root_node)),
        );
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and verify that connecting_to status is changed for upper
        // layer (but not underlying state machine) and a scan request is sent to MLME.
        let credential = fidl_sme::Credential::Password(b"wep40".to_vec());
        let req = connect_req(b"foo".to_vec(), credential);
        let _recv = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Scan(..))));

        // Simulate scan end and verify that underlying state machine's status is changed,
        // and a join request is sent to MLME.
        report_fake_scan_result(&mut sme, fake_wep_bss_description(b"foo".to_vec()));
        assert_eq!(Some(b"foo".to_vec()), sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Join(..))));
    }

    #[test]
    fn connecting_to_wep_network_unsupported() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and verify that connecting_to status is changed for upper
        // layer (but not underlying state machine) and a scan request is sent to MLME.
        let credential = fidl_sme::Credential::Password(b"wep40".to_vec());
        let req = connect_req(b"foo".to_vec(), credential);
        let mut connect_fut = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Scan(..))));

        // Simulate scan end and verify that underlying state machine's status is not changed,
        report_fake_scan_result(&mut sme, fake_wep_bss_description(b"foo".to_vec()));
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_connect_result_failed(&mut connect_fut);
    }

    #[test]
    fn connecting_password_supplied_for_protected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and verify that connecting_to status is changed for upper
        // layer (but not underlying state machine) and a scan request is sent to MLME.
        let credential = fidl_sme::Credential::Password(b"somepass".to_vec());
        let req = connect_req(b"foo".to_vec(), credential);
        let _recv = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Scan(..))));

        // Simulate scan end and verify that underlying state machine's status is changed,
        // and a join request is sent to MLME.
        report_fake_scan_result(&mut sme, fake_protected_bss_description(b"foo".to_vec()));
        assert_eq!(Some(b"foo".to_vec()), sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Join(..))));
    }

    #[test]
    fn connecting_psk_supplied_for_protected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // Issue a connect command and verify that connecting_to status is changed for upper
        // layer (but not underlying state machine) and a scan request is sent to MLME.

        // IEEE Std 802.11-2016, J.4.2, Test case 1
        // PSK for SSID "IEEE" and password "password".
        #[rustfmt::skip]
        let psk = vec![
            0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
            0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
            0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
            0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e,
        ];
        let credential = fidl_sme::Credential::Psk(psk);
        let req = connect_req(b"IEEE".to_vec(), credential);
        let _recv = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"IEEE".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Scan(..))));

        // Simulate scan end and verify that underlying state machine's status is changed,
        // and a join request is sent to MLME.
        report_fake_scan_result(&mut sme, fake_protected_bss_description(b"IEEE".to_vec()));
        assert_eq!(Some(b"IEEE".to_vec()), sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"IEEE".to_vec()) },
            sme.status()
        );

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Join(..))));
    }

    #[test]
    fn connecting_password_supplied_for_unprotected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        let credential = fidl_sme::Credential::Password(b"somepass".to_vec());
        let req = connect_req(b"foo".to_vec(), credential);
        let mut connect_fut = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should not attempt to connect
        // because a password was supplied for unprotected network. So both the
        // SME client and underlying state machine should report not connecting
        // anymore.
        report_fake_scan_result(&mut sme, fake_unprotected_bss_description(b"foo".to_vec()));
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // No join request should be sent to MLME
        loop {
            match mlme_stream.try_next() {
                Ok(event) => match event {
                    Some(MlmeRequest::Join(..)) => panic!("unexpected join request to MLME"),
                    None => break,
                    _ => (),
                },
                Err(e) => {
                    assert_eq!(e.to_string(), "receiver channel is empty");
                    break;
                }
            }
        }

        // User should get a message that connection failed
        assert_variant!(connect_fut.try_recv(), Ok(Some(failure)) => {
            assert_eq!(failure, SelectNetworkFailure::InvalidPasswordArg.into());
        });
    }

    #[test]
    fn connecting_psk_supplied_for_unprotected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        let credential = fidl_sme::Credential::Psk(b"somepass".to_vec());
        let req = connect_req(b"foo".to_vec(), credential);
        let mut connect_fut = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should not attempt to connect
        // because a password was supplied for unprotected network. So both the
        // SME client and underlying state machine should report not connecting
        // anymore.
        report_fake_scan_result(&mut sme, fake_unprotected_bss_description(b"foo".to_vec()));
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // No join request should be sent to MLME
        loop {
            match mlme_stream.try_next() {
                Ok(event) => match event {
                    Some(MlmeRequest::Join(..)) => panic!("unexpected join request to MLME"),
                    None => break,
                    _ => (),
                },
                Err(e) => {
                    assert_eq!(e.to_string(), "receiver channel is empty");
                    break;
                }
            }
        }

        // User should get a message that connection failed
        assert_variant!(connect_fut.try_recv(), Ok(Some(failure)) => {
            assert_eq!(failure, SelectNetworkFailure::InvalidPasswordArg.into());
        });
    }

    #[test]
    fn connecting_no_password_supplied_for_protected_network() {
        let (mut sme, mut mlme_stream, _info_stream, _time_stream) = create_sme();
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let req = connect_req(b"foo".to_vec(), credential);
        let mut connect_fut = sme.on_connect_command(req);
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(
            Status { connected_to: None, connecting_to: Some(b"foo".to_vec()) },
            sme.status()
        );

        // Push a fake scan result into SME. We should not attempt to connect
        // because no password was supplied for a protected network.
        report_fake_scan_result(&mut sme, fake_protected_bss_description(b"foo".to_vec()));
        assert_eq!(None, sme.state.as_ref().unwrap().status().connecting_to);
        assert_eq!(Status { connected_to: None, connecting_to: None }, sme.status());

        // No join request should be sent to MLME
        loop {
            match mlme_stream.try_next() {
                Ok(event) => match event {
                    Some(MlmeRequest::Join(..)) => panic!("unexpected join request sent to MLME"),
                    None => break,
                    _ => (),
                },
                Err(e) => {
                    assert_eq!(e.to_string(), "receiver channel is empty");
                    break;
                }
            }
        }

        // User should get a message that connection failed
        assert_variant!(connect_fut.try_recv(), Ok(Some(failure)) => {
            assert_eq!(failure, SelectNetworkFailure::InvalidPasswordArg.into());
        });
    }

    #[test]
    fn connecting_no_scan_result_with_ssid() {
        let (mut sme, _mlme_stream, _info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let mut connect_fut = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        let bss_desc = fake_unprotected_bss_description(b"bar".to_vec());
        report_fake_scan_result(&mut sme, bss_desc);

        assert_variant!(connect_fut.try_recv(), Ok(Some(failure)) => {
            assert_eq!(failure, SelectNetworkFailure::NoScanResultWithSsid.into());
        });
    }

    #[test]
    fn connecting_no_compatible_network_found() {
        let (mut sme, _mlme_stream, _info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::Password(b"password".to_vec());
        let mut connect_fut = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        let mut bss_desc = fake_protected_bss_description(b"foo".to_vec());
        bss_desc.cap.privacy = false; // this makes our check flag this BSS as incompatible
        report_fake_scan_result(&mut sme, bss_desc);

        assert_variant!(connect_fut.try_recv(), Ok(Some(failure)) => {
            assert_eq!(failure, SelectNetworkFailure::NoCompatibleNetwork.into());
        });
    }

    #[test]
    fn new_connect_attempt_cancels_pending_connect() {
        let (mut sme, _mlme_stream, _info_stream, _time_stream) = create_sme();

        let req = connect_req(b"foo".to_vec(), fidl_sme::Credential::None(fidl_sme::Empty));
        let mut connect_fut1 = sme.on_connect_command(req);

        let req2 = connect_req(b"foo".to_vec(), fidl_sme::Credential::None(fidl_sme::Empty));
        let mut connect_fut2 = sme.on_connect_command(req2);

        // User should get a message that first connection attempt is canceled
        assert_connect_result(&mut connect_fut1, ConnectResult::Canceled);
        // Report scan result to transition second connection attempt past scan. This is to verify
        // that connection attempt will be canceled even in the middle of joining the network
        report_fake_scan_result(&mut sme, fake_unprotected_bss_description(b"foo".to_vec()));

        let req3 = connect_req(b"foo".to_vec(), fidl_sme::Credential::None(fidl_sme::Empty));
        let mut _connect_fut3 = sme.on_connect_command(req3);

        // Verify that second connection attempt is canceled as new connect request comes in
        assert_connect_result(&mut connect_fut2, ConnectResult::Canceled);
    }

    #[test]
    fn test_info_event_complete_connect() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        let bss_desc = fake_unprotected_bss_description(b"foo".to_vec());
        let bssid = bss_desc.bssid;
        report_fake_scan_result(&mut sme, bss_desc);

        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        expect_info_event(&mut info_stream, InfoEvent::JoinStarted { att_id: 1 });

        sme.on_mlme_event(create_join_conf(fidl_mlme::JoinResultCodes::Success));
        sme.on_mlme_event(create_auth_conf(bssid, fidl_mlme::AuthenticateResultCodes::Success));
        sme.on_mlme_event(create_assoc_conf(fidl_mlme::AssociateResultCodes::Success));

        expect_info_event(&mut info_stream, InfoEvent::AssociationSuccess { att_id: 1 });
        expect_info_event(
            &mut info_stream,
            InfoEvent::ConnectFinished { result: ConnectResult::Success },
        );
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectStats(stats))) => {
            let scan_stats = stats.join_scan_stats().expect("expect join scan stats");
            assert!(!scan_stats.scan_start_while_connected);
            assert!(scan_stats.scan_time().into_nanos() > 0);
            assert_eq!(scan_stats.result, ScanResult::Success);
            assert_eq!(scan_stats.bss_count, 1);
            assert!(stats.auth_time().is_some());
            assert!(stats.assoc_time().is_some());
            assert!(stats.rsna_time().is_none());
            assert!(stats.connect_time().into_nanos() > 0);
            assert_eq!(stats.result, ConnectResult::Success);
            assert!(stats.candidate_network.is_some());
        });
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectionMilestone(info))) => {
            assert_eq!(info.milestone, ConnectionMilestone::Connected);
        });
    }

    #[test]
    fn test_info_event_failed_connect() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        let bss_desc = fake_unprotected_bss_description(b"foo".to_vec());
        let bssid = bss_desc.bssid;
        report_fake_scan_result(&mut sme, bss_desc);

        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        expect_info_event(&mut info_stream, InfoEvent::JoinStarted { att_id: 1 });

        sme.on_mlme_event(create_join_conf(fidl_mlme::JoinResultCodes::Success));
        let auth_failure = fidl_mlme::AuthenticateResultCodes::Refused;
        sme.on_mlme_event(create_auth_conf(bssid, auth_failure));

        let result = ConnectResult::Failed(ConnectFailure::AuthenticationFailure(auth_failure));
        expect_info_event(&mut info_stream, InfoEvent::ConnectFinished { result: result.clone() });
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectStats(stats))) => {
            assert_eq!(stats.join_scan_stats().expect("no scan stats").result, ScanResult::Success);
            assert!(stats.auth_time().is_some());
            // no association time since requests already fails at auth step
            assert!(stats.assoc_time().is_none());
            assert!(stats.rsna_time().is_none());
            assert!(stats.connect_time().into_nanos() > 0);
            assert_eq!(stats.result, result);
            assert!(stats.candidate_network.is_some());
        });
        expect_stream_empty(&mut info_stream, "unexpected event in info stream");
    }

    #[test]
    fn test_info_event_connect_canceled_during_scan() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        // Send another connect request, which should cancel first one
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(
            &mut info_stream,
            InfoEvent::ConnectFinished { result: ConnectResult::Canceled },
        );
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectStats(stats))) => {
            assert!(stats.scan_start_stats.is_some());
            // Connect attempt cancels before scan is finished. Since we send stats right away,
            // end stats is blank and a complete scan stats cannot be constructed.
            assert!(stats.scan_end_stats.is_none());
            assert!(stats.join_scan_stats().is_none());

            assert!(stats.connect_time().into_nanos() > 0);
            assert_eq!(stats.result, ConnectResult::Canceled);
            assert!(stats.candidate_network.is_none());
        });
        // New connect attempt reports starting right away (though it won't progress until old
        // scan finishes)
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);

        // Old scan finishes. However, no join scan stats is sent
        report_fake_scan_result(&mut sme, fake_unprotected_bss_description(b"foo".to_vec()));

        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 2 });
    }

    #[test]
    fn test_info_event_connect_canceled_post_scan() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        report_fake_scan_result(&mut sme, fake_unprotected_bss_description(b"foo".to_vec()));

        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        expect_info_event(&mut info_stream, InfoEvent::JoinStarted { att_id: 1 });

        // Send another connect request, which should cancel first one
        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(
            &mut info_stream,
            InfoEvent::ConnectFinished { result: ConnectResult::Canceled },
        );
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectStats(stats))) => {
            assert_eq!(stats.join_scan_stats().expect("no scan stats").result, ScanResult::Success);
            assert!(stats.connect_time().into_nanos() > 0);
            assert_eq!(stats.result, ConnectResult::Canceled);
            assert!(stats.candidate_network.is_some());
        });
    }

    #[test]
    fn test_info_event_candidate_network_multiple_bss() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let credential = fidl_sme::Credential::None(fidl_sme::Empty);
        let _recv = sme.on_connect_command(connect_req(b"foo".to_vec(), credential));
        expect_info_event(&mut info_stream, InfoEvent::ConnectStarted);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        let bss = fake_unprotected_bss_description(b"foo".to_vec());
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult { txn_id: 1, bss },
        });
        let bss = fake_unprotected_bss_description(b"foo".to_vec());
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult { txn_id: 1, bss },
        });
        // This scan result should not be counted since it's not the SSID we request
        let bss = fake_unprotected_bss_description(b"bar".to_vec());
        sme.on_mlme_event(MlmeEvent::OnScanResult {
            result: fidl_mlme::ScanResult { txn_id: 1, bss },
        });
        sme.on_mlme_event(MlmeEvent::OnScanEnd {
            end: fidl_mlme::ScanEnd { txn_id: 1, code: fidl_mlme::ScanResultCodes::Success },
        });

        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        expect_info_event(&mut info_stream, InfoEvent::JoinStarted { att_id: 1 });

        // Stop connecting attempt early since we just want to get ConnectStats
        sme.on_mlme_event(create_join_conf(fidl_mlme::JoinResultCodes::JoinFailureTimeout));

        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectFinished { .. })));
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::ConnectStats(stats))) => {
            assert_eq!(stats.join_scan_stats().expect("no scan stats").bss_count, 2);
            assert!(stats.candidate_network.is_some());
        });
    }

    #[test]
    fn test_info_event_discovery_scan() {
        let (mut sme, _mlme_stream, mut info_stream, _time_stream) = create_sme();

        let _recv = sme.on_scan_command(fidl_common::ScanType::Active);
        expect_info_event(&mut info_stream, InfoEvent::MlmeScanStart { txn_id: 1 });

        report_fake_scan_result(&mut sme, fake_bss_with_rates(b"foo".to_vec(), vec![12]));

        expect_info_event(&mut info_stream, InfoEvent::MlmeScanEnd { txn_id: 1 });
        assert_variant!(info_stream.try_next(), Ok(Some(InfoEvent::DiscoveryScanStats(scan_stats, discovery_stats))) => {
            assert!(!scan_stats.scan_start_while_connected);
            assert!(scan_stats.scan_time().into_nanos() > 0);
            assert_eq!(scan_stats.scan_type, fidl_mlme::ScanTypes::Active);
            assert_eq!(scan_stats.result, ScanResult::Success);
            assert_eq!(scan_stats.bss_count, 1);
            assert_eq!(discovery_stats, Some(DiscoveryStats {
                ess_count: 1,
                num_bss_by_channel: hashmap! { 1 => 1 },
                num_bss_by_standard: hashmap! { Standard::Dot11G => 1 },
            }));
        });
    }

    fn assert_connect_result(
        connect_result_receiver: &mut oneshot::Receiver<ConnectResult>,
        expected: ConnectResult,
    ) {
        match connect_result_receiver.try_recv() {
            Ok(Some(actual)) => assert_eq!(expected, actual),
            other => panic!("expect {:?}, got {:?}", expected, other),
        }
    }

    fn assert_connect_result_failed(connect_fut: &mut oneshot::Receiver<ConnectResult>) {
        assert_variant!(connect_fut.try_recv(), Ok(Some(ConnectResult::Failed(..))));
    }

    fn connect_req(ssid: Ssid, credential: fidl_sme::Credential) -> fidl_sme::ConnectRequest {
        fidl_sme::ConnectRequest {
            ssid,
            credential,
            radio_cfg: RadioConfig::default().to_fidl(),
            scan_type: fidl_common::ScanType::Passive,
        }
    }

    fn create_sme() -> (ClientSme, MlmeStream, InfoStream, TimeStream) {
        let inspector = finspect::Inspector::new();
        let sme_root_node = inspector.root().create_child("sme");
        ClientSme::new(
            ClientConfig::default(),
            test_utils::fake_device_info(CLIENT_ADDR),
            Arc::new(wlan_inspect::iface_mgr::IfaceTreeHolder::new(sme_root_node)),
        )
    }
}
