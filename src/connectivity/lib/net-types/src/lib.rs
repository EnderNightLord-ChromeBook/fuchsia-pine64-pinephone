// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Networking types and operations.
//!
//! This crate defines types and operations useful for operating with various
//! network protocols. Some general utilities are defined in the crate root,
//! while protocol-specific operations are defined in their own modules.

#![feature(never_type)]
#![cfg_attr(not(std), no_std)]

#[cfg(std)]
extern crate core;

pub mod ethernet;
pub mod ip;

use core::fmt::{self, Display, Formatter};
use core::ops::Deref;

mod sealed {
    // Used to ensure that certain traits cannot be implemented by anyone
    // outside this crate, such as the Ip and IpAddress traits.
    pub trait Sealed {}
}

/// A type which is a witness to some property about an address.
///
/// A type which implements `Witness<A>` wraps an address of type `A` and
/// guarantees some property about the wrapped address. It is implemented by
/// [`SpecifiedAddr`], [`UnicastAddr`], [`MulticastAddr`], and
/// [`LinkLocalAddr`].
pub trait Witness<A>: Deref<Target = A> + sealed::Sealed + Sized {
    /// Constructs a new witness type..
    ///
    /// `new` returns `None` if `addr` does not satisfy the property guaranteed
    /// by `Self`.
    fn new(addr: A) -> Option<Self>;

    /// Get a clone of the address.
    #[inline]
    fn get(&self) -> A
    where
        A: Clone,
    {
        self.deref().clone()
    }

    /// Consumes this witness and returns the contained `A`.
    fn into_addr(self) -> A;
}

// NOTE: The "witness" types UnicastAddr, MulticastAddr, and LinkLocalAddr -
// which provide the invariant that the value they contain is a unicast,
// multicast, or link-local address, respectively - cannot actually guarantee
// this property without certain promises from the implementations of the
// UnicastAddress, MulticastAddress, and LinkLocalAddress traits that they rely
// on. In particular, the values must be "immutable" in the sense that, given
// only immutable references to the values, nothing about the values can change
// such that the "unicast-ness", "multicast-ness" or "link-local-ness" of the
// values change. Since the UnicastAddress, MulticastAddress, and
// LinkLocalAddress traits are not unsafe traits, it would be unsound for unsafe
// code to rely for its soundness on this behavior. For a more in-depth
// discussion of why this isn't possible without an explicit opt-in on the part
// of the trait implementor, see this forum thread:
// https://users.rust-lang.org/t/prevent-interior-mutability/29403

/// Addresses that can be specified.
///
/// `SpecifiedAddress` is implemented by address types for which some values are
/// considered [unspecified] addresses. Unspecified addresses are usually not
/// legal to be used in actual network traffic, and are only meant to represent
/// the lack of any defined address. The exact meaning of the unspecified
/// address often varies by context. For example, the IPv4 address 0.0.0.0 and
/// the IPv6 address :: can be used, in the context of creating a listening
/// socket on systems that use the BSD sockets API, to listen on all local IP
/// addresses rather than a particular one.
///
/// [unspecified]: https://en.wikipedia.org/wiki/0.0.0.0
pub trait SpecifiedAddress {
    /// Is this a specified address?
    ///
    /// `is_specified` must maintain the invariant that, if it is called twice
    /// on the same object, and in between those two calls, no code has operated
    /// on a mutable reference to that object, both calls will return the same
    /// value. This property is required in order to implement
    /// [`SpecifiedAddr`]. Note that, since this is not an `unsafe` trait,
    /// `unsafe` code may NOT rely on this property for its soundness. However,
    /// code MAY rely on this property for its correctness.
    fn is_specified(&self) -> bool;
}

/// Addresses that can be unicast.
///
/// `UnicastAddress` is implemented by address types for which some values are
/// considered [unicast] addresses. Unicast addresses are used to identify a
/// single network node, as opposed to broadcast and multicast addresses, which
/// identify a group of nodes.
///
/// `UnicastAddress` is only implemented for addresses whose unicast-ness can be
/// determined by looking only at the address itself (this is notably not true
/// for IPv4 addresses, which can be considered broadcast addresses depending on
/// the subnet in which they are used).
///
/// [unicast]: https://en.wikipedia.org/wiki/Unicast
pub trait UnicastAddress {
    /// Is this a unicast address?
    ///
    /// `is_unicast` must maintain the invariant that, if it is called twice on
    /// the same object, and in between those two calls, no code has operated on
    /// a mutable reference to that object, both calls will return the same
    /// value. This property is required in order to implement [`UnicastAddr`].
    /// Note that, since this is not an `unsafe` trait, `unsafe` code may NOT
    /// rely on this property for its soundness. However, code MAY rely on this
    /// property for its correctness.
    ///
    /// If this type also implements [`SpecifiedAddress`], then `a.is_unicast()`
    /// implies `a.is_specified()`.
    fn is_unicast(&self) -> bool;
}

/// Addresses that can be multicast.
///
/// `MulticastAddress` is implemented by address types for which some values are
/// considered [multicast] addresses. Multicast addresses are used to identify a
/// group of multiple network nodes, as opposed to unicast addresses, which
/// identify a single node, or broadcast addresses, which identify all the nodes
/// in some region of a network.
///
/// [multicast]: https://en.wikipedia.org/wiki/Multicast
pub trait MulticastAddress {
    /// Is this a unicast address?
    ///
    /// `is_multicast` must maintain the invariant that, if it is called twice
    /// on the same object, and in between those two calls, no code has operated
    /// on a mutable reference to that object, both calls will return the same
    /// value. This property is required in order to implement
    /// [`MulticastAddr`]. Note that, since this is not an `unsafe` trait,
    /// `unsafe` code may NOT rely on this property for its soundness. However,
    /// code MAY rely on this property for its correctness.
    ///
    /// If this type also implements [`SpecifiedAddress`], then
    /// `a.is_multicast()` implies `a.is_specified()`.
    fn is_multicast(&self) -> bool;
}

/// Addresses that can be broadcast.
///
/// `BroadcastAddress` is implemented by address types for which some values are
/// considered [broadcast] addresses. Broadcast addresses are used to identify
/// all the nodes in some region of a network, as opposed to unicast addresses,
/// which identify a single node, or multicast addresses, which identify a group
/// of nodes (not necessarily all of them).
///
/// [broadcast]: https://en.wikipedia.org/wiki/Broadcasting_(networking)
pub trait BroadcastAddress {
    /// Is this a broadcast address?
    ///
    /// If this type also implements [`SpecifiedAddress`], then
    /// `a.is_broadcast()` implies `a.is_specified()`.
    fn is_broadcast(&self) -> bool;
}

/// Addresses that can be a link-local.
///
/// `LinkLocalAddress` is implemented by address types for which some values are
/// considered [link-local] addresses. Link-local addresses are used for
/// communication within a network segment, as opposed to global/public
/// addresses which may be used for communication across networks.
///
/// `LinkLocalAddress` is only implemented for addresses whose link-local-ness
/// can be determined by looking only at the address itself.
///
/// [link-local]: https://en.wikipedia.org/wiki/Link-local_address
pub trait LinkLocalAddress {
    /// Is this a link-local address?
    ///
    /// `is_linklocal` must maintain the invariant that, if it is called twice
    /// on the same object, and in between those two calls, no code has operated
    /// on a mutable reference to that object, both calls will return the same
    /// value. This property is required in order to implement
    /// [`LinkLocalAddr`]. Note that, since this is not an `unsafe` trait,
    /// `unsafe` code may NOT rely on this property for its soundness. However,
    /// code MAY rely on this property for its correctness.
    ///
    /// If this type also implements [`SpecifiedAddress`], then
    /// `a.is_linklocal()` implies `a.is_specified()`.
    fn is_linklocal(&self) -> bool;
}

/// The scope used by an implementation of [`ScopeableAddress`].
///
/// A scope is either the `Global` scope or some `NonGlobal` scope, `S`.
pub enum Scope<S> {
    Global,
    NonGlobal(S),
}

impl<S> Scope<S> {
    /// Is this the global scope?
    ///
    /// `is_global` returns true if `self` is [`Scope::Global`], and false
    /// otherwise.
    pub fn is_global(&self) -> bool {
        match self {
            Scope::Global => true,
            Scope::NonGlobal(_) => false,
        }
    }

    /// Map `Scope<S>` to `Scope<T>` by applying a function to the non-global
    /// scope.
    pub fn map<T, F: FnOnce(S) -> T>(self, f: F) -> Scope<T> {
        match self {
            Scope::Global => Scope::Global,
            Scope::NonGlobal(scope) => Scope::NonGlobal(f(scope)),
        }
    }
}

/// An address that can be tied to some scope identifier.
///
/// `ScopeableAddress` is implemented by address types for which some values can
/// have extra scoping information attached. Notably, some IPv6 addresses
/// belonging to a particular scope class require extra metadata to identify the
/// scope identifier. The scope identifier is typically the networking interface
/// identifier.
///
/// Address types which are never in any identified scope may still implement
/// `ScopeableAddress` by setting the associated `NonGlobalScope` type to the
/// never type, which has the effect of ensuring that the [`scope`] method
/// always returns [`Scope::Global`].
pub trait ScopeableAddress {
    /// The type of all non-global scopes.
    type NonGlobalScope;

    /// The scope of this address.
    ///
    /// `scope` must maintain the invariant that, if it is called twice on the
    /// same object, and in between those two calls, no code has operated on a
    /// mutable reference to that object, both calls will return the same value.
    /// This property is required in order to implement [`AddrAndZone`]. Note
    /// that, since this is not an `unsafe` trait, `unsafe` code may NOT rely on
    /// this property for its soundness. However, code MAY rely on this property
    /// for its correctness.
    ///
    /// If this type also implements [`SpecifiedAddress`] then `a.scope() !=
    /// Scope::Global` implies `a.is_specified()`, since the unspecified
    /// addresses are always global.
    fn scope(&self) -> Scope<Self::NonGlobalScope>;
}

/// An address which is guaranteed to be a specified address.
///
/// `SpecifiedAddr` wraps an address of type `A` and guarantees that it is a
/// specified address. Note that this guarantee is contingent on a correct
/// implementation of the [`SpecifiedAddress`] trait. Since that trait is not
/// `unsafe`, `unsafe` code may NOT rely on this guarantee for its soundness.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct SpecifiedAddr<A>(A);

impl<A: SpecifiedAddress> sealed::Sealed for SpecifiedAddr<A> {}
impl<A: SpecifiedAddress> Witness<A> for SpecifiedAddr<A> {
    #[inline]
    fn new(addr: A) -> Option<SpecifiedAddr<A>> {
        if !addr.is_specified() {
            return None;
        }
        Some(SpecifiedAddr(addr))
    }

    #[inline]
    fn into_addr(self) -> A {
        self.0
    }
}

impl<A> SpecifiedAddr<A> {
    /// Constructs a new `SpecifiedAddr` without checking to see if `addr` is
    /// actually a specified address.
    ///
    /// # Safety
    ///
    /// It is up to the caller to make sure that `addr` is a specified address
    /// to avoid breaking the guarantees of `SpecifiedAddr`. See
    /// [`SpecifiedAddr`] for more details.
    #[inline]
    pub const unsafe fn new_unchecked(addr: A) -> SpecifiedAddr<A> {
        SpecifiedAddr(addr)
    }
}

impl<A: SpecifiedAddress> Deref for SpecifiedAddr<A> {
    type Target = A;

    #[inline]
    fn deref(&self) -> &A {
        &self.0
    }
}

impl<A: SpecifiedAddress + Display> Display for SpecifiedAddr<A> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

/// An address which is guaranteed to be a unicast address.
///
/// `UnicastAddr` wraps an address of type `A` and guarantees that it is a
/// unicast address. Note that this guarantee is contingent on a correct
/// implementation of the [`UnicastAddress`] trait. Since that trait is not
/// `unsafe`, `unsafe` code may NOT rely on this guarantee for its soundness.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct UnicastAddr<A>(A);

impl<A: UnicastAddress> sealed::Sealed for UnicastAddr<A> {}
impl<A: UnicastAddress> Witness<A> for UnicastAddr<A> {
    #[inline]
    fn new(addr: A) -> Option<UnicastAddr<A>> {
        if !addr.is_unicast() {
            return None;
        }
        Some(UnicastAddr(addr))
    }

    #[inline]
    fn into_addr(self) -> A {
        self.0
    }
}

impl<A> UnicastAddr<A> {
    /// Constructs a new `UnicastAddr` without checking to see if `addr` is
    /// actually a unicast address.
    ///
    /// # Safety
    ///
    /// It is up to the caller to make sure that `addr` is a unicast address to
    /// avoid breaking the guarantees of `UnicastAddr`. See [`UnicastAddr`] for
    /// more details.
    #[inline]
    pub const unsafe fn new_unchecked(addr: A) -> UnicastAddr<A> {
        UnicastAddr(addr)
    }
}

impl<A: UnicastAddress + SpecifiedAddress> UnicastAddr<A> {
    /// Converts this `UnicastAddr` into a [`SpecifiedAddr`].
    ///
    /// [`UnicastAddress::is_unicast`] implies
    /// [`SpecifiedAddress::is_specified`], so all `UnicastAddr`s are guaranteed
    /// to be specified, so this conversion is infallible.
    #[inline]
    pub fn into_specified(self) -> SpecifiedAddr<A> {
        SpecifiedAddr(self.0)
    }
}

impl<A: UnicastAddress> Deref for UnicastAddr<A> {
    type Target = A;

    #[inline]
    fn deref(&self) -> &A {
        &self.0
    }
}

impl<A: UnicastAddress + Display> Display for UnicastAddr<A> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl<A: UnicastAddress + SpecifiedAddress> From<UnicastAddr<A>> for SpecifiedAddr<A> {
    fn from(addr: UnicastAddr<A>) -> SpecifiedAddr<A> {
        addr.into_specified()
    }
}

/// An address which is guaranteed to be a multicast address.
///
/// `MulticastAddr` wraps an address of type `A` and guarantees that it is a
/// multicast address. Note that this guarantee is contingent on a correct
/// implementation of the [`MulticastAddress`] trait. Since that trait is not
/// `unsafe`, `unsafe` code may NOT rely on this guarantee for its soundness.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct MulticastAddr<A>(A);

impl<A: MulticastAddress> sealed::Sealed for MulticastAddr<A> {}
impl<A: MulticastAddress> Witness<A> for MulticastAddr<A> {
    #[inline]
    fn new(addr: A) -> Option<MulticastAddr<A>> {
        if !addr.is_multicast() {
            return None;
        }
        Some(MulticastAddr(addr))
    }

    #[inline]
    fn into_addr(self) -> A {
        self.0
    }
}

impl<A> MulticastAddr<A> {
    /// Construct a new `MulticastAddr` without checking to see if `addr` is
    /// actually a multicast address.
    ///
    /// # Safety
    ///
    /// It is up to the caller to make sure that `addr` is a multicast address
    /// to avoid breaking the guarantees of `MulticastAddr`. See
    /// [`MulticastAddr`] for more details.
    #[inline]
    pub const unsafe fn new_unchecked(addr: A) -> MulticastAddr<A> {
        MulticastAddr(addr)
    }
}

impl<A: MulticastAddress + SpecifiedAddress> MulticastAddr<A> {
    /// Converts this `MulticastAddr` into a [`SpecifiedAddr`].
    ///
    /// [`MulticastAddress::is_multicast`] implies
    /// [`SpecifiedAddress::is_specified`], so all `MulticastAddr`s are
    /// guaranteed to be specified, so this conversion is infallible.
    #[inline]
    pub fn into_specified(self) -> SpecifiedAddr<A> {
        SpecifiedAddr(self.0)
    }
}

impl<A: MulticastAddress> Deref for MulticastAddr<A> {
    type Target = A;

    #[inline]
    fn deref(&self) -> &A {
        &self.0
    }
}

impl<A: MulticastAddress + Display> Display for MulticastAddr<A> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl<A: MulticastAddress + SpecifiedAddress> From<MulticastAddr<A>> for SpecifiedAddr<A> {
    fn from(addr: MulticastAddr<A>) -> SpecifiedAddr<A> {
        addr.into_specified()
    }
}

/// An address which is guaranteed to be a link-local address.
///
/// `LinkLocalAddr` wraps an address of type `A` and guarantees that it is a
/// link-local address. Note that this guarantee is contingent on a correct
/// implementation of the [`LinkLocalAddress`] trait. Since that trait is not
/// `unsafe`, `unsafe` code may NOT rely on this guarantee for its soundness.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct LinkLocalAddr<A>(A);

impl<A: LinkLocalAddress> sealed::Sealed for LinkLocalAddr<A> {}
impl<A: LinkLocalAddress> Witness<A> for LinkLocalAddr<A> {
    #[inline]
    fn new(addr: A) -> Option<LinkLocalAddr<A>> {
        if !addr.is_linklocal() {
            return None;
        }
        Some(LinkLocalAddr(addr))
    }

    #[inline]
    fn into_addr(self) -> A {
        self.0
    }
}

impl<A> LinkLocalAddr<A> {
    /// Construct a new `LinkLocalAddr` without checking to see if `addr` is
    /// actually a link-local address.
    ///
    /// # Safety
    ///
    /// It is up to the caller to make sure that `addr` is a link-local address
    /// to avoid breaking the guarantees of `LinkLocalAddr`. See
    /// [`LinkLocalAddr`] for more details.
    #[inline]
    pub const unsafe fn new_unchecked(addr: A) -> LinkLocalAddr<A> {
        LinkLocalAddr(addr)
    }
}

impl<A: LinkLocalAddress + SpecifiedAddress> LinkLocalAddr<A> {
    /// Converts this `LinkLocalAddr` into a [`SpecifiedAddr`].
    ///
    /// [`LinkLocalAddress::is_linklocal`] implies
    /// [`SpecifiedAddress::is_specified`], so all `LinkLocalAddr`s are
    /// guaranteed to be specified, so this conversion is infallible.
    #[inline]
    pub fn into_specified(self) -> SpecifiedAddr<A> {
        SpecifiedAddr(self.0)
    }
}

impl<A: LinkLocalAddress> Deref for LinkLocalAddr<A> {
    type Target = A;

    #[inline]
    fn deref(&self) -> &A {
        &self.0
    }
}

impl<A: LinkLocalAddress + Display> Display for LinkLocalAddr<A> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl<A: LinkLocalAddress + SpecifiedAddress> From<LinkLocalAddr<A>> for SpecifiedAddr<A> {
    fn from(addr: LinkLocalAddr<A>) -> SpecifiedAddr<A> {
        addr.into_specified()
    }
}

/// A witness type for an address and and a scope zone.
///
/// `AddrAndZone` carries an address that *may* have a scope, alongside the
/// particular zone of that scope. The zone is also referred to as a "scope
/// identifier" in some systems (such as Linux).
///
/// Note that although `AddrAndZone` acts as a witness type, it does not
/// implement [`Witness`] since it carries both the address and scoping
/// information, and not only the witnessed address.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct AddrAndZone<A, Z>(A, Z);

impl<A: ScopeableAddress, Z> AddrAndZone<A, Z> {
    /// Creates a new `AddrAndZone`, returning `Some` only if the provided
    /// `addr` is scopeable.
    pub fn new(addr: A, zone: Z) -> Option<Self> {
        if !addr.scope().is_global() {
            Some(Self(addr, zone))
        } else {
            None
        }
    }

    /// Turns this `AddrAndZone` into its forming parts.
    pub fn into_addr_scope_id(self) -> (A, Z) {
        (self.0, self.1)
    }
}

impl<A, Z> AddrAndZone<A, Z> {
    /// Construct a new `AddrAndZone` without checking to see if `addr` is
    /// actually a scopeable address.
    ///
    /// # Safety
    ///
    /// It is up to the caller to make sure that `addr` is a scopeable address
    /// to avoid breaking the guarantees of `AddrAndZone`.
    #[inline]
    pub const unsafe fn new_unchecked(addr: A, zone: Z) -> Self {
        Self(addr, zone)
    }
}

impl<A: ScopeableAddress + SpecifiedAddress, Z> AddrAndZone<A, Z> {
    /// Turns this `AddrAndZone` into its forming parts, providing a safe
    /// `SpecifiedAddr`.
    pub fn into_specified_addr_zone(self) -> (SpecifiedAddr<A>, Z) {
        (SpecifiedAddr(self.0), self.1)
    }
}

impl<A: ScopeableAddress + Display, Z: Display> Display for AddrAndZone<A, Z> {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        write!(f, "{}%{}", self.0, self.1)
    }
}

impl<A: ScopeableAddress, Z> sealed::Sealed for AddrAndZone<A, Z> {}

/// An address that may have an associated scope zone.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub enum ScopedAddress<A, Z> {
    Global(SpecifiedAddr<A>),
    Scoped(AddrAndZone<A, Z>),
}

impl<A: ScopeableAddress + SpecifiedAddress, Z> ScopedAddress<A, Z> {
    /// Creates a new `ScopedAddress` with the provided optional scope zone.
    ///
    /// If `zone` is `None`, [`ScopedAddress::Global`] is returned. Otherwise, a
    /// [`ScopedAddress::Scoped`] is returned only if the provided `addr` is
    /// scopeable.
    pub fn new(addr: A, zone: Option<Z>) -> Option<Self> {
        match zone {
            Some(zone) => AddrAndZone::new(addr, zone).map(ScopedAddress::Scoped),
            None => SpecifiedAddr::new(addr).map(ScopedAddress::Global),
        }
    }

    /// Decomposes this `ScopedAddress` into a `SpecifiedAddr` and an optional
    /// scope zone.
    pub fn into_addr_zone(self) -> (SpecifiedAddr<A>, Option<Z>) {
        match self {
            ScopedAddress::Global(addr) => (addr, None),
            ScopedAddress::Scoped(scope_and_zone) => {
                let (addr, zone) = scope_and_zone.into_specified_addr_zone();
                (addr, Some(zone))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    enum Address {
        Unspecified,
        Unicast,
        Multicast,
        LinkLocal,
    }

    impl SpecifiedAddress for Address {
        fn is_specified(&self) -> bool {
            *self != Address::Unspecified
        }
    }

    impl UnicastAddress for Address {
        fn is_unicast(&self) -> bool {
            *self == Address::Unicast
        }
    }

    impl MulticastAddress for Address {
        fn is_multicast(&self) -> bool {
            *self == Address::Multicast
        }
    }

    impl LinkLocalAddress for Address {
        fn is_linklocal(&self) -> bool {
            *self == Address::LinkLocal
        }
    }

    impl ScopeableAddress for Address {
        type NonGlobalScope = ();

        fn scope(&self) -> Scope<()> {
            if self.is_linklocal() {
                Scope::NonGlobal(())
            } else {
                Scope::Global
            }
        }
    }

    #[test]
    fn test_specified_addr() {
        assert_eq!(SpecifiedAddr::new(Address::Unicast), Some(SpecifiedAddr(Address::Unicast)));
        assert_eq!(SpecifiedAddr::new(Address::Unspecified), None);
    }

    #[test]
    fn test_unicast_addr() {
        assert_eq!(UnicastAddr::new(Address::Unicast), Some(UnicastAddr(Address::Unicast)));
        assert_eq!(UnicastAddr::new(Address::Multicast), None);
        assert_eq!(
            unsafe { UnicastAddr::new_unchecked(Address::Unicast) },
            UnicastAddr(Address::Unicast)
        );
    }

    #[test]
    fn test_multicast_addr() {
        assert_eq!(MulticastAddr::new(Address::Multicast), Some(MulticastAddr(Address::Multicast)));
        assert_eq!(MulticastAddr::new(Address::Unicast), None);
        assert_eq!(
            unsafe { MulticastAddr::new_unchecked(Address::Multicast) },
            MulticastAddr(Address::Multicast)
        );
    }

    #[test]
    fn test_linklocal_addr() {
        assert_eq!(LinkLocalAddr::new(Address::LinkLocal), Some(LinkLocalAddr(Address::LinkLocal)));
        assert_eq!(LinkLocalAddr::new(Address::Multicast), None);
        assert_eq!(
            unsafe { LinkLocalAddr::new_unchecked(Address::LinkLocal) },
            LinkLocalAddr(Address::LinkLocal)
        );
    }

    #[test]
    fn test_addr_and_zone() {
        let addr_and_zone = AddrAndZone::new(Address::LinkLocal, ());
        assert_eq!(addr_and_zone, Some(AddrAndZone(Address::LinkLocal, ())));
        assert_eq!(addr_and_zone.unwrap().into_addr_scope_id(), (Address::LinkLocal, ()));
        assert_eq!(AddrAndZone::new(Address::Unicast, ()), None);
        assert_eq!(
            unsafe { AddrAndZone::new_unchecked(Address::LinkLocal, ()) },
            AddrAndZone(Address::LinkLocal, ())
        );
    }

    #[test]
    fn test_scoped_address() {
        // Type alias to help the compiler when the scope type can't be
        // inferred.
        type ScopedAddress = crate::ScopedAddress<Address, ()>;
        assert_eq!(
            ScopedAddress::new(Address::Unicast, None),
            Some(ScopedAddress::Global(SpecifiedAddr(Address::Unicast)))
        );
        assert_eq!(ScopedAddress::new(Address::Unspecified, None), None);
        assert_eq!(
            ScopedAddress::new(Address::LinkLocal, None),
            Some(ScopedAddress::Global(SpecifiedAddr(Address::LinkLocal)))
        );
        assert_eq!(ScopedAddress::new(Address::Unicast, Some(())), None);
        assert_eq!(ScopedAddress::new(Address::Unspecified, Some(())), None);
        assert_eq!(
            ScopedAddress::new(Address::LinkLocal, Some(())),
            Some(ScopedAddress::Scoped(AddrAndZone(Address::LinkLocal, ())))
        );

        assert_eq!(
            ScopedAddress::new(Address::Unicast, None).unwrap().into_addr_zone(),
            (SpecifiedAddr(Address::Unicast), None)
        );
        assert_eq!(
            ScopedAddress::new(Address::LinkLocal, Some(())).unwrap().into_addr_zone(),
            (SpecifiedAddr(Address::LinkLocal), Some(()))
        );
    }
}
