// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod handshake;

use self::handshake::{
    fourway::{self, Fourway},
    group_key::{self, GroupKey},
};
use crate::integrity::integrity_algorithm;
use crate::key::gtk::Gtk;
use crate::key::ptk::Ptk;
use crate::rsna::{Dot11VerifiedKeyFrame, UpdateSink};
use crate::Error;
use failure;
use wlan_common::ie::rsn::akm::Akm;
use zerocopy::ByteSlice;

#[derive(Debug, Clone, PartialEq)]
pub enum Key {
    Pmk(Vec<u8>),
    Ptk(Ptk),
    Gtk(Gtk),
    Igtk(Vec<u8>),
    MicRx(Vec<u8>),
    MicTx(Vec<u8>),
    Smk(Vec<u8>),
    Stk(Vec<u8>),
}

impl Key {
    pub fn name(&self) -> &'static str {
        match self {
            Key::Pmk(..) => "PMK",
            Key::Ptk(..) => "PTK",
            Key::Gtk(..) => "GTK",
            Key::Igtk(..) => "IGTK",
            Key::MicRx(..) => "MIC_RX",
            Key::MicTx(..) => "MIC_TX",
            Key::Smk(..) => "SMK",
            Key::Stk(..) => "STK",
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum Method {
    FourWayHandshake(Fourway),
    GroupKeyHandshake(GroupKey),
}

impl Method {
    pub fn on_eapol_key_frame<B: ByteSlice>(
        &mut self,
        update_sink: &mut UpdateSink,
        key_replay_counter: u64,
        frame: Dot11VerifiedKeyFrame<B>,
    ) -> Result<(), failure::Error> {
        match self {
            Method::FourWayHandshake(hs) => {
                hs.on_eapol_key_frame(update_sink, key_replay_counter, frame)
            }
            Method::GroupKeyHandshake(hs) => {
                hs.on_eapol_key_frame(update_sink, key_replay_counter, frame)
            }
        }
    }
    pub fn initiate(
        &mut self,
        update_sink: &mut UpdateSink,
        key_replay_counter: u64,
    ) -> Result<(), failure::Error> {
        match self {
            Method::FourWayHandshake(hs) => hs.initiate(update_sink, key_replay_counter),
            // Only 4-Way Handshake supports initiation so far.
            _ => Ok(()),
        }
    }

    pub fn destroy(self) -> Config {
        match self {
            Method::FourWayHandshake(hs) => hs.destroy(),
            Method::GroupKeyHandshake(hs) => hs.destroy(),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Config {
    FourWayHandshake(fourway::Config),
    GroupKeyHandshake(group_key::Config),
}

/// Computes and returns a serialized key frame's MIC.
/// Fails if the AKM has no associated integrity algorithm or MIC size.
pub fn compute_mic_from_buf(kck: &[u8], akm: &Akm, frame: &[u8]) -> Result<Vec<u8>, Error> {
    let integrity_alg = integrity_algorithm(&akm).ok_or(Error::UnsupportedAkmSuite)?;
    let mic_len = akm.mic_bytes().ok_or(Error::UnsupportedAkmSuite)? as usize;
    let mut mic = integrity_alg.compute(kck, frame)?;
    mic.truncate(mic_len);
    Ok(mic)
}

/// Computes and returns a key frame's MIC.
/// Fails if the AKM has no associated integrity algorithm or MIC size, the given Key Frame's MIC
/// has a different size than the MIC length derived from the AKM or the Key Frame doesn't have its
/// MIC bit set.
pub fn compute_mic<B: ByteSlice>(
    kck: &[u8],
    akm: &Akm,
    frame: &eapol::KeyFrameRx<B>,
) -> Result<Vec<u8>, Error> {
    let integrity_alg = integrity_algorithm(&akm).ok_or(Error::UnsupportedAkmSuite)?;
    let mic_len = akm.mic_bytes().ok_or(Error::UnsupportedAkmSuite)? as usize;
    if !frame.key_frame_fields.key_info().key_mic() {
        return Err(Error::ComputingMicForUnprotectedFrame);
    }
    if frame.key_mic.len() != mic_len {
        return Err(Error::MicSizesDiffer(frame.key_mic.len(), mic_len));
    }

    let mut buf = vec![];
    frame.write_into(true, &mut buf).expect("write_into should never fail for Vec");
    let written = buf.len();
    buf.truncate(written);
    let mut mic = integrity_alg.compute(kck, &buf[..])?;
    mic.truncate(mic_len);
    Ok(mic)
}

#[cfg(test)]
mod tests {
    use super::*;
    use wlan_common::ie::rsn::akm::PSK;

    fn fake_key_frame(mic_len: usize) -> eapol::KeyFrameTx {
        let key_info = eapol::KeyInformation(0).with_key_mic(true);
        eapol::KeyFrameTx::new(
            eapol::ProtocolVersion::IEEE802DOT1X2010,
            eapol::KeyFrameFields::new(
                eapol::KeyDescriptor::IEEE802DOT11,
                key_info,
                16,
                0,
                [0u8; 32],
                [0u8; 16],
                0,
            ),
            vec![],
            mic_len,
        )
    }

    #[test]
    fn compute_mic_unknown_akm() {
        const KCK: [u8; 16] = [5; 16];
        let frame = fake_key_frame(16)
            .serialize()
            .finalize_with_mic(&[0u8; 16][..])
            .expect("failed to create fake key frame");
        let err = compute_mic(&KCK[..], &Akm::new_dot11(200), &frame.keyframe())
            .expect_err("expected failure with unsupported AKM");
        match err {
            Error::UnsupportedAkmSuite => (),
            other => panic!("received unexpected error type: {}", other),
        }
    }

    #[test]
    fn compute_mic_bit_not_set() {
        const KCK: [u8; 16] = [5; 16];
        let mut frame = fake_key_frame(16);
        frame.key_frame_fields.set_key_info(eapol::KeyInformation(0));
        let frame =
            frame.serialize().finalize_without_mic().expect("failed to create fake key frame");
        let err = compute_mic(&KCK[..], &Akm::new_dot11(PSK), &frame.keyframe())
            .expect_err("expected failure with MIC bit not being set");
        match err {
            Error::ComputingMicForUnprotectedFrame => (),
            other => panic!("received unexpected error type: {}", other),
        }
    }

    #[test]
    fn compute_mic_different_mic_sizes() {
        const KCK: [u8; 16] = [5; 16];
        let frame = fake_key_frame(0)
            .serialize()
            .finalize_with_mic(&[][..])
            .expect("failed to create fake key frame");
        let err = compute_mic(&KCK[..], &Akm::new_dot11(PSK), &frame.keyframe())
            .expect_err("expected failure with different MIC sizes");
        match err {
            Error::MicSizesDiffer(0, 16) => (),
            other => panic!("received unexpected error type: {}", other),
        }
    }

    #[test]
    fn compute_mic_success() {
        const KCK: [u8; 16] = [5; 16];
        let psk = Akm::new_dot11(PSK);
        let frame = fake_key_frame(16)
            .serialize()
            .finalize_with_mic(&[0u8; 16][..])
            .expect("failed to create fake key frame");
        let mic = compute_mic(&KCK[..], &psk, &frame.keyframe())
            .expect("expected failure with unsupported AKM");

        let integrity_alg =
            integrity_algorithm(&psk).expect("expected known integrity algorithm for PSK");
        assert!(integrity_alg.verify(&KCK[..], &frame[..], &mic[..]));
    }

}
