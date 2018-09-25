// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(test)]
#![deny(warnings)]

use bitfield::{bitfield, bitfield_debug, bitfield_fields, bitfield_struct};
use bytes::{BufMut, Bytes};
use nom::{be_u16, be_u64, be_u8, call, do_parse, eof, error_position, map, named_args, take, verify};
use std::convert::AsMut;

pub trait FrameReceiver {
    fn on_eapol_frame(&self, frame: &Frame) -> Result<(), failure::Error>;
}

pub trait KeyFrameReceiver {
    fn on_eapol_key_frame(&self, frame: &KeyFrame) -> Result<(), failure::Error>;
}

#[derive(Debug, PartialEq)]
pub enum Frame {
    Key(KeyFrame),
}

// IEEE Std 802.1X-2010, 11.9, Table 11-5
#[derive(Debug, PartialEq)]
pub enum KeyDescriptor {
    Rc4 = 1,
    Ieee802dot11 = 2,
}

impl KeyDescriptor {
    pub fn from_u8(n: u8) -> Option<KeyDescriptor> {
        match n {
            1 => Some(KeyDescriptor::Rc4),
            2 => Some(KeyDescriptor::Ieee802dot11),
            _ => None,
        }
    }
}

// IEEE Std 802.11-2016, 12.7.2 b.2)
pub const KEY_TYPE_GROUP_SMK: u16 = 0;
pub const KEY_TYPE_PAIRWISE: u16 = 1;

// IEEE Std 802.1X-2010, 11.3.1
#[derive(Debug)]
pub enum ProtocolVersion {
    Ieee802dot1x2001 = 1,
    Ieee802dot1x2004 = 2,
    Ieee802dot1x2010 = 3,
}

// IEEE Std 802.1X-2010, 11.3.2, Table 11-3
pub enum PacketType {
    Eap = 0,
    Start = 1,
    Logoff = 2,
    Key = 3,
    AsfAlert = 4,
    Mka = 5,
    AnnouncementGeneric = 6,
    AnnouncementSpecific = 7,
    AnnouncementReq = 8,
}

// IEEE Std 802.11-2016, 12.7.2, Figure 12-33
bitfield! {
    #[derive(PartialEq)]
    pub struct KeyInformation(u16);
    impl Debug;
    pub key_descriptor_version, set_key_descriptor_version: 2, 0;
    pub key_type, set_key_type: 3, 3;
    // Bit 4-5 reserved.
    pub install, set_install: 6;
    pub key_ack, set_key_ack: 7;
    pub key_mic, set_key_mic: 8;
    pub secure, set_secure: 9;
    pub error, set_error: 10;
    pub request, set_request: 11;
    pub encrypted_key_data, set_encrypted_key_data: 12;
    pub smk_message, set_smk_message: 13;
    // Bit 14-15 reserved.

    pub value, _: 15,0;
}

impl Clone for KeyInformation {
    fn clone(&self) -> KeyInformation {
        KeyInformation(self.value())
    }
}

impl Default for KeyInformation {
    fn default() -> KeyInformation {
        KeyInformation(0)
    }
}

// IEEE Std 802.11-2016, 12.7.2, Figure 12-32
#[derive(Default, Debug, Clone, PartialEq)]
pub struct KeyFrame {
    pub version: u8,
    pub packet_type: u8,
    pub packet_body_len: u16,

    pub descriptor_type: u8,
    pub key_info: KeyInformation,
    pub key_len: u16,
    pub key_replay_counter: u64,
    pub key_nonce: [u8; 32],
    pub key_iv: [u8; 16],
    pub key_rsc: u64,
    // 8 octests reserved.
    pub key_mic: Bytes, /* AKM dependent size */
    pub key_data_len: u16,
    pub key_data: Bytes,
}

impl KeyFrame {
    pub fn len(&self) -> usize {
        let static_part_len: usize = 83;
        let dynamic_part_len: usize = self.key_mic.len() + self.key_data.len();
        static_part_len + dynamic_part_len
    }

    pub fn as_bytes(&self, clear_mic: bool, buf: &mut Vec<u8>) {
        buf.reserve(self.len());

        buf.put_u8(self.version);
        buf.put_u8(self.packet_type);
        buf.put_u16_be(self.packet_body_len);
        buf.put_u8(self.descriptor_type);
        buf.put_u16_be(self.key_info.value());
        buf.put_u16_be(self.key_len);
        buf.put_u64_be(self.key_replay_counter);
        buf.put_slice(&self.key_nonce[..]);
        buf.put_slice(&self.key_iv[..]);
        buf.put_u64_be(self.key_rsc);
        buf.put_uint_be(0, 8);
        if clear_mic {
            let zeroes: Vec<u8> = vec![0; self.key_mic.len()];
            buf.put_slice(&zeroes[..]);
        } else {
            buf.put_slice(&self.key_mic[..]);
        }
        buf.put_u16_be(self.key_data_len);
        buf.put_slice(&self.key_data[..]);
    }

    pub fn update_packet_body_len(&mut self) {
        // Minimum length of an EAPOL Key frame without its dynamic MIC and Key Data field.
        let min_len = 79;
        self.packet_body_len = min_len + self.key_mic.len() as u16 + self.key_data_len;
    }
}

pub fn to_array<A>(slice: &[u8]) -> A
    where
        A: Sized + Default + AsMut<[u8]>,
{
    let mut array = Default::default();
    <A as AsMut<[u8]>>::as_mut(&mut array).clone_from_slice(slice);
    array
}

named_args!(pub key_frame_from_bytes(mic_size: u16) <KeyFrame>,
       do_parse!(
           version: be_u8 >>
           packet_type: verify!(be_u8, |v:u8| v == PacketType::Key as u8) >>
           packet_body_len: be_u16 >>

           descriptor_type: be_u8 >>
           key_info: map!(be_u16, KeyInformation) >>
           key_len: be_u16 >>
           key_replay_counter: be_u64 >>
           key_nonce: take!(32) >>
           key_iv: take!(16) >>
           key_rsc: be_u64 >>
           take!(8 /* reserved octets */) >>
           key_mic: take!(mic_size) >>
           key_data_len: be_u16 >>
           key_data: take!(key_data_len) >>
           eof!() >>
           (KeyFrame{
               version: version,
               packet_type: packet_type,
               packet_body_len: packet_body_len,
               descriptor_type: descriptor_type,
               key_info: key_info,
               key_len: key_len,
               key_replay_counter: key_replay_counter,
               key_mic: Bytes::from(key_mic),
               key_rsc: key_rsc,
               key_iv: to_array(key_iv),
               key_nonce: to_array(key_nonce),
               key_data_len: key_data_len,
               key_data: Bytes::from(key_data),
           })
    )
);

#[cfg(test)]
mod tests {
    use super::*;
    use test::{black_box, Bencher};

    #[bench]
    fn bench_key_frame_from_bytes(b: &mut Bencher) {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x54, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01,
            0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03,
            0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02,
            0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01,
            0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03,
            0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02, 0x03, 0x01, 0x02,
            0x03,
        ];
        b.iter(|| key_frame_from_bytes(&frame, black_box(16)));
    }

    #[test]
    fn test_key_info() {
        let value = 0b1010_0000_0000_0000u16;
        let key_info = KeyInformation(value);
        assert_eq!(key_info.key_descriptor_version(), 0);
        assert!(key_info.smk_message());
        assert_eq!(key_info.value(), value);
        let cloned = key_info.clone();
        assert_eq!(key_info.value(), cloned.value());
    }

    #[test]
    fn test_no_key_frame() {
        let frame: Vec<u8> = vec![
            0x01, 0x01, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(!result.is_done());
    }

    #[test]
    fn test_too_long() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03, 0x04,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(!result.is_done());
    }

    #[test]
    fn test_too_short() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(!result.is_done());
    }

    #[test]
    fn test_dynamic_mic_size() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x6f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
            0x07, 0x08, 0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1,
            0x22, 0x79, 0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38,
            0x98, 0x25, 0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        let result = key_frame_from_bytes(&frame, 32);
        assert!(result.is_done());
    }

    #[test]
    fn test_as_bytes() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(result.is_done());
        let keyframe: KeyFrame = result.unwrap().1;
        verify_as_bytes_result(keyframe, false, &frame[..]);
    }

    #[test]
    fn test_as_bytes_too_small() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(result.is_done());
        let keyframe: KeyFrame = result.unwrap().1;

        // Buffer is too small to write entire frame to.
        let mut buf = Vec::with_capacity(frame.len() - 1);
        keyframe.as_bytes(false, &mut buf);
        verify_as_bytes_result(keyframe, false, &buf[..]);
    }

    #[test]
    fn test_as_bytes_dynamic_mic_size() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x6f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
            0x07, 0x08, 0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1,
            0x22, 0x79, 0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38,
            0x98, 0x25, 0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        let result = key_frame_from_bytes(&frame, 32);
        assert!(result.is_done());
        let keyframe: KeyFrame = result.unwrap().1;
        verify_as_bytes_result(keyframe, false, &frame[..]);
    }

    #[test]
    fn test_as_bytes_clear_mic() {
        #[cfg_attr(rustfmt, rustfmt_skip)]
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            // MIC
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
            0x0F, 0x10,
            0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(result.is_done());
        let keyframe: KeyFrame = result.unwrap().1;

        #[cfg_attr(rustfmt, rustfmt_skip)]
        let expected: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            // Cleared MIC
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00,
            0x00, 0x03, 0x01, 0x02, 0x03,
        ];
        verify_as_bytes_result(keyframe, true, &expected[..]);
    }

    fn verify_as_bytes_result(keyframe: KeyFrame, clear_mic: bool, expected: &[u8]) {
        let mut buf = Vec::with_capacity(128);
        keyframe.as_bytes(clear_mic, &mut buf);
        let written = buf.len();
        let left_over = buf.split_off(written);
        assert_eq!(&buf[..], expected);
        assert!(left_over.iter().all(|b| *b == 0));
    }

    #[test]
    fn test_correct_packet() {
        let frame: Vec<u8> = vec![
            0x01, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79,
            0xfe, 0xc3, 0xb9, 0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25,
            0xf8, 0xc7, 0xca, 0x55, 0x86, 0xbc, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x03, 0x01, 0x02, 0x03,
        ];
        let result = key_frame_from_bytes(&frame, 16);
        assert!(result.is_done());
        let keyframe: KeyFrame = result.unwrap().1;
        assert_eq!(keyframe.version, 1);
        assert_eq!(keyframe.packet_type, 3);
        assert_eq!(keyframe.packet_body_len, 95);
        assert_eq!(keyframe.descriptor_type, 2);
        assert_eq!(keyframe.key_info.value(), 0x008a);
        assert_eq!(keyframe.key_info.key_descriptor_version(), 2);
        assert!(keyframe.key_info.key_ack());
        assert_eq!(keyframe.key_len, 16);
        assert_eq!(keyframe.key_replay_counter, 1);
        let nonce: Vec<u8> = vec![
            0x39, 0x5c, 0xc7, 0x6e, 0x1a, 0xe9, 0x9f, 0xa0, 0xb1, 0x22, 0x79, 0xfe, 0xc3, 0xb9,
            0xa9, 0x9e, 0x1d, 0x9a, 0x21, 0xb8, 0x47, 0x51, 0x38, 0x98, 0x25, 0xf8, 0xc7, 0xca,
            0x55, 0x86, 0xbc, 0xda,
        ];
        assert_eq!(&keyframe.key_nonce[..], &nonce[..]);
        assert_eq!(keyframe.key_rsc, 0);
        let mic = [0; 16];
        assert_eq!(&keyframe.key_mic[..], mic);
        assert_eq!(keyframe.key_data_len, 3);
        let data: Vec<u8> = vec![0x01, 0x02, 0x03];
        assert_eq!(&keyframe.key_data[..], &data[..]);
    }
}
