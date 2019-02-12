// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Parsing and serialization of ARP packets.

#![allow(private_in_public)]

#[cfg(test)]
use std::fmt::{self, Debug, Formatter};
use std::mem;

use byteorder::{ByteOrder, NetworkEndian};
use packet::{BufferView, BufferViewMut, InnerPacketBuilder, ParsablePacket, ParseMetadata};
use zerocopy::{AsBytes, ByteSlice, FromBytes, LayoutVerified, Unaligned};

use crate::device::arp::{ArpHardwareType, ArpOp};
use crate::device::ethernet::{EtherType, Mac};
use crate::error::ParseError;
use crate::ip::Ipv4Addr;

// Header has the same memory layout (thanks to repr(C, packed)) as an ARP
// header. Thus, we can simply reinterpret the bytes of the ARP header as a
// Header and then safely access its fields. Note the following caveats:
// - We cannot make any guarantees about the alignment of an instance of this
//   struct in memory or of any of its fields. This is true both because
//   repr(packed) removes the padding that would be used to ensure the alignment
//   of individual fields, but also because we are given no guarantees about
//   where within a given memory buffer a particular packet (and thus its
//   header) will be located.
// - Individual fields are all either u8 or [u8; N] rather than u16, u32, etc.
//   This is for two reasons:
//   - u16 and larger have larger-than-1 alignments, which are forbidden as
//     described above
//   - We are not guaranteed that the local platform has the same endianness as
//     network byte order (big endian), so simply treating a sequence of bytes
//     as a u16 or other multi-byte number would not necessarily be correct.
//     Instead, we use the NetworkEndian type and its reader and writer methods
//     to correctly access these fields.
#[derive(Default)]
#[repr(C, packed)]
struct Header {
    htype: [u8; 2], // Hardware (e.g. Ethernet)
    ptype: [u8; 2], // Protocol (e.g. IPv4)
    hlen: u8,       // Length (in octets) of hardware address
    plen: u8,       // Length (in octets) of protocol address
    oper: [u8; 2],  // Operation: 1 for Req, 2 for Reply
}

unsafe impl FromBytes for Header {}
unsafe impl AsBytes for Header {}
unsafe impl Unaligned for Header {}

impl Header {
    fn hardware_protocol(&self) -> u16 {
        NetworkEndian::read_u16(&self.htype)
    }

    fn set_hardware_protocol(&mut self, htype: ArpHardwareType, hlen: u8) -> &mut Self {
        NetworkEndian::write_u16(&mut self.htype, htype as u16);
        self.hlen = hlen;
        self
    }

    fn network_protocol(&self) -> u16 {
        NetworkEndian::read_u16(&self.ptype)
    }

    fn set_network_protocol(&mut self, ptype: EtherType, plen: u8) -> &mut Self {
        NetworkEndian::write_u16(&mut self.ptype, ptype.into());
        self.plen = plen;
        self
    }

    fn op_code(&self) -> u16 {
        NetworkEndian::read_u16(&self.oper)
    }

    fn set_op_code(&mut self, op: ArpOp) -> &mut Self {
        NetworkEndian::write_u16(&mut self.oper, op as u16);
        self
    }

    fn hardware_address_len(&self) -> u8 {
        self.hlen
    }

    fn protocol_address_len(&self) -> u8 {
        self.plen
    }
}

/// Peek at an ARP header to see what hardware and protocol address types are
/// used.
///
/// Since `ArpPacket` is statically typed with the hardware and protocol address
/// types expected in the header and body, these types must be known ahead of
/// time before calling `parse`. If multiple different types are valid in a
/// given parsing context, and so the caller cannot know ahead of time which
/// types to use, `peek_arp_types` can be used to peek at the header first to
/// figure out which static types should be used in a subsequent call to
/// `parse`.
///
/// Note that `peek_arp_types` only inspects certain fields in the header, and
/// so `peek_arp_types` succeeding does not guarantee that a subsequent call to
/// `parse` will also succeed.
pub fn peek_arp_types<B: ByteSlice>(bytes: B) -> Result<(ArpHardwareType, EtherType), ParseError> {
    let (header, _) = LayoutVerified::<B, Header>::new_unaligned_from_prefix(bytes)
        .ok_or_else(debug_err_fn!(ParseError::Format, "too few bytes for header"))?;
    let hw = ArpHardwareType::from_u16(header.hardware_protocol()).ok_or_else(debug_err_fn!(
        ParseError::NotSupported,
        "unrecognized hardware protocol: {:x}",
        header.hardware_protocol()
    ))?;
    let proto = EtherType::from_u16(header.network_protocol()).ok_or_else(debug_err_fn!(
        ParseError::NotSupported,
        "unrecognized network protocol: {:x}",
        header.network_protocol()
    ))?;
    let hlen = match hw {
        ArpHardwareType::Ethernet => <Mac as HType>::hlen(),
    };
    let plen = match proto {
        EtherType::Ipv4 => <Ipv4Addr as PType>::plen(),
        _ => {
            return debug_err!(
                Err(ParseError::NotSupported),
                "unsupported network protocol: {}",
                proto
            );
        }
    };
    if header.hardware_address_len() != hlen || header.protocol_address_len() != plen {
        return debug_err!(
            Err(ParseError::Format),
            "unexpected hardware or protocol address length for protocol {}",
            proto
        );
    }
    Ok((hw, proto))
}

// See comment on Header for an explanation of the memory safety requirements.
#[repr(C, packed)]
struct Body<HwAddr, ProtoAddr> {
    sha: HwAddr,
    spa: ProtoAddr,
    tha: HwAddr,
    tpa: ProtoAddr,
}

unsafe impl<HwAddr: FromBytes, ProtoAddr: FromBytes> FromBytes for Body<HwAddr, ProtoAddr> {}
unsafe impl<HwAddr: AsBytes, ProtoAddr: AsBytes> AsBytes for Body<HwAddr, ProtoAddr> {}
unsafe impl<HwAddr: Unaligned, ProtoAddr: Unaligned> Unaligned for Body<HwAddr, ProtoAddr> {}

impl<HwAddr: Copy, ProtoAddr: Copy> Body<HwAddr, ProtoAddr> {
    fn set_sha(&mut self, sha: HwAddr) -> &mut Self {
        self.sha = sha;
        self
    }

    fn set_spa(&mut self, spa: ProtoAddr) -> &mut Self {
        self.spa = spa;
        self
    }

    fn set_tha(&mut self, tha: HwAddr) -> &mut Self {
        self.tha = tha;
        self
    }

    fn set_tpa(&mut self, tpa: ProtoAddr) -> &mut Self {
        self.tpa = tpa;
        self
    }
}

/// A trait to represent a ARP hardware type.
pub trait HType: FromBytes + AsBytes + Unaligned + Copy + Clone {
    /// The hardware type.
    fn htype() -> ArpHardwareType;
    /// The in-memory size of an instance of the type.
    fn hlen() -> u8;
}

/// A trait to represent a ARP protocol type.
pub trait PType: FromBytes + AsBytes + Unaligned + Copy + Clone {
    /// The protocol type.
    fn ptype() -> EtherType;
    /// The in-memory size of an instance of the type.
    fn plen() -> u8;
    /// Returns a concrete instance of the protocol address
    ///
    /// This is a hack, since we only support Ipv4 - if we support more
    /// protocols in the future, we'll need to change this to return an enum of
    /// all the possible protocol addresses.
    fn addr(self) -> Ipv4Addr;
}

impl HType for Mac {
    fn htype() -> ArpHardwareType {
        ArpHardwareType::Ethernet
    }
    fn hlen() -> u8 {
        use std::convert::TryFrom;
        u8::try_from(mem::size_of::<Mac>()).unwrap()
    }
}

impl PType for Ipv4Addr {
    fn ptype() -> EtherType {
        EtherType::Ipv4
    }
    fn plen() -> u8 {
        use std::convert::TryFrom;
        u8::try_from(mem::size_of::<Ipv4Addr>()).unwrap()
    }
    fn addr(self) -> Ipv4Addr {
        self
    }
}

/// An ARP packet.
///
/// A `ArpPacket` shares its underlying memory with the byte slice it was parsed
/// from or serialized to, meaning that no copying or extra allocation is
/// necessary.
pub struct ArpPacket<B, HwAddr, ProtoAddr> {
    header: LayoutVerified<B, Header>,
    body: LayoutVerified<B, Body<HwAddr, ProtoAddr>>,
}

impl<B: ByteSlice, HwAddr, ProtoAddr> ParsablePacket<B, ()> for ArpPacket<B, HwAddr, ProtoAddr>
where
    HwAddr: Copy + HType + FromBytes + Unaligned,
    ProtoAddr: Copy + PType + FromBytes + Unaligned,
{
    type Error = ParseError;

    fn parse_metadata(&self) -> ParseMetadata {
        ParseMetadata::from_inner_packet(self.header.bytes().len() + self.body.bytes().len())
    }

    fn parse<BV: BufferView<B>>(mut buffer: BV, args: ()) -> Result<Self, ParseError> {
        let header = buffer
            .take_obj_front::<Header>()
            .ok_or_else(debug_err_fn!(ParseError::Format, "too few bytes for header"))?;
        let body = buffer
            .take_obj_front::<Body<HwAddr, ProtoAddr>>()
            .ok_or_else(debug_err_fn!(ParseError::Format, "too few bytes for body"))?;
        // Consume any padding bytes added by the previous layer.
        buffer.take_rest_front();

        if header.hardware_protocol() != <HwAddr as HType>::htype() as u16
            || header.network_protocol() != <ProtoAddr as PType>::ptype().into()
        {
            return debug_err!(
                Err(ParseError::NotExpected),
                "unexpected hardware or network protocols"
            );
        }
        if header.hardware_address_len() != <HwAddr as HType>::hlen()
            || header.protocol_address_len() != <ProtoAddr as PType>::plen()
        {
            return debug_err!(
                Err(ParseError::Format),
                "unexpected hardware or protocol address length"
            );
        }

        if ArpOp::from_u16(header.op_code()).is_none() {
            return debug_err!(
                Err(ParseError::Format),
                "unrecognized op code: {:x}",
                header.op_code()
            );
        }

        Ok(ArpPacket { header, body })
    }
}

impl<B: ByteSlice, HwAddr, ProtoAddr> ArpPacket<B, HwAddr, ProtoAddr>
where
    HwAddr: Copy + HType + FromBytes + Unaligned,
    ProtoAddr: Copy + PType + FromBytes + Unaligned,
{
    /// The type of ARP packet
    pub fn operation(&self) -> ArpOp {
        // This is verified in `parse`, so should be safe to unwrap
        ArpOp::from_u16(self.header.op_code()).unwrap()
    }

    /// The hardware address of the ARP packet sender.
    pub fn sender_hardware_address(&self) -> HwAddr {
        self.body.sha
    }

    /// The protocol address of the ARP packet sender.
    pub fn sender_protocol_address(&self) -> ProtoAddr {
        self.body.spa
    }

    /// The hardware address of the ARP packet target.
    pub fn target_hardware_address(&self) -> HwAddr {
        self.body.tha
    }

    /// The protocol address of the ARP packet target.
    pub fn target_protocol_address(&self) -> ProtoAddr {
        self.body.tpa
    }

    /// Construct a builder with the same contents as this packet.
    pub fn builder(&self) -> ArpPacketBuilder<HwAddr, ProtoAddr> {
        ArpPacketBuilder {
            op: self.operation(),
            sha: self.sender_hardware_address(),
            spa: self.sender_protocol_address(),
            tha: self.target_hardware_address(),
            tpa: self.target_protocol_address(),
        }
    }
}

/// A builder for ARP packets.
pub struct ArpPacketBuilder<HwAddr, ProtoAddr> {
    op: ArpOp,
    sha: HwAddr,
    spa: ProtoAddr,
    tha: HwAddr,
    tpa: ProtoAddr,
}

impl<HwAddr, ProtoAddr> ArpPacketBuilder<HwAddr, ProtoAddr> {
    /// Construct a new `ArpPacketBuilder`.
    pub fn new(
        operation: ArpOp,
        sender_hardware_addr: HwAddr,
        sender_protocol_addr: ProtoAddr,
        target_hardware_addr: HwAddr,
        target_protocol_addr: ProtoAddr,
    ) -> ArpPacketBuilder<HwAddr, ProtoAddr> {
        ArpPacketBuilder {
            op: operation,
            sha: sender_hardware_addr,
            spa: sender_protocol_addr,
            tha: target_hardware_addr,
            tpa: target_protocol_addr,
        }
    }
}

impl<HwAddr, ProtoAddr> InnerPacketBuilder for ArpPacketBuilder<HwAddr, ProtoAddr>
where
    HwAddr: Copy + HType + FromBytes + AsBytes + Unaligned,
    ProtoAddr: Copy + PType + FromBytes + AsBytes + Unaligned,
{
    fn bytes(&self) -> usize {
        mem::size_of::<Header>() + mem::size_of::<Body<HwAddr, ProtoAddr>>()
    }

    fn serialize(self, mut buffer: &mut [u8]) {
        // implements BufferViewMut, giving us take_obj_xxx_zero methods
        let mut buffer = &mut buffer;

        // SECURITY: Use _zero constructors to ensure we zero memory to prevent
        // leaking information from packets previously stored in this buffer.
        let mut header =
            buffer.take_obj_front_zero::<Header>().expect("not enough bytes for an ARP packet");
        let mut body = buffer
            .take_obj_front_zero::<Body<HwAddr, ProtoAddr>>()
            .expect("not enough bytes for an ARP packet");
        header
            .set_hardware_protocol(<HwAddr as HType>::htype(), <HwAddr as HType>::hlen())
            .set_network_protocol(<ProtoAddr as PType>::ptype(), <ProtoAddr as PType>::plen())
            .set_op_code(self.op);
        body.set_sha(self.sha).set_spa(self.spa).set_tha(self.tha).set_tpa(self.tpa);
    }
}

#[cfg(test)]
impl<B, HwAddr, ProtoAddr> Debug for ArpPacket<B, HwAddr, ProtoAddr> {
    fn fmt(&self, fmt: &mut Formatter) -> fmt::Result {
        write!(fmt, "ArpPacket")
    }
}

#[cfg(test)]
mod tests {
    use packet::{FnSerializer, ParseBuffer, Serializer};

    use super::*;
    use crate::ip::Ipv4Addr;
    use crate::wire::ethernet::EthernetFrame;

    const TEST_SENDER_IPV4: Ipv4Addr = Ipv4Addr::new([1, 2, 3, 4]);
    const TEST_TARGET_IPV4: Ipv4Addr = Ipv4Addr::new([5, 6, 7, 8]);
    const TEST_SENDER_MAC: Mac = Mac::new([0, 1, 2, 3, 4, 5]);
    const TEST_TARGET_MAC: Mac = Mac::new([6, 7, 8, 9, 10, 11]);

    #[test]
    fn test_parse_serialize_full() {
        crate::testutil::set_logger_for_test();
        use crate::wire::testdata::*;

        let mut req = &ARP_REQUEST[..];
        let frame = req.parse::<EthernetFrame<_>>().unwrap();
        assert_eq!(frame.ethertype(), Some(Ok(EtherType::Arp)));

        let (hw, proto) = peek_arp_types(frame.body()).unwrap();
        assert_eq!(hw, ArpHardwareType::Ethernet);
        assert_eq!(proto, EtherType::Ipv4);
        let mut body = frame.body();
        let arp = body.parse::<ArpPacket<_, Mac, Ipv4Addr>>().unwrap();
        assert_eq!(arp.operation(), ArpOp::Request);
        assert_eq!(frame.src_mac(), arp.sender_hardware_address());

        let frame_bytes = arp.builder().encapsulate(frame.builder()).serialize_outer();
        assert_eq!(frame_bytes.as_ref(), ARP_REQUEST);
    }

    fn header_to_bytes(header: Header) -> [u8; 8] {
        let mut bytes = [0; 8];
        {
            let mut lv = LayoutVerified::new_unaligned(&mut bytes[..]).unwrap();
            *lv = header;
        }
        bytes
    }

    // Return a new Header for an Ethernet/IPv4 ARP request.
    fn new_header() -> Header {
        let mut header = Header::default();
        header.set_hardware_protocol(<Mac as HType>::htype(), <Mac as HType>::hlen());
        header.set_network_protocol(<Ipv4Addr as PType>::ptype(), <Ipv4Addr as PType>::plen());
        header.set_op_code(ArpOp::Request);
        header
    }

    #[test]
    fn test_peek() {
        let header = new_header();
        let (hw, proto) = peek_arp_types(&header_to_bytes(header)[..]).unwrap();
        assert_eq!(hw, ArpHardwareType::Ethernet);
        assert_eq!(proto, EtherType::Ipv4);

        // Test that an invalid operation is not rejected; peek_arp_types does
        // not inspect the operation.
        let mut header = new_header();
        NetworkEndian::write_u16(&mut header.oper[..], 3);
        let (hw, proto) = peek_arp_types(&header_to_bytes(header)[..]).unwrap();
        assert_eq!(hw, ArpHardwareType::Ethernet);
        assert_eq!(proto, EtherType::Ipv4);
    }

    #[test]
    fn test_parse() {
        let mut buf = &mut [
            0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 5, 6, 7, 8,
        ][..];
        (&mut buf[..8]).copy_from_slice(&header_to_bytes(new_header()));
        let (hw, proto) = peek_arp_types(&buf[..]).unwrap();
        assert_eq!(hw, ArpHardwareType::Ethernet);
        assert_eq!(proto, EtherType::Ipv4);

        let mut buf = &mut buf;
        let packet = buf.parse::<ArpPacket<_, Mac, Ipv4Addr>>().unwrap();
        assert_eq!(packet.sender_hardware_address(), TEST_SENDER_MAC);
        assert_eq!(packet.sender_protocol_address(), TEST_SENDER_IPV4);
        assert_eq!(packet.target_hardware_address(), TEST_TARGET_MAC);
        assert_eq!(packet.target_protocol_address(), TEST_TARGET_IPV4);
        assert_eq!(packet.operation(), ArpOp::Request);
    }

    #[test]
    fn test_serialize() {
        let mut buf = FnSerializer::new_vec(ArpPacketBuilder::new(
            ArpOp::Request,
            TEST_SENDER_MAC,
            TEST_SENDER_IPV4,
            TEST_TARGET_MAC,
            TEST_TARGET_IPV4,
        ))
        .serialize_outer();
        assert_eq!(
            AsRef::<[u8]>::as_ref(&buf),
            &[0, 1, 8, 0, 6, 4, 0, 1, 0, 1, 2, 3, 4, 5, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 5, 6, 7, 8,]
        );
        let packet = buf.parse::<ArpPacket<_, Mac, Ipv4Addr>>().unwrap();
        assert_eq!(packet.sender_hardware_address(), TEST_SENDER_MAC);
        assert_eq!(packet.sender_protocol_address(), TEST_SENDER_IPV4);
        assert_eq!(packet.target_hardware_address(), TEST_TARGET_MAC);
        assert_eq!(packet.target_protocol_address(), TEST_TARGET_IPV4);
        assert_eq!(packet.operation(), ArpOp::Request);
    }

    #[test]
    fn test_peek_error() {
        // Test that a header which is too short is rejected.
        let buf = [0; 7];
        assert_eq!(peek_arp_types(&buf[..]).unwrap_err(), ParseError::Format);

        // Test that an unexpected hardware protocol type is rejected.
        let mut header = new_header();
        NetworkEndian::write_u16(&mut header.htype[..], 0);
        assert_eq!(
            peek_arp_types(&header_to_bytes(header)[..]).unwrap_err(),
            ParseError::NotSupported
        );

        // Test that an unexpected network protocol type is rejected.
        let mut header = new_header();
        NetworkEndian::write_u16(&mut header.ptype[..], 0);
        assert_eq!(
            peek_arp_types(&header_to_bytes(header)[..]).unwrap_err(),
            ParseError::NotSupported
        );

        // Test that an incorrect hardware address len is rejected.
        let mut header = new_header();
        header.hlen = 7;
        assert_eq!(peek_arp_types(&header_to_bytes(header)[..]).unwrap_err(), ParseError::Format);

        // Test that an incorrect protocol address len is rejected.
        let mut header = new_header();
        header.plen = 5;
        assert_eq!(peek_arp_types(&header_to_bytes(header)[..]).unwrap_err(), ParseError::Format);
    }

    #[test]
    fn test_parse_error() {
        // Assert that parsing a buffer results in an error.
        fn assert_err(mut buf: &[u8], err: ParseError) {
            assert_eq!(buf.parse::<ArpPacket<_, Mac, Ipv4Addr>>().unwrap_err(), err);
        }

        // Assert that parsing a particular header results in an error.
        fn assert_header_err(header: Header, err: ParseError) {
            let mut buf = [0; 28];
            *LayoutVerified::<_, Header>::new_unaligned_from_prefix(&mut buf[..]).unwrap().0 =
                header;
            assert_err(&buf[..], err);
        }

        // Test that a packet which is too short is rejected.
        let buf = [0; 27];
        assert_err(&[0; 27][..], ParseError::Format);

        let mut buf = [0; 28];

        // Test that an unexpected hardware protocol type is rejected.
        let mut header = new_header();
        NetworkEndian::write_u16(&mut header.htype[..], 0);
        assert_header_err(header, ParseError::NotExpected);

        // Test that an unexpected network protocol type is rejected.
        let mut header = new_header();
        NetworkEndian::write_u16(&mut header.ptype[..], 0);
        assert_header_err(header, ParseError::NotExpected);

        // Test that an incorrect hardware address len is rejected.
        let mut header = new_header();
        header.hlen = 7;
        assert_header_err(header, ParseError::Format);

        // Test that an incorrect protocol address len is rejected.
        let mut header = new_header();
        header.plen = 5;
        assert_header_err(header, ParseError::Format);

        // Test that an invalid operation is rejected.
        let mut header = new_header();
        NetworkEndian::write_u16(&mut header.oper[..], 3);
        assert_header_err(header, ParseError::Format);
    }

    #[test]
    fn test_serialize_zeroes() {
        // Test that ArpPacket::serialize properly zeroes memory before
        // serializing the packet.
        let mut buf_0 = [0; 28];
        InnerPacketBuilder::serialize(
            ArpPacketBuilder::new(
                ArpOp::Request,
                TEST_SENDER_MAC,
                TEST_SENDER_IPV4,
                TEST_TARGET_MAC,
                TEST_TARGET_IPV4,
            ),
            &mut buf_0[..],
        );
        let mut buf_1 = [0xFF; 28];
        InnerPacketBuilder::serialize(
            ArpPacketBuilder::new(
                ArpOp::Request,
                TEST_SENDER_MAC,
                TEST_SENDER_IPV4,
                TEST_TARGET_MAC,
                TEST_TARGET_IPV4,
            ),
            &mut buf_1[..],
        );
        assert_eq!(buf_0, buf_1);
    }

    #[test]
    #[should_panic]
    fn test_serialize_panic_insufficient_packet_space() {
        // Test that a buffer which doesn't leave enough room for the packet is
        // rejected.
        InnerPacketBuilder::serialize(
            ArpPacketBuilder::new(
                ArpOp::Request,
                TEST_SENDER_MAC,
                TEST_SENDER_IPV4,
                TEST_TARGET_MAC,
                TEST_TARGET_IPV4,
            ),
            &mut [0; 27],
        );
    }
}
