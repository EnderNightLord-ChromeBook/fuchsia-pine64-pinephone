// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::u32;

use super::*;

#[derive(Debug)]
/// AVRCP 1.6.1 section 6.7.1 GetPlayStatus
#[allow(dead_code)] // TODO(BT-2218): WIP. Remove once used.
pub struct GetPlayStatusCommand {}

impl GetPlayStatusCommand {
    #[allow(dead_code)] // TODO(BT-2218): WIP. Remove once used.
    pub fn new() -> GetPlayStatusCommand {
        Self {}
    }
}

impl VendorDependent for GetPlayStatusCommand {
    fn pdu_id(&self) -> PduId {
        PduId::GetPlayStatus
    }
}

impl Decodable for GetPlayStatusCommand {
    fn decode(_buf: &[u8]) -> PacketResult<Self> {
        Ok(Self {})
    }
}

impl Encodable for GetPlayStatusCommand {
    fn encoded_len(&self) -> usize {
        0
    }

    fn encode(&self, _buf: &mut [u8]) -> PacketResult<()> {
        Ok(())
    }
}

const SONG_LENGTH_LEN: usize = 4;
const SONG_POSITION_LEN: usize = 4;
const PLAYSTATUS_LEN: usize = 1;
const RESPONSE_LEN: usize = SONG_LENGTH_LEN + SONG_POSITION_LEN + PLAYSTATUS_LEN;

/// AVRCP 1.6.1 section 6.7.1 GetPlayStatus
#[allow(dead_code)] // TODO(BT-2218): WIP. Remove once used.
pub struct GetPlayStatusResponse {
    pub song_length: u32,
    pub song_position: u32,
    pub playback_status: PlaybackStatus,
}

impl GetPlayStatusResponse {
    #[allow(dead_code)] // TODO(BT-2218): WIP. Remove once used.
    /// Time is encoded as milliseconds. Max value is (2^32 – 1)
    pub fn new(
        song_length: u32,
        song_position: u32,
        playback_status: PlaybackStatus,
    ) -> GetPlayStatusResponse {
        Self { song_length, song_position, playback_status }
    }
}

impl VendorDependent for GetPlayStatusResponse {
    fn pdu_id(&self) -> PduId {
        PduId::GetPlayStatus
    }
}

impl Decodable for GetPlayStatusResponse {
    fn decode(buf: &[u8]) -> PacketResult<Self> {
        if buf.len() < RESPONSE_LEN {
            return Err(Error::InvalidMessage);
        }

        let mut temp = [0; SONG_LENGTH_LEN];
        temp.copy_from_slice(&buf[0..SONG_LENGTH_LEN]);
        let song_length = u32::from_be_bytes(temp);

        temp = [0; SONG_POSITION_LEN];
        temp.copy_from_slice(&buf[SONG_LENGTH_LEN..SONG_LENGTH_LEN + SONG_POSITION_LEN]);
        let song_position = u32::from_be_bytes(temp);

        let playback_status = PlaybackStatus::try_from(buf[SONG_LENGTH_LEN + SONG_POSITION_LEN])?;

        Ok(Self { song_length, song_position, playback_status })
    }
}

impl Encodable for GetPlayStatusResponse {
    fn encoded_len(&self) -> usize {
        RESPONSE_LEN
    }

    fn encode(&self, buf: &mut [u8]) -> PacketResult<()> {
        if buf.len() < self.encoded_len() {
            return Err(Error::OutOfRange);
        }

        let sl_bytes = u32::to_be_bytes(self.song_length);
        let sp_bytes = u32::to_be_bytes(self.song_position);

        buf[0..SONG_LENGTH_LEN].copy_from_slice(&sl_bytes);
        buf[SONG_LENGTH_LEN..SONG_LENGTH_LEN + SONG_POSITION_LEN].copy_from_slice(&sp_bytes);
        buf[SONG_LENGTH_LEN + SONG_POSITION_LEN] = u8::from(&self.playback_status);

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_play_status_command_encode() {
        let b = GetPlayStatusCommand::new();
        assert_eq!(b.encoded_len(), 0);
        let mut buf = vec![0; b.encoded_len()];
        assert!(b.encode(&mut buf[..]).is_ok());
        assert_eq!(buf, &[]);
    }

    #[test]
    fn test_get_play_status_command_decode() {
        let b = GetPlayStatusCommand::decode(&[]).expect("unable to decode");
        assert_eq!(b.encoded_len(), 0);
        let mut buf = vec![0; b.encoded_len()];
        assert!(b.encode(&mut buf[..]).is_ok());
        assert_eq!(buf, &[]);
    }

    #[test]
    fn test_get_play_status_response_encode() {
        let b = GetPlayStatusResponse::new(0x64, 0x102095, PlaybackStatus::Playing);
        assert_eq!(b.encoded_len(), 9);
        let mut buf = vec![0; b.encoded_len()];
        assert!(b.encode(&mut buf[..]).is_ok());
        assert_eq!(
            buf,
            &[
                0x00, 0x00, 0x00, 0x64, // length
                0x00, 0x10, 0x20, 0x95, // position
                0x01, // Playing
            ]
        );
    }

    #[test]
    fn test_get_play_status_response_decode() {
        let b = GetPlayStatusResponse::decode(&[
            0x00, 0x00, 0x00, 0x64, // length
            0x00, 0x10, 0x20, 0x90, // position
            0x03, // FwdSeek
        ])
        .expect("unable to decode packet");
        assert_eq!(b.encoded_len(), 9);
        assert_eq!(b.playback_status, PlaybackStatus::FwdSeek);
        assert_eq!(b.song_length, 0x64);
        assert_eq!(b.song_position, 0x102090);
    }
}
