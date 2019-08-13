// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::MgmtSubtype,
    zerocopy::{ByteSlice, LayoutVerified},
};

mod fields;
mod reason;
mod status;

pub use {fields::*, reason::*, status::*};

#[derive(Debug)]
pub enum MgmtBody<B: ByteSlice> {
    Beacon { bcn_hdr: LayoutVerified<B, BeaconHdr>, elements: B },
    Authentication { auth_hdr: LayoutVerified<B, AuthHdr>, elements: B },
    AssociationReq { assoc_req_hdr: LayoutVerified<B, AssocReqHdr>, elements: B },
    AssociationResp { assoc_resp_hdr: LayoutVerified<B, AssocRespHdr>, elements: B },
    Unsupported { subtype: MgmtSubtype },
}

impl<B: ByteSlice> MgmtBody<B> {
    pub fn parse(subtype: MgmtSubtype, bytes: B) -> Option<Self> {
        match subtype {
            MgmtSubtype::BEACON => {
                let (bcn_hdr, elements) = LayoutVerified::new_unaligned_from_prefix(bytes)?;
                Some(MgmtBody::Beacon { bcn_hdr, elements })
            }
            MgmtSubtype::AUTH => {
                let (auth_hdr, elements) = LayoutVerified::new_unaligned_from_prefix(bytes)?;
                Some(MgmtBody::Authentication { auth_hdr, elements })
            }
            MgmtSubtype::ASSOC_REQ => {
                let (assoc_req_hdr, elements) = LayoutVerified::new_unaligned_from_prefix(bytes)?;
                Some(MgmtBody::AssociationReq { assoc_req_hdr, elements })
            }
            MgmtSubtype::ASSOC_RESP => {
                let (assoc_resp_hdr, elements) = LayoutVerified::new_unaligned_from_prefix(bytes)?;
                Some(MgmtBody::AssociationResp { assoc_resp_hdr, elements })
            }
            subtype => Some(MgmtBody::Unsupported { subtype }),
        }
    }
}

#[cfg(test)]
mod tests {
    use {super::*, crate::assert_variant, crate::mac::*};

    #[test]
    fn mgmt_hdr_len() {
        assert_eq!(MgmtHdr::len(HtControl::ABSENT), 24);
        assert_eq!(MgmtHdr::len(HtControl::PRESENT), 28);
    }

    #[test]
    fn parse_beacon_frame() {
        #[rustfmt::skip]
            let bytes = vec![
            1,1,1,1,1,1,1,1, // timestamp
            2,2, // beacon_interval
            3,3, // capabilities
            0,5,1,2,3,4,5 // SSID IE: "12345"
        ];
        assert_variant!(
            MgmtBody::parse(MgmtSubtype::BEACON, &bytes[..]),
            Some(MgmtBody::Beacon { bcn_hdr, elements }) => {
                assert_eq!(0x0101010101010101, { bcn_hdr.timestamp });
                assert_eq!(0x0202, { bcn_hdr.beacon_interval });
                assert_eq!(0x0303, { bcn_hdr.capabilities.0 });
                assert_eq!(&[0, 5, 1, 2, 3, 4, 5], &elements[..]);
            },
            "expected beacon frame"
        );
    }
}
