// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std;
use std::fmt::{self, Debug, Display, Formatter};
use std::hash::Hash;

use byteorder::{ByteOrder, NetworkEndian};
use zerocopy::{AsBytes, FromBytes, Unaligned};

/// An IP protocol version.
#[allow(missing_docs)]
#[derive(Copy, Clone, Eq, PartialEq, Debug)]
pub enum IpVersion {
    V4,
    V6,
}

impl IpVersion {
    /// The number for this IP protocol version.
    ///
    /// 4 for `V4` and 6 for `V6`.
    pub fn version_number(&self) -> u8 {
        match self {
            IpVersion::V4 => 4,
            IpVersion::V6 => 6,
        }
    }

    /// Is this IPv4?
    pub fn is_v4(&self) -> bool {
        *self == IpVersion::V4
    }

    /// Is this IPv6?
    pub fn is_v6(&self) -> bool {
        *self == IpVersion::V6
    }
}

mod sealed {
    // Ensure that only Ipv4 and Ipv6 can implement IpVersion and that only
    // Ipv4Addr and Ipv6Addr can implement IpAddr.
    pub trait Sealed {}

    impl Sealed for super::Ipv4 {}
    impl Sealed for super::Ipv6 {}
    impl Sealed for super::Ipv4Addr {}
    impl Sealed for super::Ipv6Addr {}
}

/// A trait for IP protocol versions.
///
/// `Ip` encapsulates the details of a version of the IP protocol. It includes
/// the `IpVersion` enum (`VERSION`) and address type (`Addr`). It is
/// implemented by `Ipv4` and `Ipv6`.
pub trait Ip: Sized + self::sealed::Sealed {
    /// The IP version.
    ///
    /// `V4` for IPv4 and `V6` for IPv6.
    const VERSION: IpVersion;

    /// The default loopback address.
    ///
    /// When sending packets to a loopback interface, this address is used as
    /// the source address. It is an address in the loopback subnet.
    const LOOPBACK_ADDRESS: Self::Addr;

    /// The subnet of loopback addresses.
    ///
    /// Addresses in this subnet must not appear outside a host, and may only be
    /// used for loopback interfaces.
    const LOOPBACK_SUBNET: Subnet<Self::Addr>;

    /// The address type for this IP version.
    ///
    /// `Ipv4Addr` for IPv4 and `Ipv6Addr` for IPv6.
    type Addr: IpAddr<Version = Self>;
}

/// IPv4.
///
/// `Ipv4` implements `Ip` for IPv4.
#[derive(Debug, Default)]
pub struct Ipv4;

impl Ip for Ipv4 {
    const VERSION: IpVersion = IpVersion::V4;

    // https://tools.ietf.org/html/rfc5735#section-3
    const LOOPBACK_ADDRESS: Ipv4Addr = Ipv4Addr::new([127, 0, 0, 1]);
    const LOOPBACK_SUBNET: Subnet<Ipv4Addr> =
        Subnet { network: Ipv4Addr::new([127, 0, 0, 0]), prefix: 8 };
    type Addr = Ipv4Addr;
}

impl Ipv4 {
    /// The global broadcast address.
    ///
    /// This address is considered to be a broadcast address on all networks
    /// regardless of subnet address. This is distinct from the subnet-specific
    /// broadcast address (e.g., 192.168.255.255 on the subnet 192.168.0.0/16).
    pub const BROADCAST_ADDRESS: Ipv4Addr = Ipv4Addr::new([255, 255, 255, 255]);
}

/// IPv6.
///
/// `Ipv6` implements `Ip` for IPv6.
#[derive(Debug, Default)]
pub struct Ipv6;

impl Ip for Ipv6 {
    const VERSION: IpVersion = IpVersion::V6;
    const LOOPBACK_ADDRESS: Ipv6Addr =
        Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]);
    const LOOPBACK_SUBNET: Subnet<Ipv6Addr> = Subnet {
        network: Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
        prefix: 128,
    };
    type Addr = Ipv6Addr;
}

/// An IPv4 or IPv6 address.
pub trait IpAddr
where
    Self: Sized + Eq + PartialEq + Hash + Copy + Display + Debug + self::sealed::Sealed,
{
    /// The number of bytes in an address of this type.
    ///
    /// 4 for IPv4 and 16 for IPv6.
    const BYTES: u8;

    /// The IP version type of this address.
    ///
    /// `Ipv4` for `Ipv4Addr` and `Ipv6` for `Ipv6Addr`.
    type Version: Ip<Addr = Self>;

    /// Get the underlying bytes of the address.
    fn bytes(&self) -> &[u8];

    /// Mask off the top bits of the address.
    ///
    /// Return a copy of `self` where all but the top `bits` bits are set to 0.
    fn mask(&self, bits: u8) -> Self;
}

/// An IPv4 address.
#[derive(Copy, Clone, Default, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Ipv4Addr([u8; 4]);

unsafe impl FromBytes for Ipv4Addr {}
unsafe impl AsBytes for Ipv4Addr {}
unsafe impl Unaligned for Ipv4Addr {}

impl Ipv4Addr {
    /// Create a new IPv4 address.
    pub const fn new(bytes: [u8; 4]) -> Self {
        Ipv4Addr(bytes)
    }

    /// Get the bytes of the IPv4 address.
    pub const fn ipv4_bytes(&self) -> [u8; 4] {
        self.0
    }
}

impl IpAddr for Ipv4Addr {
    const BYTES: u8 = 4;

    type Version = Ipv4;

    fn mask(&self, bits: u8) -> Self {
        assert!(bits <= 32);
        if bits == 0 {
            // shifting left by the size of the value is undefined
            Ipv4Addr([0; 4])
        } else {
            let mask = <u32>::max_value() << (32 - bits);
            let masked = NetworkEndian::read_u32(&self.0) & mask;
            let mut ret = Ipv4Addr::default();
            NetworkEndian::write_u32(&mut ret.0, masked);
            ret
        }
    }

    fn bytes(&self) -> &[u8] {
        &self.0
    }
}

impl From<std::net::Ipv4Addr> for Ipv4Addr {
    fn from(ip: std::net::Ipv4Addr) -> Self {
        Ipv4Addr::new(ip.octets())
    }
}

impl Display for Ipv4Addr {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        write!(f, "{}.{}.{}.{}", self.0[0], self.0[1], self.0[2], self.0[3])
    }
}

impl Debug for Ipv4Addr {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// An IPv6 address.
#[derive(Copy, Clone, Default, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Ipv6Addr([u8; 16]);

unsafe impl FromBytes for Ipv6Addr {}
unsafe impl AsBytes for Ipv6Addr {}
unsafe impl Unaligned for Ipv6Addr {}

impl Ipv6Addr {
    /// Create a new IPv6 address.
    pub const fn new(bytes: [u8; 16]) -> Self {
        Ipv6Addr(bytes)
    }

    /// Get the bytes of the IPv6 address.
    pub const fn ipv6_bytes(&self) -> [u8; 16] {
        self.0
    }
}

impl IpAddr for Ipv6Addr {
    const BYTES: u8 = 16;

    type Version = Ipv6;

    fn mask(&self, bits: u8) -> Self {
        assert!(bits <= 128);
        if bits == 0 {
            // shifting left by the size of the value is undefined
            Ipv6Addr([0; 16])
        } else {
            let mask = <u128>::max_value() << (128 - bits);
            let masked = NetworkEndian::read_u128(&self.0) & mask;
            let mut ret = Ipv6Addr::default();
            NetworkEndian::write_u128(&mut ret.0, masked);
            ret
        }
    }

    fn bytes(&self) -> &[u8] {
        &self.0
    }
}

impl From<std::net::Ipv6Addr> for Ipv6Addr {
    fn from(ip: std::net::Ipv6Addr) -> Self {
        Ipv6Addr::new(ip.octets())
    }
}

impl Display for Ipv6Addr {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        let to_u16 = |idx| NetworkEndian::read_u16(&self.0[idx..idx + 2]);
        Display::fmt(
            &std::net::Ipv6Addr::new(
                to_u16(0),
                to_u16(2),
                to_u16(4),
                to_u16(6),
                to_u16(8),
                to_u16(10),
                to_u16(12),
                to_u16(14),
            ),
            f,
        )
    }
}

impl Debug for Ipv6Addr {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// An IP subnet.
///
/// `Subnet` is a combination of an IP network address and a prefix length.
#[derive(Copy, Clone)]
pub struct Subnet<A: IpAddr> {
    // invariant: normalized to contain only prefix bits
    network: A,
    prefix: u8,
}

impl<A: IpAddr> Subnet<A> {
    /// Create a new subnet.
    ///
    /// Create a new subnet with the given network address and prefix length.
    ///
    /// # Panics
    ///
    /// `new` panics if `prefix` is longer than the number of bits in this type
    /// of IP address (32 for IPv4 and 128 for IPv6).
    pub fn new(network: A, prefix: u8) -> Subnet<A> {
        assert!(prefix <= A::BYTES * 8);
        let network = network.mask(prefix);
        Subnet { network, prefix }
    }

    /// Get the network address component of this subnet.
    ///
    /// `network` returns the network address component of this subnet. Any bits
    /// beyond the prefix will be zero.
    pub fn network(&self) -> A {
        self.network
    }

    /// Get the prefix length component of this subnet.
    pub fn prefix(&self) -> u8 {
        self.prefix
    }

    /// Test whether an address is in this subnet.
    ///
    /// Test whether `address` is in this subnet by testing whether the prefix
    /// bits match the prefix bits of the subnet's network address. This is
    /// equivalent to `subnet.network() == address.mask(subnet.prefix())`.
    pub fn contains(&self, address: A) -> bool {
        self.network == address.mask(self.prefix)
    }
}

impl Subnet<Ipv4Addr> {
    /// Get the broadcast address in this subnet.
    pub fn broadcast(&self) -> Ipv4Addr {
        let bits = 32 - self.prefix;
        if bits == 32 {
            // shifting right by the size of the value is undefined
            Ipv4Addr([0xFF; 4])
        } else {
            let mask = <u32>::max_value() >> (32 - bits);
            let masked = NetworkEndian::read_u32(&self.network.0) | mask;
            let mut ret = Ipv4Addr::default();
            NetworkEndian::write_u32(&mut ret.0, masked);
            ret
        }
    }
}

impl<A: IpAddr> Display for Subnet<A> {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        write!(f, "{}/{}", self.network, self.prefix)
    }
}

impl<A: IpAddr> Debug for Subnet<A> {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        write!(f, "{}/{}", self.network, self.prefix)
    }
}

/// An IP protocol or next header number.
///
/// For IPv4, this is the protocol number. For IPv6, this is the next header
/// number.
#[allow(missing_docs)]
#[derive(Copy, Clone, Hash, Eq, PartialEq)]
pub enum IpProto {
    Icmp,
    Tcp,
    Udp,
    Icmpv6,
    Other(u8),
}

impl IpProto {
    const ICMP: u8 = 1;
    const TCP: u8 = 6;
    const UDP: u8 = 17;
    const ICMPV6: u8 = 58;
}

impl From<u8> for IpProto {
    fn from(u: u8) -> IpProto {
        match u {
            Self::ICMP => IpProto::Icmp,
            Self::TCP => IpProto::Tcp,
            Self::UDP => IpProto::Udp,
            Self::ICMPV6 => IpProto::Icmpv6,
            u => IpProto::Other(u),
        }
    }
}

impl Into<u8> for IpProto {
    fn into(self) -> u8 {
        match self {
            IpProto::Icmp => Self::ICMP,
            IpProto::Tcp => Self::TCP,
            IpProto::Udp => Self::UDP,
            IpProto::Icmpv6 => Self::ICMPV6,
            IpProto::Other(u) => u,
        }
    }
}

impl Display for IpProto {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        write!(
            f,
            "{}",
            match self {
                IpProto::Icmp => "ICMP",
                IpProto::Tcp => "TCP",
                IpProto::Udp => "UDP",
                IpProto::Icmpv6 => "ICMPv6",
                IpProto::Other(u) => return write!(f, "IP protocol {}", u),
            }
        )
    }
}

impl Debug for IpProto {
    fn fmt(&self, f: &mut Formatter) -> Result<(), fmt::Error> {
        Display::fmt(self, f)
    }
}

/// An IPv4 header option.
///
/// An IPv4 header option comprises metadata about the option (which is stored
/// in the kind byte) and the option itself. Note that all kind-byte-only
/// options are handled by the utilities in `wire::util::options`, so this type
/// only supports options with variable-length data.
///
/// See [Wikipedia] or [RFC 791] for more details.
///
/// [Wikipedia]: https://en.wikipedia.org/wiki/IPv4#Options
/// [RFC 791]: https://tools.ietf.org/html/rfc791#page-15
pub struct Ipv4Option<'a> {
    /// Whether this option needs to be copied into all fragments of a fragmented packet.
    pub copied: bool,
    // TODO(joshlf): include "Option Class"?
    /// The variable-length option data.
    pub data: Ipv4OptionData<'a>,
}

/// The data associated with an IPv4 header option.
///
/// `Ipv4OptionData` represents the variable-length data field of an IPv4 header
/// option.
#[allow(missing_docs)]
pub enum Ipv4OptionData<'a> {
    // The maximum header length is 60 bytes, and the fixed-length header is 20
    // bytes, so there are 40 bytes for the options. That leaves a maximum
    // options size of 1 kind byte + 1 length byte + 38 data bytes.
    /// Data for an unrecognized option kind.
    ///
    /// Any unrecognized option kind will have its data parsed using this
    /// variant. This allows code to copy unrecognized options into packets when
    /// forwarding.
    ///
    /// `data`'s length is in the range [0, 38].
    Unrecognized { kind: u8, len: u8, data: &'a [u8] },
}

#[cfg(test)]
mod tests {
    use super::*;

    macro_rules! add_mask_test {
        ($name:ident, $addr:ident, $from_ip:expr => {
          $($mask:expr => $to_ip:expr),*
        }) => {
            #[test]
            fn $name() {
                let from = $addr::new($from_ip);
                $(
                  assert_eq!(from.mask($mask), $addr::new($to_ip), "(`{}`.mask({}))", from, $mask);
                )*
            }
        };

        ($name:ident, $addr:ident, $from_ip:expr => {
          $($mask:expr => $to_ip:expr),*,
        }) => (
          add_mask_test!($name, $addr, $from_ip => { $($mask => $to_ip),* });
        )
    }

    add_mask_test!(v4_full_mask, Ipv4Addr, [255, 254, 253, 252] => {
        32 => [255, 254, 253, 252],
        28 => [255, 254, 253, 240],
        24 => [255, 254, 253, 0],
        20 => [255, 254, 240, 0],
        16 => [255, 254, 0,   0],
        12 => [255, 240, 0,   0],
        8  => [255, 0,   0,   0],
        4  => [240, 0,   0,   0],
        0  => [0,   0,   0,   0],
    });

    add_mask_test!(v6_full_mask, Ipv6Addr,
        [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0] => {
          128 => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0],
          112 => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0x00, 0x00],
          96  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0xF5, 0xF4, 0x00, 0x00, 0x00, 0x00],
          80  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0xF7, 0xF6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          64  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          48  => [0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          32  => [0xFF, 0xFE, 0xFD, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          16  => [0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          8   => [0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          0   => [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
        }
    );
}
