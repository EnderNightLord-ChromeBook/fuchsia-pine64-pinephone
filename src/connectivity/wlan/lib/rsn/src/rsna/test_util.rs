// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use crate::crypto_utils::nonce::NonceReader;
use crate::key::exchange::handshake::fourway::{self, Fourway};
use crate::key::exchange::{compute_mic, compute_mic_from_buf};
use crate::key::{
    gtk::{Gtk, GtkProvider},
    ptk::Ptk,
};
use crate::key_data::kde;
use crate::keywrap::keywrap_algorithm;
use crate::psk;
use crate::rsna::{Dot11VerifiedKeyFrame, NegotiatedProtection, SecAssocUpdate};
use crate::{Authenticator, Supplicant};
use eapol::KeyFrameTx;
use hex::FromHex;
use std::sync::{Arc, Mutex};
use wlan_common::ie::rsn::{
    akm::{self, Akm},
    cipher::{self, Cipher},
    rsne::Rsne,
    suite_selector::OUI,
};
use zerocopy::ByteSlice;

pub const S_ADDR: [u8; 6] = [0x81, 0x76, 0x61, 0x14, 0xDF, 0xC9];
pub const A_ADDR: [u8; 6] = [0x1D, 0xE3, 0xFD, 0xDF, 0xCB, 0xD3];

pub fn get_a_rsne() -> Rsne {
    let mut rsne = Rsne::new();
    rsne.group_data_cipher_suite = Some(Cipher { oui: OUI, suite_type: cipher::CCMP_128 });
    rsne.pairwise_cipher_suites.push(Cipher { oui: OUI, suite_type: cipher::CCMP_128 });
    rsne.pairwise_cipher_suites.push(Cipher { oui: OUI, suite_type: cipher::TKIP });
    rsne.akm_suites.push(Akm { oui: OUI, suite_type: akm::PSK });
    rsne
}

pub fn get_rsne_bytes(rsne: &Rsne) -> Vec<u8> {
    let mut buf = Vec::with_capacity(rsne.len());
    rsne.write_into(&mut buf).expect("error writing RSNE into buffer");
    buf
}

pub fn get_s_rsne() -> Rsne {
    let mut rsne = Rsne::new();
    rsne.group_data_cipher_suite = Some(Cipher { oui: OUI, suite_type: cipher::CCMP_128 });
    rsne.pairwise_cipher_suites.push(Cipher { oui: OUI, suite_type: cipher::CCMP_128 });
    rsne.akm_suites.push(Akm { oui: OUI, suite_type: akm::PSK });
    rsne
}

pub fn get_supplicant() -> Supplicant {
    let nonce_rdr = NonceReader::new(&S_ADDR[..]).expect("error creating Reader");
    let psk = psk::compute("ThisIsAPassword".as_bytes(), "ThisIsASSID".as_bytes())
        .expect("error computing PSK");
    Supplicant::new_wpa2psk_ccmp128(
        nonce_rdr,
        psk,
        test_util::S_ADDR,
        ProtectionInfo::Rsne(test_util::get_s_rsne()),
        test_util::A_ADDR,
        ProtectionInfo::Rsne(test_util::get_a_rsne()),
    )
    .expect("could not create Supplicant")
}

pub fn get_authenticator() -> Authenticator {
    let gtk_provider = GtkProvider::new(Cipher { oui: OUI, suite_type: cipher::CCMP_128 })
        .expect("error creating GtkProvider");
    let nonce_rdr = NonceReader::new(&S_ADDR[..]).expect("error creating Reader");
    let psk = psk::compute("ThisIsAPassword".as_bytes(), "ThisIsASSID".as_bytes())
        .expect("error computing PSK");
    Authenticator::new_wpa2psk_ccmp128(
        nonce_rdr,
        Arc::new(Mutex::new(gtk_provider)),
        psk,
        test_util::S_ADDR,
        ProtectionInfo::Rsne(test_util::get_s_rsne()),
        test_util::A_ADDR,
        ProtectionInfo::Rsne(test_util::get_a_rsne()),
    )
    .expect("could not create Authenticator")
}

pub fn get_ptk(anonce: &[u8], snonce: &[u8]) -> Ptk {
    let akm = get_akm();
    let s_rsne = get_s_rsne();
    let cipher = s_rsne
        .pairwise_cipher_suites
        .get(0)
        .expect("Supplicant's RSNE holds no Pairwise Cipher suite");
    let pmk = get_pmk();
    Ptk::new(&pmk[..], &A_ADDR, &S_ADDR, anonce, snonce, &akm, cipher.clone())
        .expect("error deriving PTK")
}

pub fn get_pmk() -> Vec<u8> {
    Vec::from_hex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af")
        .expect("error reading PMK from hex")
}

pub fn encrypt_key_data(kek: &[u8], key_data: &[u8]) -> Vec<u8> {
    let keywrap_alg =
        keywrap_algorithm(&get_akm()).expect("error AKM has no known keywrap Algorithm");
    keywrap_alg.wrap(kek, key_data).expect("could not encrypt key data")
}

pub fn mic_len() -> usize {
    get_akm().mic_bytes().expect("AKM has no known MIC size") as usize
}

pub fn get_akm() -> akm::Akm {
    get_s_rsne().akm_suites.remove(0)
}

pub fn get_4whs_msg1<'a, F>(anonce: &[u8], msg_modifier: F) -> eapol::KeyFrameBuf
where
    F: Fn(&mut KeyFrameTx),
{
    let mut msg1 = KeyFrameTx::new(
        eapol::ProtocolVersion::IEEE802DOT1X2001,
        eapol::KeyFrameFields::new(
            eapol::KeyDescriptor::IEEE802DOT11,
            eapol::KeyInformation(0x008a),
            16,
            1,
            eapol::to_array(anonce),
            [0u8; 16],
            0,
        ),
        vec![],
        mic_len(),
    );
    msg_modifier(&mut msg1);
    msg1.serialize().finalize_without_mic().expect("failed to construct 4whs msg 1")
}

pub fn get_4whs_msg3<'a, F>(
    ptk: &Ptk,
    anonce: &[u8],
    gtk: &[u8],
    msg_modifier: F,
) -> eapol::KeyFrameBuf
where
    F: Fn(&mut KeyFrameTx),
{
    let mut w = kde::Writer::new(vec![]);
    w.write_gtk(&kde::Gtk::new(2, kde::GtkInfoTx::BothRxTx, gtk)).expect("error writing GTK KDE");
    w.write_protection(&ProtectionInfo::Rsne(get_a_rsne())).expect("error writing RSNE");
    let key_data = w.finalize_for_encryption().expect("error finalizing key data");
    let encrypted_key_data = encrypt_key_data(ptk.kek(), &key_data[..]);

    let mut msg3 = KeyFrameTx::new(
        eapol::ProtocolVersion::IEEE802DOT1X2001,
        eapol::KeyFrameFields::new(
            eapol::KeyDescriptor::IEEE802DOT11,
            eapol::KeyInformation(0x13ca),
            16,
            2, // replay counter
            eapol::to_array(anonce),
            [0u8; 16], // iv
            0,         // rsc
        ),
        encrypted_key_data,
        mic_len(),
    );
    msg_modifier(&mut msg3);
    let msg3 = msg3.serialize();
    let mic = compute_mic_from_buf(ptk.kck(), &get_akm(), msg3.unfinalized_buf())
        .expect("failed to compute msg3 mic");
    msg3.finalize_with_mic(&mic[..]).expect("failed to construct 4whs msg 3")
}

pub fn get_group_key_hs_msg1(
    ptk: &Ptk,
    gtk: &[u8],
    key_id: u8,
    key_replay_counter: u64,
) -> eapol::KeyFrameBuf {
    let mut w = kde::Writer::new(vec![]);
    w.write_gtk(&kde::Gtk::new(key_id, kde::GtkInfoTx::BothRxTx, gtk))
        .expect("error writing GTK KDE");
    let key_data = w.finalize_for_encryption().expect("error finalizing key data");
    let encrypted_key_data = encrypt_key_data(ptk.kek(), &key_data[..]);

    let msg1 = KeyFrameTx::new(
        eapol::ProtocolVersion::IEEE802DOT1X2001,
        eapol::KeyFrameFields::new(
            eapol::KeyDescriptor::IEEE802DOT11,
            eapol::KeyInformation(0x1382),
            16,
            key_replay_counter,
            [0u8; 32], // nonce
            [0u8; 16], // iv
            0,         // rsc
        ),
        encrypted_key_data,
        mic_len(),
    );
    let msg1 = msg1.serialize();
    let mic = compute_mic_from_buf(ptk.kck(), &get_akm(), msg1.unfinalized_buf())
        .expect("failed to compute mic");
    msg1.finalize_with_mic(&mic[..]).expect("failed to construct group key hs msg 1")
}

pub fn is_zero(slice: &[u8]) -> bool {
    slice.iter().all(|&x| x == 0)
}

pub fn make_fourway_cfg(role: Role) -> fourway::Config {
    let gtk_provider = GtkProvider::new(Cipher { oui: OUI, suite_type: cipher::CCMP_128 })
        .expect("error creating GtkProvider");
    let nonce_rdr = NonceReader::new(&S_ADDR[..]).expect("error creating Reader");
    fourway::Config::new(
        role,
        test_util::S_ADDR,
        ProtectionInfo::Rsne(test_util::get_s_rsne()),
        test_util::A_ADDR,
        ProtectionInfo::Rsne(test_util::get_a_rsne()),
        nonce_rdr,
        Some(Arc::new(Mutex::new(gtk_provider))),
    )
    .expect("could not construct PTK exchange method")
}

pub fn make_handshake(role: Role) -> Fourway {
    let pmk = test_util::get_pmk();
    Fourway::new(make_fourway_cfg(role), pmk).expect("error while creating 4-Way Handshake")
}

pub fn finalize_key_frame(frame: &mut eapol::KeyFrameRx<&mut [u8]>, kck: Option<&[u8]>) {
    if let Some(kck) = kck {
        let mic = compute_mic(kck, &get_akm(), &frame).expect("failed to compute mic");
        frame.key_mic.copy_from_slice(&mic[..]);
    }
}

fn make_verified<B: ByteSlice + std::fmt::Debug>(
    frame: eapol::KeyFrameRx<B>,
    role: Role,
    key_replay_counter: u64,
) -> Dot11VerifiedKeyFrame<B> {
    let protection = NegotiatedProtection::from_rsne(&test_util::get_s_rsne())
        .expect("could not derive negotiated RSNE");

    let result = Dot11VerifiedKeyFrame::from_frame(frame, &role, &protection, key_replay_counter);
    assert!(result.is_ok(), "failed verifying message sent to {:?}: {}", role, result.unwrap_err());
    result.unwrap()
}

pub fn get_eapol_resp(updates: &[SecAssocUpdate]) -> Option<eapol::KeyFrameBuf> {
    updates
        .iter()
        .filter_map(|u| match u {
            SecAssocUpdate::TxEapolKeyFrame(resp) => Some(resp),
            _ => None,
        })
        .next()
        .map(|x| x.clone())
}

pub fn expect_eapol_resp(updates: &[SecAssocUpdate]) -> eapol::KeyFrameBuf {
    get_eapol_resp(updates).expect("updates do not contain EAPOL frame")
}

pub fn get_reported_ptk(updates: &[SecAssocUpdate]) -> Option<Ptk> {
    updates
        .iter()
        .filter_map(|u| match u {
            SecAssocUpdate::Key(Key::Ptk(ptk)) => Some(ptk),
            _ => None,
        })
        .next()
        .map(|x| x.clone())
}

pub fn expect_reported_ptk(updates: &[SecAssocUpdate]) -> Ptk {
    get_reported_ptk(updates).expect("updates do not contain PTK")
}

pub fn get_reported_gtk(updates: &[SecAssocUpdate]) -> Option<Gtk> {
    updates
        .iter()
        .filter_map(|u| match u {
            SecAssocUpdate::Key(Key::Gtk(gtk)) => Some(gtk),
            _ => None,
        })
        .next()
        .map(|x| x.clone())
}

pub fn expect_reported_gtk(updates: &[SecAssocUpdate]) -> Gtk {
    get_reported_gtk(updates).expect("updates do not contain GTK")
}

pub fn get_reported_status(updates: &[SecAssocUpdate]) -> Option<SecAssocStatus> {
    updates
        .iter()
        .filter_map(|u| match u {
            SecAssocUpdate::Status(status) => Some(status),
            _ => None,
        })
        .next()
        .map(|x| x.clone())
}

pub fn expect_reported_status(updates: &[SecAssocUpdate]) -> SecAssocStatus {
    get_reported_status(updates).expect("updates do not contain a status")
}

pub struct FourwayTestEnv {
    supplicant: Fourway,
    authenticator: Fourway,
}

impl FourwayTestEnv {
    pub fn new() -> FourwayTestEnv {
        FourwayTestEnv {
            supplicant: make_handshake(Role::Supplicant),
            authenticator: make_handshake(Role::Authenticator),
        }
    }

    pub fn initiate<'a>(&mut self, krc: u64) -> eapol::KeyFrameBuf {
        // Initiate 4-Way Handshake. The Authenticator will send message #1 of the handshake.
        let mut a_update_sink = vec![];
        let result = self.authenticator.initiate(&mut a_update_sink, krc);
        assert!(result.is_ok(), "Authenticator failed initiating: {}", result.unwrap_err());
        assert_eq!(a_update_sink.len(), 1);

        // Verify Authenticator sent message #1.
        expect_eapol_resp(&a_update_sink[..])
    }

    pub fn send_msg1_to_supplicant<'a, B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg1: eapol::KeyFrameRx<B>,
        krc: u64,
    ) -> (eapol::KeyFrameBuf, Ptk) {
        let anonce = msg1.key_frame_fields.key_nonce;
        let verified_msg1 = make_verified(msg1, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg1);
        assert!(result.is_ok(), "Supplicant failed processing msg #1: {}", result.unwrap_err());
        let msg2 = expect_eapol_resp(&s_update_sink[..]);
        let keyframe = msg2.keyframe();
        let ptk = get_ptk(&anonce[..], &keyframe.key_frame_fields.key_nonce[..]);

        (msg2, ptk)
    }

    pub fn send_msg1_to_supplicant_expect_err<B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg1: eapol::KeyFrameRx<B>,
        krc: u64,
    ) {
        let verified_msg1 = make_verified(msg1, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg1);
        assert!(result.is_err(), "Supplicant successfully processed illegal msg #1");
    }

    pub fn send_msg2_to_authenticator<'a, B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg2: eapol::KeyFrameRx<B>,
        expected_krc: u64,
        next_krc: u64,
    ) -> eapol::KeyFrameBuf {
        let verified_msg2 = make_verified(msg2, Role::Authenticator, expected_krc);

        // Send message #1 to Supplicant and extract responses.
        let mut a_update_sink = vec![];
        let result =
            self.authenticator.on_eapol_key_frame(&mut a_update_sink, next_krc, verified_msg2);
        assert!(result.is_ok(), "Authenticator failed processing msg #2: {}", result.unwrap_err());
        expect_eapol_resp(&a_update_sink[..])
    }

    pub fn send_msg3_to_supplicant<'a, B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg3: eapol::KeyFrameRx<B>,
        krc: u64,
    ) -> (eapol::KeyFrameBuf, Ptk, Gtk) {
        let verified_msg3 = make_verified(msg3, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg3);
        assert!(result.is_ok(), "Supplicant failed processing msg #3: {}", result.unwrap_err());
        let msg4 = expect_eapol_resp(&s_update_sink[..]);
        let s_ptk = expect_reported_ptk(&s_update_sink[..]);
        let s_gtk = expect_reported_gtk(&s_update_sink[..]);

        (msg4, s_ptk, s_gtk)
    }

    pub fn send_msg3_to_supplicant_capture_updates<B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg3: eapol::KeyFrameRx<B>,
        krc: u64,
        mut update_sink: &mut UpdateSink,
    ) {
        let verified_msg3 = make_verified(msg3, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let result = self.supplicant.on_eapol_key_frame(&mut update_sink, 0, verified_msg3);
        assert!(result.is_ok(), "Supplicant failed processing msg #3: {}", result.unwrap_err());
    }

    pub fn send_msg3_to_supplicant_expect_err<B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg3: eapol::KeyFrameRx<B>,
        krc: u64,
    ) {
        let verified_msg3 = make_verified(msg3, Role::Supplicant, krc);

        // Send message #1 to Supplicant and extract responses.
        let mut s_update_sink = vec![];
        let result = self.supplicant.on_eapol_key_frame(&mut s_update_sink, 0, verified_msg3);
        assert!(result.is_err(), "Supplicant successfully processed illegal msg #3");
    }

    pub fn send_msg4_to_authenticator<B: ByteSlice + std::fmt::Debug>(
        &mut self,
        msg4: eapol::KeyFrameRx<B>,
        expected_krc: u64,
    ) -> (Ptk, Gtk) {
        let verified_msg4 = make_verified(msg4, Role::Authenticator, expected_krc);

        // Send message #1 to Supplicant and extract responses.
        let mut a_update_sink = vec![];
        let result =
            self.authenticator.on_eapol_key_frame(&mut a_update_sink, expected_krc, verified_msg4);
        assert!(result.is_ok(), "Authenticator failed processing msg #4: {}", result.unwrap_err());
        let a_ptk = expect_reported_ptk(&a_update_sink[..]);
        let a_gtk = expect_reported_gtk(&a_update_sink[..]);

        (a_ptk, a_gtk)
    }
}
