// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Custom error types for the netstack.

use failure::Fail;
use net_types::ip::{Ip, IpAddress};
use net_types::MulticastAddress;

use crate::wire::icmp::IcmpIpTypes;

/// Results returned from many functions in the netstack.
pub(crate) type Result<T> = std::result::Result<T, NetstackError>;

/// Results returned from parsing functions in the netstack.
pub(crate) type ParseResult<T> = std::result::Result<T, ParseError>;

/// Results returned from IP packet parsing functions in the netstack.
pub(crate) type IpParseResult<I, T> = std::result::Result<T, IpParseError<I>>;

/// Top-level error type the netstack.
#[derive(Fail, Debug)]
pub enum NetstackError {
    #[fail(display = "{}", _0)]
    /// Errors related to packet parsing.
    Parse(#[cause] ParseError),

    /// Error when item already exists.
    #[fail(display = "Item already exists")]
    Exists,

    /// Error when item is not found.
    #[fail(display = "Item not found")]
    NotFound,
    // Add error types here as we add more to the stack
}

/// Error type for packet parsing.
#[derive(Fail, Debug, PartialEq)]
pub enum ParseError {
    #[fail(display = "Operation is not supported")]
    NotSupported,
    #[fail(display = "Operation was not expected in this context")]
    NotExpected,
    #[fail(display = "Invalid checksum")]
    Checksum,
    #[fail(display = "Packet is not formatted properly")]
    Format,
}

/// Action to take when an error is encountered while parsing an IP packet.
#[derive(Debug, PartialEq)]
pub enum IpParseErrorAction {
    /// Discard the packet and do nothing further.
    DiscardPacket,

    /// Discard the packet and send an ICMP response.
    DiscardPacketSendICMP,

    /// Discard the packet and send an ICMP response if the packet's
    /// destination address was not a multicast address.
    DiscardPacketSendICMPNoMulticast,
}

impl IpParseErrorAction {
    /// Determines whether or not an ICMP message should be sent.
    ///
    /// Returns `true` if we should send an ICMP response. We should send
    /// an ICMP response if an action is set to `DiscardPacketSendICMP`, or
    /// if an action is set to `DiscardPacketSendICMPNoMulticast` and `dst_addr`
    /// (the destination address of the original packet that lead to a parsing
    /// error) is not a multicast address.
    pub(crate) fn should_send_icmp<A: IpAddress>(&self, dst_addr: &A) -> bool {
        match *self {
            IpParseErrorAction::DiscardPacket => false,
            IpParseErrorAction::DiscardPacketSendICMP => true,
            IpParseErrorAction::DiscardPacketSendICMPNoMulticast => !dst_addr.is_multicast(),
        }
    }

    /// Determines whether or not an ICMP message should be sent even if
    /// the original packet's destination address is a multicast.
    pub(crate) fn should_send_icmp_to_multicast(&self) -> bool {
        match *self {
            IpParseErrorAction::DiscardPacketSendICMP => true,
            IpParseErrorAction::DiscardPacket
            | IpParseErrorAction::DiscardPacketSendICMPNoMulticast => false,
        }
    }
}

/// Error type for IP packet parsing.
#[derive(Fail, Debug, PartialEq)]
pub(crate) enum IpParseError<I: Ip> {
    #[fail(display = "Parsing Error")]
    Parse { error: ParseError },
    /// For errors where an ICMP Parameter Problem error needs to be
    /// sent to the source of a packet.
    ///
    /// `src_ip` and `dst_ip` are the source and destination IP addresses of the
    /// original packet. `header_len` is the length of the header that we know
    /// about up to the point of the parameter problem error. `action` is the
    /// action we should take after encountering the error. If `must_send_icmp`
    /// is `true` we MUST send an ICMP response if `action` specifies it;
    /// otherwise, we MAY choose to just discard the packet and do nothing
    /// further. `code` is the ICMP (ICMPv4 or ICMPv6) specific parameter
    /// problem code that provides more granular information about the parameter
    /// problem encountered. `pointer` is the offset of the erroneous value within
    /// the IP packet, calculated from the beginning of the IP packet.
    #[fail(display = "Parameter Problem")]
    ParameterProblem {
        src_ip: I::Addr,
        dst_ip: I::Addr,
        code: <I as IcmpIpTypes>::ParameterProblemCode,
        pointer: <I as IcmpIpTypes>::ParameterProblemPointer,
        must_send_icmp: bool,
        header_len: usize,
        action: IpParseErrorAction,
    },
}

impl<I: Ip> From<ParseError> for IpParseError<I> {
    fn from(error: ParseError) -> Self {
        IpParseError::Parse { error }
    }
}

/// Error when something exists unexpectedly, such as trying to add an
/// element when the element is already present.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct ExistsError;

impl From<ExistsError> for NetstackError {
    fn from(_: ExistsError) -> NetstackError {
        NetstackError::Exists
    }
}

/// Error when something unexpectedly doesn't exist, such as trying to
/// remove an element when the element is not present.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct NotFoundError;

impl From<NotFoundError> for NetstackError {
    fn from(_: NotFoundError) -> NetstackError {
        NetstackError::NotFound
    }
}
