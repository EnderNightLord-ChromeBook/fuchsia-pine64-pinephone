// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon::{self as zx, prelude::DurationNum};

use crate::client::info::ConnectionPingInfo;
use crate::timer::TimeoutDuration;
use crate::MacAddr;

pub const ESTABLISHING_RSNA_TIMEOUT_SECONDS: i64 = 3;
pub const KEY_FRAME_EXCHANGE_TIMEOUT_MILLIS: i64 = 200;
pub const KEY_FRAME_EXCHANGE_MAX_ATTEMPTS: u32 = 3;
pub const CONNECTION_PING_TIMEOUT_MINUTES: i64 = 1;
pub const INSPECT_PULSE_CHECK_MINUTES: i64 = 1;
pub const SAE_RETRANSMISSION_TIMEOUT_MILLIS: i64 = 200;
pub const SAE_PMKSA_EXPIRATION_TIMEOUT_HOURS: i64 = 24 * 7;

#[derive(Debug, Clone)]
pub enum Event {
    EstablishingRsnaTimeout(EstablishingRsnaTimeout),
    KeyFrameExchangeTimeout(KeyFrameExchangeTimeout),
    ConnectionPing(ConnectionPingInfo),
    InspectPulseCheck(InspectPulseCheck),
    SaeTimeout(SaeTimeout),
}
impl From<EstablishingRsnaTimeout> for Event {
    fn from(timeout: EstablishingRsnaTimeout) -> Self {
        Event::EstablishingRsnaTimeout(timeout)
    }
}
impl From<KeyFrameExchangeTimeout> for Event {
    fn from(timeout: KeyFrameExchangeTimeout) -> Self {
        Event::KeyFrameExchangeTimeout(timeout)
    }
}
impl From<InspectPulseCheck> for Event {
    fn from(this: InspectPulseCheck) -> Self {
        Event::InspectPulseCheck(this)
    }
}
impl From<SaeTimeout> for Event {
    fn from(this: SaeTimeout) -> Self {
        Event::SaeTimeout(this)
    }
}

#[derive(Debug, Clone)]
pub struct EstablishingRsnaTimeout;
impl TimeoutDuration for EstablishingRsnaTimeout {
    fn timeout_duration(&self) -> zx::Duration {
        ESTABLISHING_RSNA_TIMEOUT_SECONDS.seconds()
    }
}

#[derive(Debug, Clone)]
pub struct KeyFrameExchangeTimeout {
    pub bssid: MacAddr,
    pub sta_addr: MacAddr,
    pub frame: eapol::KeyFrameBuf,
    pub attempt: u32,
}
impl TimeoutDuration for KeyFrameExchangeTimeout {
    fn timeout_duration(&self) -> zx::Duration {
        KEY_FRAME_EXCHANGE_TIMEOUT_MILLIS.millis()
    }
}

#[derive(Debug, Clone)]
pub struct InspectPulseCheck;
impl TimeoutDuration for InspectPulseCheck {
    fn timeout_duration(&self) -> zx::Duration {
        INSPECT_PULSE_CHECK_MINUTES.minutes()
    }
}

#[derive(Debug, Clone)]
pub struct SaeTimeout {
    pub timer: wlan_sae::Timeout,
    pub id: u64,
}
impl TimeoutDuration for SaeTimeout {
    fn timeout_duration(&self) -> zx::Duration {
        match self.timer {
            wlan_sae::Timeout::Retransmission => SAE_RETRANSMISSION_TIMEOUT_MILLIS.millis(),
            wlan_sae::Timeout::KeyExpiration => SAE_PMKSA_EXPIRATION_TIMEOUT_HOURS.hours(),
        }
    }
}
