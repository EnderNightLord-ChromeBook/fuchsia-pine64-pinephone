// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod aid;
mod authenticator;
mod event;
mod remote_client;
mod rsn;
#[cfg(test)]
mod test_utils;

use crate::ap::{
    aid::AssociationId,
    authenticator::Authenticator,
    event::{Event, SmeEvent},
    rsn::{create_wpa2_psk_rsne, is_valid_rsne_subset},
};
use crate::phy_selection::derive_phy_cbw_for_ap;
use crate::responder::Responder;
use crate::sink::MlmeSink;
use crate::timer::{self, EventId, TimedEvent, Timer};
use crate::{DeviceInfo, MacAddr, MlmeRequest, Ssid};
use failure::{bail, ensure};
use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, MlmeEvent};
use futures::channel::{mpsc, oneshot};
use log::{debug, error, info, warn};
use std::boxed::Box;
use std::sync::{Arc, Mutex};
use wlan_common::ie::rsn::rsne::{self, Rsne};
use wlan_common::{
    channel::{Channel, Phy},
    RadioConfig,
};
use wlan_rsn::{
    self, gtk::GtkProvider, nonce::NonceReader, psk, NegotiatedProtection, ProtectionInfo,
};

const DEFAULT_BEACON_PERIOD: u16 = 100;
const DEFAULT_DTIM_PERIOD: u8 = 1;

#[derive(Clone, Debug, PartialEq)]
pub struct Config {
    pub ssid: Ssid,
    pub password: Vec<u8>,
    pub radio_cfg: RadioConfig,
}

// OpRadioConfig keeps admitted configuration and operation state
#[derive(Clone, Debug, PartialEq)]
pub struct OpRadioConfig {
    phy: Phy,
    chan: Channel,
}

pub type TimeStream = timer::TimeStream<Event>;

enum State {
    Idle {
        ctx: Context,
    },
    Starting {
        ctx: Context,
        ssid: Ssid,
        rsn_cfg: Option<RsnCfg>,
        responder: Responder<StartResult>,
        start_timeout: EventId,
    },
    Started {
        bss: InfraBss,
    },
}

#[derive(Clone)]
struct RsnCfg {
    psk: psk::Psk,
    rsne: Rsne,
}

struct InfraBss {
    ssid: Ssid,
    rsn_cfg: Option<RsnCfg>,
    client_map: remote_client::Map,
    ctx: Context,
}

pub struct Context {
    device_info: DeviceInfo,
    mlme_sink: MlmeSink,
    timer: Timer<Event>,
}

pub struct ApSme {
    state: Option<State>,
}

#[derive(Debug, PartialEq)]
pub enum StartResult {
    Success,
    AlreadyStarted,
    InternalError,
    Canceled,
    TimedOut,
    PreviousStartInProgress,
    InvalidArguments,
    DfsUnsupported,
}

impl ApSme {
    pub fn new(device_info: DeviceInfo) -> (Self, crate::MlmeStream, TimeStream) {
        let (mlme_sink, mlme_stream) = mpsc::unbounded();
        let (timer, time_stream) = timer::create_timer();
        let sme = ApSme {
            state: Some(State::Idle {
                ctx: Context { device_info, mlme_sink: MlmeSink::new(mlme_sink), timer },
            }),
        };
        (sme, mlme_stream, time_stream)
    }

    pub fn on_start_command(&mut self, config: Config) -> oneshot::Receiver<StartResult> {
        let (responder, receiver) = Responder::new();
        self.state = self.state.take().map(|state| match state {
            State::Idle { mut ctx } => {
                if let Err(result) = validate_config(&config) {
                    responder.respond(result);
                    return State::Idle { ctx };
                }

                let op_result = adapt_operation(&ctx.device_info, &config.radio_cfg);
                let op = match op_result {
                    Err(e) => {
                        error!("error in adapting to device capabilities. operation not accepted: {:?}", e);
                        responder.respond(StartResult::InternalError);
                        return State::Idle { ctx };
                    },
                    Ok(o) => o
                };

                let rsn_cfg_result = create_rsn_cfg(&config.ssid[..], &config.password[..]);
                let rsn_cfg = match rsn_cfg_result {
                    Err(e) => {
                        error!("error configuring RSN: {}", e);
                        responder.respond(StartResult::InternalError);
                        return State::Idle { ctx };
                    },
                    Ok(rsn_cfg) => rsn_cfg
                };

                let req = create_start_request(&op, &config.ssid, rsn_cfg.as_ref());
                ctx.mlme_sink.send(MlmeRequest::Start(req));
                let event = Event::Sme { event: SmeEvent::StartTimeout };
                let start_timeout = ctx.timer.schedule(event);

                State::Starting {
                    ctx,
                    ssid: config.ssid,
                    rsn_cfg,
                    responder,
                    start_timeout,
                }
            },
            s @ State::Starting { .. } => {
                responder.respond(StartResult::PreviousStartInProgress);
                s
            }
            s @ State::Started { .. } => {
                responder.respond(StartResult::AlreadyStarted);
                s
            }
        });
        receiver
    }

    pub fn on_stop_command(&mut self) -> oneshot::Receiver<()> {
        let (stop_responder, receiver) = Responder::new();
        self.state = self.state.take().map(|state| match state {
            s @ State::Idle { .. } => {
                stop_responder.respond(());
                s
            }
            State::Starting { ctx, responder: start_responder, .. } => {
                start_responder.respond(StartResult::Canceled);
                stop_responder.respond(());
                State::Idle { ctx }
            }
            State::Started { bss } => {
                let req = fidl_mlme::StopRequest { ssid: bss.ssid.clone() };
                bss.ctx.mlme_sink.send(MlmeRequest::Stop(req));
                // Currently, MLME doesn't send any response back. We simply assume
                // that the stop request succeeded immediately
                stop_responder.respond(());
                State::Idle { ctx: bss.ctx }
            }
        });
        receiver
    }
}

/// Adapt user-providing operation condition to underlying device capabilities.
fn adapt_operation(
    device_info: &DeviceInfo,
    usr_cfg: &RadioConfig,
) -> Result<OpRadioConfig, failure::Error> {
    // TODO(porce): .expect() may go way, if wlantool absorbs the default value,
    // eg. CBW20 in HT. But doing so would hinder later control from WLANCFG.
    if usr_cfg.phy.is_none() || usr_cfg.cbw.is_none() || usr_cfg.primary_chan.is_none() {
        bail!("Incomplete user config: {:?}", usr_cfg);
    }

    let phy = usr_cfg.phy.unwrap();
    let chan = Channel::new(usr_cfg.primary_chan.unwrap(), usr_cfg.cbw.unwrap());
    if !chan.is_valid() {
        bail!("Invalid channel: {:?}", usr_cfg);
    }

    let (phy_adapted, cbw_adapted) = derive_phy_cbw_for_ap(device_info, &phy, &chan);
    Ok(OpRadioConfig { phy: phy_adapted, chan: Channel::new(chan.primary, cbw_adapted) })
}

impl super::Station for ApSme {
    type Event = Event;

    fn on_mlme_event(&mut self, event: MlmeEvent) {
        debug!("received MLME event: {:?}", event);
        self.state = self.state.take().map(|mut state| match state {
            State::Idle { .. } => {
                warn!("received MlmeEvent while ApSme is idle {:?}", event);
                state
            }
            State::Starting { ctx, ssid, rsn_cfg, responder, start_timeout } => match event {
                MlmeEvent::StartConf { resp } => {
                    handle_start_conf(resp, ctx, ssid, rsn_cfg, responder)
                }
                _ => {
                    warn!("received MlmeEvent while ApSme is starting {:?}", event);
                    State::Starting { ctx, ssid, rsn_cfg, responder, start_timeout }
                }
            },
            State::Started { ref mut bss } => {
                match event {
                    MlmeEvent::AuthenticateInd { ind } => bss.handle_auth_ind(ind),
                    MlmeEvent::DeauthenticateInd { ind } => {
                        bss.handle_deauth(&ind.peer_sta_address)
                    }
                    MlmeEvent::DeauthenticateConf { resp } => {
                        bss.handle_deauth(&resp.peer_sta_address)
                    }
                    MlmeEvent::AssociateInd { ind } => bss.handle_assoc_ind(ind),
                    MlmeEvent::DisassociateInd { ind } => bss.handle_disassoc_ind(ind),
                    MlmeEvent::EapolInd { ind } => {
                        let _ = bss.handle_eapol_ind(ind).map_err(|e| warn!("{}", e));
                    }
                    MlmeEvent::EapolConf { resp } => {
                        if resp.result_code != fidl_mlme::EapolResultCodes::Success {
                            // TODO(NET-1634) - Handle unsuccessful EAPoL confirmation. It doesn't
                            //                  include client address, though. Maybe we can just
                            //                  ignore these messages and just set a handshake
                            //                  timeout instead
                            info!("Received unsuccessful EapolConf");
                        }
                    }
                    _ => warn!("unsupported MlmeEvent type {:?}; ignoring", event),
                }
                state
            }
        });
    }

    fn on_timeout(&mut self, timed_event: TimedEvent<Event>) {
        self.state = self.state.take().map(|mut state| match state {
            State::Idle { .. } => state,
            State::Starting { start_timeout, ctx, responder, ssid, rsn_cfg } => {
                match timed_event.event {
                    Event::Sme { event } => match event {
                        SmeEvent::StartTimeout if start_timeout == timed_event.id => {
                            warn!("Timed out waiting for MLME to start");
                            responder.respond(StartResult::TimedOut);
                            State::Idle { ctx }
                        }
                        _ => State::Starting { start_timeout, ctx, responder, ssid, rsn_cfg },
                    },
                    _ => State::Starting { start_timeout, ctx, responder, ssid, rsn_cfg },
                }
            }
            State::Started { ref mut bss } => {
                bss.handle_timeout(timed_event);
                state
            }
        });
    }
}

fn validate_config(config: &Config) -> Result<(), StartResult> {
    let rc = &config.radio_cfg;
    if rc.phy.is_none() || rc.cbw.is_none() || rc.primary_chan.is_none() {
        return Err(StartResult::InvalidArguments);
    }
    let c = Channel::new(rc.primary_chan.unwrap(), config.radio_cfg.cbw.unwrap());
    if !c.is_valid() {
        Err(StartResult::InvalidArguments)
    } else if c.is_dfs() {
        Err(StartResult::DfsUnsupported)
    } else {
        Ok(())
    }
}

fn handle_start_conf(
    conf: fidl_mlme::StartConfirm,
    ctx: Context,
    ssid: Ssid,
    rsn_cfg: Option<RsnCfg>,
    responder: Responder<StartResult>,
) -> State {
    match conf.result_code {
        fidl_mlme::StartResultCodes::Success => {
            responder.respond(StartResult::Success);
            State::Started { bss: InfraBss { ssid, rsn_cfg, client_map: Default::default(), ctx } }
        }
        result_code => {
            error!("failed to start BSS: {:?}", result_code);
            responder.respond(StartResult::InternalError);
            State::Idle { ctx }
        }
    }
}

impl InfraBss {
    fn handle_auth_ind(&mut self, ind: fidl_mlme::AuthenticateIndication) {
        if self.client_map.remove_client(&ind.peer_sta_address).is_some() {
            warn!(
                "client {:?} authenticates while still associated; removed client from map",
                ind.peer_sta_address
            );
        }

        let result_code = if ind.auth_type == fidl_mlme::AuthenticationTypes::OpenSystem {
            fidl_mlme::AuthenticateResultCodes::Success
        } else {
            warn!("unsupported authentication type {:?}", ind.auth_type);
            fidl_mlme::AuthenticateResultCodes::Refused
        };
        let resp =
            fidl_mlme::AuthenticateResponse { peer_sta_address: ind.peer_sta_address, result_code };
        self.ctx.mlme_sink.send(MlmeRequest::AuthResponse(resp));
    }

    fn handle_deauth(&mut self, client_addr: &MacAddr) {
        let _ = self.client_map.remove_client(client_addr);
    }

    fn handle_assoc_ind(&mut self, ind: fidl_mlme::AssociateIndication) {
        if self.client_map.remove_client(&ind.peer_sta_address).is_some() {
            warn!(
                "client {:?} associates while still associated; removed client from map",
                ind.peer_sta_address
            );
        }

        let result = match (ind.rsn.as_ref(), self.rsn_cfg.clone()) {
            (Some(s_rsne_bytes), Some(a_rsn)) => {
                self.handle_rsn_assoc_ind(s_rsne_bytes, a_rsn, &ind.peer_sta_address)
            }
            (None, None) => self.add_client(ind.peer_sta_address.clone(), None),
            _ => {
                warn!("unexpected RSN element from client: {:?}", ind.rsn);
                Err(fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch)
            }
        };
        let (aid, result_code) = match result {
            Ok(aid) => (aid, fidl_mlme::AssociateResultCodes::Success),
            Err(result_code) => (0, result_code),
        };
        let resp = fidl_mlme::AssociateResponse {
            peer_sta_address: ind.peer_sta_address.clone(),
            result_code,
            association_id: aid,
        };

        self.ctx.mlme_sink.send(MlmeRequest::AssocResponse(resp));
        if result_code == fidl_mlme::AssociateResultCodes::Success && self.rsn_cfg.is_some() {
            match self.client_map.get_mut_client(&ind.peer_sta_address) {
                Some(client) => client.initiate_key_exchange(&mut self.ctx, 1),
                None => error!(
                    "cannot initiate key exchange for unknown client: {:02X?}",
                    ind.peer_sta_address
                ),
            }
        }
    }

    fn handle_disassoc_ind(&mut self, ind: fidl_mlme::DisassociateIndication) {
        let _ = self.client_map.remove_client(&ind.peer_sta_address);
    }

    fn handle_rsn_assoc_ind(
        &mut self,
        s_rsne_bytes: &Vec<u8>,
        a_rsn: RsnCfg,
        client_addr: &MacAddr,
    ) -> Result<AssociationId, fidl_mlme::AssociateResultCodes> {
        let (_, s_rsne) = rsne::from_bytes(s_rsne_bytes).map_err(|e| {
            warn!("failed to deserialize RSNE: {:?}", e);
            fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch
        })?;
        validate_s_rsne(&s_rsne, &a_rsn.rsne).map_err(|_| {
            warn!("incompatible client RSNE");
            fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch
        })?;

        // Note: There should be one Reader per device, not per SME.
        // Follow-up with improving on this.
        let nonce_rdr = NonceReader::new(&self.ctx.device_info.addr[..]).map_err(|e| {
            warn!("failed to create NonceReader: {}", e);
            fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch
        })?;
        let gtk_provider = get_gtk_provider(&s_rsne).map_err(|e| {
            warn!("failed to create GtkProvider: {}", e);
            fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch
        })?;
        let authenticator = wlan_rsn::Authenticator::new_wpa2psk_ccmp128(
            nonce_rdr,
            gtk_provider,
            a_rsn.psk.clone(),
            client_addr.clone(),
            ProtectionInfo::Rsne(s_rsne),
            self.ctx.device_info.addr,
            ProtectionInfo::Rsne(a_rsn.rsne),
        )
        .map_err(|e| {
            warn!("failed to create authenticator: {}", e);
            fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch
        })?;
        self.add_client(client_addr.clone(), Some(Box::new(authenticator)))
    }

    fn add_client(
        &mut self,
        addr: MacAddr,
        auth: Option<Box<dyn Authenticator>>,
    ) -> Result<AssociationId, fidl_mlme::AssociateResultCodes> {
        self.client_map.add_client(addr, auth).map_err(|e| {
            warn!("unable to add user to client map: {}", e);
            fidl_mlme::AssociateResultCodes::RefusedReasonUnspecified
        })
    }

    fn handle_eapol_ind(&mut self, ind: fidl_mlme::EapolIndication) -> Result<(), failure::Error> {
        let client_addr = &ind.src_addr;
        match self.client_map.get_mut_client(client_addr) {
            Some(client) => client.handle_eapol_ind(ind, &mut self.ctx),
            None => bail!("client {:02X?} not found; ignoring EapolInd msg", client_addr),
        }
    }

    fn handle_timeout(&mut self, timed_event: TimedEvent<Event>) {
        match timed_event.event {
            Event::Sme { .. } => (),
            Event::Client { addr, event } => {
                if let Some(client) = self.client_map.get_mut_client(&addr) {
                    client.handle_timeout(timed_event.id, event, &mut self.ctx);
                }
            }
        }
    }
}

fn validate_s_rsne(s_rsne: &Rsne, a_rsne: &Rsne) -> Result<(), failure::Error> {
    ensure!(is_valid_rsne_subset(s_rsne, a_rsne)?, "incompatible client RSNE");
    Ok(())
}

fn get_gtk_provider(s_rsne: &Rsne) -> Result<Arc<Mutex<GtkProvider>>, failure::Error> {
    let negotiated_protection = NegotiatedProtection::from_rsne(&s_rsne)?;
    let gtk_provider = GtkProvider::new(negotiated_protection.group_data)?;
    Ok(Arc::new(Mutex::new(gtk_provider)))
}

fn create_rsn_cfg(ssid: &[u8], password: &[u8]) -> Result<Option<RsnCfg>, failure::Error> {
    if password.is_empty() {
        Ok(None)
    } else {
        let psk = psk::compute(password, ssid)?;
        Ok(Some(RsnCfg { psk, rsne: create_wpa2_psk_rsne() }))
    }
}

fn create_start_request(
    op: &OpRadioConfig,
    ssid: &Ssid,
    ap_rsn: Option<&RsnCfg>,
) -> fidl_mlme::StartRequest {
    let rsne_bytes = ap_rsn.as_ref().map(|RsnCfg { rsne, .. }| {
        let mut buf = Vec::with_capacity(rsne.len());
        if let Err(e) = rsne.write_into(&mut buf) {
            error!("error writing RSNE into MLME-START.request: {}", e);
        }
        buf
    });

    let (cbw, _secondary80) = op.chan.cbw.to_fidl();

    fidl_mlme::StartRequest {
        ssid: ssid.clone(),
        bss_type: fidl_mlme::BssTypes::Infrastructure,
        beacon_period: DEFAULT_BEACON_PERIOD,
        dtim_period: DEFAULT_DTIM_PERIOD,
        channel: op.chan.primary,
        country: fidl_mlme::Country {
            // TODO(WLAN-870): Get config from wlancfg
            alpha2: ['U' as u8, 'S' as u8],
            suffix: fidl_mlme::COUNTRY_ENVIRON_ALL,
        },
        rsne: rsne_bytes,
        mesh_id: vec![],
        phy: op.phy.to_fidl(),
        cbw: cbw,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_wlan_mlme as fidl_mlme;
    use wlan_common::{
        assert_variant,
        channel::{Cbw, Phy},
        RadioConfig,
    };

    use crate::{test_utils::*, MlmeStream, Station};

    const AP_ADDR: [u8; 6] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66];
    const CLIENT_ADDR: [u8; 6] = [0x7A, 0xE7, 0x76, 0xD9, 0xF2, 0x67];
    const CLIENT_ADDR2: [u8; 6] = [0x22, 0x22, 0x22, 0x22, 0x22, 0x22];
    const SSID: &'static [u8] = &[0x46, 0x55, 0x43, 0x48, 0x53, 0x49, 0x41];
    const RSNE: &'static [u8] = &[
        0x30, // element id
        0x2A, // length
        0x01, 0x00, // version
        0x00, 0x0f, 0xac, 0x04, // group data cipher suite -- CCMP-128
        0x01, 0x00, // pairwise cipher suite count
        0x00, 0x0f, 0xac, 0x04, // pairwise cipher suite list -- CCMP-128
        0x01, 0x00, // akm suite count
        0x00, 0x0f, 0xac, 0x02, // akm suite list -- PSK
        0xa8, 0x04, // rsn capabilities
        0x01, 0x00, // pmk id count
        // pmk id list
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
        0x11, 0x00, 0x0f, 0xac, 0x04, // group management cipher suite -- CCMP-128
    ];

    fn radio_cfg(pri_chan: u8) -> RadioConfig {
        RadioConfig::new(Phy::Ht, Cbw::Cbw20, pri_chan)
    }

    fn unprotected_config() -> Config {
        Config { ssid: SSID.to_vec(), password: vec![], radio_cfg: radio_cfg(11) }
    }

    fn protected_config() -> Config {
        Config {
            ssid: SSID.to_vec(),
            password: vec![0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68],
            radio_cfg: radio_cfg(11),
        }
    }

    #[test]
    fn test_validate_config() {
        assert_eq!(
            Err(StartResult::InvalidArguments),
            validate_config(&Config { ssid: vec![], password: vec![], radio_cfg: radio_cfg(15) })
        );
        assert_eq!(
            Err(StartResult::DfsUnsupported),
            validate_config(&Config { ssid: vec![], password: vec![], radio_cfg: radio_cfg(52) })
        );
        assert_eq!(
            Ok(()),
            validate_config(&Config { ssid: vec![], password: vec![], radio_cfg: radio_cfg(40) })
        );
    }

    #[test]
    fn authenticate_while_sme_is_idle() {
        let (mut sme, mut mlme_stream, _) = create_sme();
        let client = Client::default();
        sme.on_mlme_event(client.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));

        assert_variant!(mlme_stream.try_next(), Err(e) => {
            assert_eq!(e.to_string(), "receiver channel is empty");
        });
    }

    #[test]
    fn ap_starts_success() {
        let (mut sme, mut mlme_stream, _) = create_sme();
        let mut receiver = sme.on_start_command(unprotected_config());

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Start(start_req))) => {
            assert_eq!(start_req.ssid, SSID.to_vec());
            assert_eq!(start_req.bss_type, fidl_mlme::BssTypes::Infrastructure);
            assert_ne!(start_req.beacon_period, 0);
            assert_ne!(start_req.dtim_period, 0);
            assert_eq!(
                start_req.channel,
                unprotected_config().radio_cfg.primary_chan.expect("invalid config")
            );
            assert!(start_req.rsne.is_none());
        });

        assert_eq!(Ok(None), receiver.try_recv());
        sme.on_mlme_event(create_start_conf(fidl_mlme::StartResultCodes::Success));
        assert_eq!(Ok(Some(StartResult::Success)), receiver.try_recv());
    }

    #[test]
    fn ap_starts_timeout() {
        let (mut sme, _, mut time_stream) = create_sme();
        let mut receiver = sme.on_start_command(unprotected_config());

        let (_, event) = time_stream.try_next().unwrap().expect("expect timer message");
        sme.on_timeout(event);

        assert_eq!(Ok(Some(StartResult::TimedOut)), receiver.try_recv());
    }

    #[test]
    fn ap_starts_fails() {
        let (mut sme, _, _) = create_sme();
        let mut receiver = sme.on_start_command(unprotected_config());

        sme.on_mlme_event(create_start_conf(fidl_mlme::StartResultCodes::NotSupported));
        assert_eq!(Ok(Some(StartResult::InternalError)), receiver.try_recv());
    }

    #[test]
    fn start_req_while_ap_is_starting() {
        let (mut sme, _, _) = create_sme();
        let mut receiver_one = sme.on_start_command(unprotected_config());

        // While SME is starting, any start request receives an error immediately
        let mut receiver_two = sme.on_start_command(unprotected_config());
        assert_eq!(Ok(Some(StartResult::PreviousStartInProgress)), receiver_two.try_recv());

        // Start confirmation for first request should still have an affect
        sme.on_mlme_event(create_start_conf(fidl_mlme::StartResultCodes::Success));
        assert_eq!(Ok(Some(StartResult::Success)), receiver_one.try_recv());
    }

    #[test]
    fn ap_stops_while_idle() {
        let (mut sme, _, _) = create_sme();
        let mut receiver = sme.on_stop_command();
        assert_eq!(Ok(Some(())), receiver.try_recv());
    }

    #[test]
    fn stop_req_while_ap_is_starting() {
        let (mut sme, _, _) = create_sme();
        let mut start_receiver = sme.on_start_command(unprotected_config());
        let mut stop_receiver = sme.on_stop_command();
        assert_eq!(Ok(Some(StartResult::Canceled)), start_receiver.try_recv());
        assert_eq!(Ok(Some(())), stop_receiver.try_recv());
    }

    #[test]
    fn ap_stops_after_started() {
        let (mut sme, mut mlme_stream, _) = start_unprotected_ap();
        let mut receiver = sme.on_stop_command();

        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Stop(stop_req))) => {
            assert_eq!(stop_req.ssid, SSID.to_vec());
        });
        assert_eq!(Ok(Some(())), receiver.try_recv());
    }

    #[test]
    fn client_authenticates_supported_authentication_type() {
        let (mut sme, mut mlme_stream, _) = start_unprotected_ap();
        let client = Client::default();
        sme.on_mlme_event(client.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));
        client.verify_auth_resp(&mut mlme_stream, fidl_mlme::AuthenticateResultCodes::Success);
    }

    #[test]
    fn client_authenticates_unsupported_authentication_type() {
        let (mut sme, mut mlme_stream, _) = start_unprotected_ap();
        let client = Client::default();
        let auth_ind = client.create_auth_ind(fidl_mlme::AuthenticationTypes::FastBssTransition);
        sme.on_mlme_event(auth_ind);
        client.verify_auth_resp(&mut mlme_stream, fidl_mlme::AuthenticateResultCodes::Refused);
    }

    #[test]
    fn client_associates_unprotected_network() {
        let (mut sme, mut mlme_stream, _) = start_unprotected_ap();
        let client = Client::default();
        sme.on_mlme_event(client.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));
        client.verify_auth_resp(&mut mlme_stream, fidl_mlme::AuthenticateResultCodes::Success);

        sme.on_mlme_event(client.create_assoc_ind(None));
        client.verify_assoc_resp(&mut mlme_stream, 1, fidl_mlme::AssociateResultCodes::Success);
    }

    #[test]
    fn client_associates_valid_rsne() {
        let (mut sme, mut mlme_stream, _) = start_protected_ap();
        let client = Client::default();
        client.authenticate_and_drain_mlme(&mut sme, &mut mlme_stream);

        sme.on_mlme_event(client.create_assoc_ind(Some(RSNE.to_vec())));
        client.verify_assoc_resp(&mut mlme_stream, 1, fidl_mlme::AssociateResultCodes::Success);
        client.verify_eapol_req(&mut mlme_stream);
    }

    #[test]
    fn client_associates_invalid_rsne() {
        let (mut sme, mut mlme_stream, _) = start_protected_ap();
        let client = Client::default();
        client.authenticate_and_drain_mlme(&mut sme, &mut mlme_stream);

        sme.on_mlme_event(client.create_assoc_ind(None));
        client.verify_assoc_resp(
            &mut mlme_stream,
            0,
            fidl_mlme::AssociateResultCodes::RefusedCapabilitiesMismatch,
        );
    }

    #[test]
    fn rsn_handshake_timeout() {
        let (mut sme, mut mlme_stream, mut time_stream) = start_protected_ap();
        let client = Client::default();
        client.authenticate_and_drain_mlme(&mut sme, &mut mlme_stream);

        sme.on_mlme_event(client.create_assoc_ind(Some(RSNE.to_vec())));
        client.verify_assoc_resp(&mut mlme_stream, 1, fidl_mlme::AssociateResultCodes::Success);

        for _i in 0..4 {
            client.verify_eapol_req(&mut mlme_stream);

            let (_, event) = time_stream.try_next().unwrap().expect("expect timer message");
            // Calling `on_timeout` with a different event ID is a no-op
            let mut fake_event = event.clone();
            fake_event.id += 1;
            sme.on_timeout(fake_event);
            assert_variant!(mlme_stream.try_next(), Err(e) => {
                assert_eq!(e.to_string(), "receiver channel is empty")
            });
            sme.on_timeout(event);
        }

        client.verify_deauth_req(&mut mlme_stream, fidl_mlme::ReasonCode::FourwayHandshakeTimeout);
    }

    #[test]
    fn client_restarts_authentication_flow() {
        let (mut sme, mut mlme_stream, _) = start_unprotected_ap();
        let client = Client::default();
        client.authenticate_and_drain_mlme(&mut sme, &mut mlme_stream);
        client.associate_and_drain_mlme(&mut sme, &mut mlme_stream, None);

        sme.on_mlme_event(client.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));
        client.verify_auth_resp(&mut mlme_stream, fidl_mlme::AuthenticateResultCodes::Success);

        sme.on_mlme_event(client.create_assoc_ind(None));
        client.verify_assoc_resp(&mut mlme_stream, 1, fidl_mlme::AssociateResultCodes::Success);
    }

    #[test]
    fn multiple_clients_associate() {
        let (mut sme, mut mlme_stream, _) = start_protected_ap();
        let client1 = Client::default();
        let client2 = Client { addr: CLIENT_ADDR2 };

        sme.on_mlme_event(client1.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));
        client1.verify_auth_resp(&mut mlme_stream, fidl_mlme::AuthenticateResultCodes::Success);

        sme.on_mlme_event(client2.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));
        client2.verify_auth_resp(&mut mlme_stream, fidl_mlme::AuthenticateResultCodes::Success);

        sme.on_mlme_event(client1.create_assoc_ind(Some(RSNE.to_vec())));
        client1.verify_assoc_resp(&mut mlme_stream, 1, fidl_mlme::AssociateResultCodes::Success);
        client1.verify_eapol_req(&mut mlme_stream);

        sme.on_mlme_event(client2.create_assoc_ind(Some(RSNE.to_vec())));
        client2.verify_assoc_resp(&mut mlme_stream, 2, fidl_mlme::AssociateResultCodes::Success);
        client2.verify_eapol_req(&mut mlme_stream);
    }

    #[test]
    fn test_adapt_operation() {
        // Invalid input
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig { phy: None, cbw: Some(Cbw::Cbw20), primary_chan: Some(1) };
            let got = adapt_operation(&dinf, &ucfg);
            assert!(got.is_err());
        }
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw40, 48);
            let got = adapt_operation(&dinf, &ucfg);
            assert!(got.is_err());
        }

        // VHT device, VHT config
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw80, 48);
            let want = OpRadioConfig { phy: Phy::Vht, chan: Channel::new(48, Cbw::Cbw80) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw40Below, 48);
            let want = OpRadioConfig { phy: Phy::Vht, chan: Channel::new(48, Cbw::Cbw40Below) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw20, 48);
            let want = OpRadioConfig { phy: Phy::Vht, chan: Channel::new(48, Cbw::Cbw20) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }

        // VHT device, HT config
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Ht, Cbw::Cbw40Below, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw40Below) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Ht, Cbw::Cbw20, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw20) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_vht(fidl_mlme::ChanWidthSet::TwentyOnly);
            let ucfg = RadioConfig::new(Phy::Ht, Cbw::Cbw40Below, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw20) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }

        // HT device, VHT config
        {
            let dinf = fake_device_info_ht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw80, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw40Below) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_ht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw40Below, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw40Below) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_ht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Vht, Cbw::Cbw20, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw20) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }

        // HT device, HT config
        {
            let dinf = fake_device_info_ht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Ht, Cbw::Cbw40Below, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw40Below) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_ht(fidl_mlme::ChanWidthSet::TwentyForty);
            let ucfg = RadioConfig::new(Phy::Ht, Cbw::Cbw20, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw20) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
        {
            let dinf = fake_device_info_ht(fidl_mlme::ChanWidthSet::TwentyOnly);
            let ucfg = RadioConfig::new(Phy::Ht, Cbw::Cbw40Below, 48);
            let want = OpRadioConfig { phy: Phy::Ht, chan: Channel::new(48, Cbw::Cbw20) };
            let got = adapt_operation(&dinf, &ucfg).unwrap();
            assert_eq!(want, got);
        }
    }

    fn create_start_conf(result_code: fidl_mlme::StartResultCodes) -> MlmeEvent {
        MlmeEvent::StartConf { resp: fidl_mlme::StartConfirm { result_code } }
    }

    struct Client {
        addr: MacAddr,
    }

    impl Client {
        fn default() -> Self {
            Client { addr: CLIENT_ADDR }
        }

        fn authenticate_and_drain_mlme(
            &self,
            sme: &mut ApSme,
            mlme_stream: &mut crate::MlmeStream,
        ) {
            sme.on_mlme_event(self.create_auth_ind(fidl_mlme::AuthenticationTypes::OpenSystem));
            assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::AuthResponse(..))));
        }

        fn associate_and_drain_mlme(
            &self,
            sme: &mut ApSme,
            mlme_stream: &mut crate::MlmeStream,
            rsne: Option<Vec<u8>>,
        ) {
            sme.on_mlme_event(self.create_assoc_ind(rsne));
            assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::AssocResponse(..))));
        }

        fn create_auth_ind(&self, auth_type: fidl_mlme::AuthenticationTypes) -> MlmeEvent {
            MlmeEvent::AuthenticateInd {
                ind: fidl_mlme::AuthenticateIndication { peer_sta_address: self.addr, auth_type },
            }
        }

        fn create_assoc_ind(&self, rsne: Option<Vec<u8>>) -> MlmeEvent {
            MlmeEvent::AssociateInd {
                ind: fidl_mlme::AssociateIndication {
                    peer_sta_address: self.addr,
                    listen_interval: 100,
                    ssid: Some(SSID.to_vec()),
                    rsn: rsne,
                },
            }
        }

        fn verify_auth_resp(
            &self,
            mlme_stream: &mut MlmeStream,
            result_code: fidl_mlme::AuthenticateResultCodes,
        ) {
            let msg = mlme_stream.try_next();
            assert_variant!(msg, Ok(Some(MlmeRequest::AuthResponse(auth_resp))) => {
                assert_eq!(auth_resp.peer_sta_address, self.addr);
                assert_eq!(auth_resp.result_code, result_code);
            });
        }

        fn verify_assoc_resp(
            &self,
            mlme_stream: &mut MlmeStream,
            aid: AssociationId,
            result_code: fidl_mlme::AssociateResultCodes,
        ) {
            let msg = mlme_stream.try_next();
            assert_variant!(msg, Ok(Some(MlmeRequest::AssocResponse(assoc_resp))) => {
                assert_eq!(assoc_resp.peer_sta_address, self.addr);
                assert_eq!(assoc_resp.association_id, aid);
                assert_eq!(assoc_resp.result_code, result_code);
            });
        }

        fn verify_eapol_req(&self, mlme_stream: &mut MlmeStream) {
            assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Eapol(eapol_req))) => {
                assert_eq!(eapol_req.src_addr, AP_ADDR);
                assert_eq!(eapol_req.dst_addr, self.addr);
                assert!(eapol_req.data.len() > 0);
            });
        }

        fn verify_deauth_req(
            &self,
            mlme_stream: &mut MlmeStream,
            reason_code: fidl_mlme::ReasonCode,
        ) {
            let msg = mlme_stream.try_next();
            assert_variant!(msg, Ok(Some(MlmeRequest::Deauthenticate(deauth_req))) => {
                assert_eq!(deauth_req.peer_sta_address, self.addr);
                assert_eq!(deauth_req.reason_code, reason_code);
            });
        }
    }

    fn start_protected_ap() -> (ApSme, crate::MlmeStream, TimeStream) {
        start_ap(true)
    }

    fn start_unprotected_ap() -> (ApSme, crate::MlmeStream, TimeStream) {
        start_ap(false)
    }

    fn start_ap(protected: bool) -> (ApSme, crate::MlmeStream, TimeStream) {
        let (mut sme, mut mlme_stream, mut time_stream) = create_sme();
        let config = if protected { protected_config() } else { unprotected_config() };
        let mut receiver = sme.on_start_command(config);
        assert_variant!(mlme_stream.try_next(), Ok(Some(MlmeRequest::Start(..))));
        // drain time stream
        while let Ok(..) = time_stream.try_next() {}
        sme.on_mlme_event(create_start_conf(fidl_mlme::StartResultCodes::Success));

        assert_eq!(Ok(Some(StartResult::Success)), receiver.try_recv());
        (sme, mlme_stream, time_stream)
    }

    fn create_sme() -> (ApSme, MlmeStream, TimeStream) {
        ApSme::new(fake_device_info(AP_ADDR))
    }
}
