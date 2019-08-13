// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Internet Group Management Protocol (IGMP).
//!
//! IGMP is a communications protocol used by hosts and adjacent routers on IPv4
//! networks to establish multicast group memberships.

use std::collections::HashMap;
use std::fmt::{Debug, Display};
use std::time::Duration;

use failure::Fail;
use log::{debug, error};
use net_types::ip::{AddrSubnet, IpAddress, Ipv4Addr};
use net_types::MulticastAddr;
use packet::{BufferMut, EmptyBuf, InnerPacketBuilder};
use rand::Rng;
use rand_xorshift::XorShiftRng;
use zerocopy::ByteSlice;

use crate::context::{
    FrameContext, InstantContext, RngContext, RngContextExt, StateContext, TimerContext,
};
use crate::ip::gmp::{Action, Actions, GmpAction, GmpStateMachine, ProtocolSpecific};
use crate::ip::IpDeviceIdContext;
use crate::wire::igmp::{
    messages::{IgmpLeaveGroup, IgmpMembershipReportV1, IgmpMembershipReportV2, IgmpPacket},
    IgmpMessage, IgmpPacketBuilder, MessageType,
};
use crate::Instant;

/// Metadata for sending an IGMP packet.
///
/// `IgmpPacketMetadata` is used by [`IgmpContext`]'s [`FrameContext`] bound.
/// When [`FrameContext::send_frame`] is called with an `IgmpPacketMetadata`,
/// the body will be encapsulated in an IP packet with a TTL of 1 and with the
/// "Router Alert" option set.
pub(crate) struct IgmpPacketMetadata<D> {
    pub(crate) device: D,
    pub(crate) src_ip: Ipv4Addr,
    pub(crate) dst_ip: Ipv4Addr,
}

impl<D> IgmpPacketMetadata<D> {
    fn new(device: D, src_ip: Ipv4Addr, dst_ip: Ipv4Addr) -> IgmpPacketMetadata<D> {
        IgmpPacketMetadata { device, src_ip, dst_ip }
    }
}

/// The execution context for the Internet Group Management Protocol (IGMP).
pub(crate) trait IgmpContext:
    IpDeviceIdContext
    + TimerContext<IgmpTimerId<<Self as IpDeviceIdContext>::DeviceId>>
    + RngContext
    + StateContext<
        <Self as IpDeviceIdContext>::DeviceId,
        IgmpInterface<<Self as InstantContext>::Instant>,
    > + FrameContext<EmptyBuf, IgmpPacketMetadata<<Self as IpDeviceIdContext>::DeviceId>>
{
    /// Gets an IP address and subnet associated with this device.
    fn get_ip_addr_subnet(&self, device: Self::DeviceId) -> Option<AddrSubnet<Ipv4Addr>>;
}

/// Receive an IGMP message in an IP packet.
pub(crate) fn receive_igmp_packet<C: IgmpContext, B: BufferMut>(
    ctx: &mut C,
    device: C::DeviceId,
    src_ip: Ipv4Addr,
    dst_ip: Ipv4Addr,
    mut buffer: B,
) {
    let packet = match buffer.parse_with::<_, IgmpPacket<&[u8]>>(()) {
        Ok(packet) => packet,
        Err(err) => {
            debug!("Cannot parse the incoming IGMP packet, dropping.");
            return;
        } // TODO: Do something else here?
    };

    if let Err(e) = match packet {
        IgmpPacket::MembershipQueryV2(msg) => {
            let now = ctx.now();
            handle_igmp_message(ctx, device, msg, |rng, state, msg| {
                state.query_received(rng, msg.max_response_time().into(), now)
            })
        }
        IgmpPacket::MembershipReportV1(msg) => {
            handle_igmp_message(ctx, device, msg, |_, state, _| state.report_received())
        }
        IgmpPacket::MembershipReportV2(msg) => {
            handle_igmp_message(ctx, device, msg, |_, state, _| state.report_received())
        }
        IgmpPacket::LeaveGroup(_) => {
            debug!("Hosts are not interested in Leave Group messages");
            return;
        }
        _ => {
            debug!("We do not support IGMPv3 yet");
            return;
        }
    } {
        error!("Error occurred when handling IGMPv2 message: {}", e);
    }
}

fn handle_igmp_message<C: IgmpContext, B: ByteSlice, M, F>(
    ctx: &mut C,
    device: C::DeviceId,
    msg: IgmpMessage<B, M>,
    handler: F,
) -> IgmpResult<(), C::DeviceId>
where
    M: MessageType<B, FixedHeader = Ipv4Addr>,
    F: Fn(
        &mut XorShiftRng,
        &mut IgmpGroupState<C::Instant>,
        &IgmpMessage<B, M>,
    ) -> Actions<Igmpv2ProtocolSpecific>,
{
    // TODO(joshlf): Once we figure out how to access the RNG and the state at
    // the same time, get rid of this hack. For the time being, this is probably
    // fine because, while the `XorShiftRng` isn't cryptographically secure, its
    // seed is, which means that, at worst, an attacker will be able to
    // correlate events generated during this one function call.
    let mut rng = ctx.new_xorshift_rng();
    let group_addr = msg.group_addr();
    if group_addr.is_unspecified() {
        let mut addr_and_actions = ctx
            .get_state_mut(device)
            .groups
            .iter_mut()
            .map(|(addr, state)| (addr.clone(), handler(&mut rng, state, &msg)))
            .collect::<Vec<_>>();
        // `addr` must be a multicast address, otherwise it will not have
        // an associated state in the first place
        for (addr, actions) in addr_and_actions {
            run_actions(ctx, device, actions, addr);
        }
        Ok(())
    } else if let Some(group_addr) = MulticastAddr::new(group_addr) {
        let actions = match ctx.get_state_mut(device).groups.get_mut(&group_addr) {
            Some(state) => handler(&mut rng, state, &msg),
            None => return Err(IgmpError::NotAMember { addr: *group_addr }),
        };
        // `group_addr` here must be a multicast address for similar reasons
        run_actions(ctx, device, actions, group_addr);
        Ok(())
    } else {
        Err(IgmpError::NotAMember { addr: group_addr })
    }
}

#[derive(Debug, Fail)]
pub(crate) enum IgmpError<D: Display + Debug + Send + Sync + 'static> {
    /// The host is trying to operate on an group address of which the host is not a member.
    #[fail(display = "the host has not already been a member of the address: {}", addr)]
    NotAMember { addr: Ipv4Addr },
    /// Failed to send an IGMP packet.
    #[fail(display = "failed to send out an IGMP packet to address: {}", addr)]
    SendFailure { addr: Ipv4Addr },
    /// The given device does not have an assigned IP address.
    #[fail(display = "no ip address is associated with the device: {}", device)]
    NoIpAddress { device: D },
}

pub(crate) type IgmpResult<T, D> = Result<T, IgmpError<D>>;

#[derive(Debug, PartialEq, Eq, Clone, Copy, Hash)]
pub(crate) enum IgmpTimerId<D> {
    /// The timer used to switch a host from Delay Member state to Idle Member
    /// state.
    ReportDelay { device: D, group_addr: MulticastAddr<Ipv4Addr> },
    /// The timer used to determine whether there is a router speaking IGMPv1.
    V1RouterPresent { device: D },
}

impl<D> IgmpTimerId<D> {
    fn new_report_delay(device: D, group_addr: MulticastAddr<Ipv4Addr>) -> IgmpTimerId<D> {
        IgmpTimerId::ReportDelay { device, group_addr }
    }

    fn new_v1_router_present(device: D) -> IgmpTimerId<D> {
        IgmpTimerId::V1RouterPresent { device }
    }
}

pub(crate) fn handle_timeout<C: IgmpContext>(ctx: &mut C, timer: IgmpTimerId<C::DeviceId>) {
    match timer {
        IgmpTimerId::ReportDelay { device, group_addr } => {
            let actions = match ctx.get_state_mut(device).groups.get_mut(&group_addr) {
                Some(state) => state.report_timer_expired(),
                None => {
                    error!("Not already a member");
                    return;
                }
            };
            run_actions(ctx, device, actions, group_addr);
        }
        IgmpTimerId::V1RouterPresent { device } => {
            for (_, state) in ctx.get_state_mut(device).groups.iter_mut() {
                state.v1_router_present_timer_expired();
            }
        }
    }
}

fn send_igmp_message<C: IgmpContext, M>(
    ctx: &mut C,
    device: C::DeviceId,
    group_addr: MulticastAddr<Ipv4Addr>,
    dst_ip: MulticastAddr<Ipv4Addr>,
    max_resp_time: M::MaxRespTime,
) -> IgmpResult<(), C::DeviceId>
where
    M: MessageType<EmptyBuf, FixedHeader = Ipv4Addr, VariableBody = ()>,
{
    let src_ip = match ctx.get_ip_addr_subnet(device) {
        Some(addr_subnet) => addr_subnet.addr(),
        None => return Err(IgmpError::NoIpAddress { device }),
    };
    let body =
        IgmpPacketBuilder::<EmptyBuf, M>::new_with_resp_time(group_addr.get(), max_resp_time);
    ctx.send_frame(IgmpPacketMetadata::new(device, src_ip, dst_ip.get()), body.into_serializer())
        .map_err(|_| IgmpError::SendFailure { addr: *group_addr })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Igmpv2ProtocolSpecific {
    v1_router_present: bool,
}

impl Default for Igmpv2ProtocolSpecific {
    fn default() -> Self {
        Igmpv2ProtocolSpecific { v1_router_present: false }
    }
}

#[derive(PartialEq, Eq, Debug)]
enum Igmpv2Actions {
    ScheduleV1RouterPresentTimer(Duration),
}

struct Igmpv2HostConfig {
    // When a host wants to send a report not because
    // of a query, this value is used as the delay timer.
    unsolicited_report_interval: Duration,
    // When this option is true, the host can send a leave
    // message even when it is not the last one in the multicast
    // group.
    send_leave_anyway: bool,
    // Default timer value for Version 1 Router Present Timeout.
    v1_router_present_timeout: Duration,
}

/// The default value for `unsolicited_report_interval` as per [RFC 2236 section 8.10].
///
/// [RFC 2236 section 8.10]: https://tools.ietf.org/html/rfc2236#section-8.10
const DEFAULT_UNSOLICITED_REPORT_INTERVAL: Duration = Duration::from_secs(10);
/// The default value for `v1_router_present_timeout` as per [RFC 2236 section 8.11].
///
/// [RFC 2236 section 8.11]: https://tools.ietf.org/html/rfc2236#section-8.11
const DEFAULT_V1_ROUTER_PRESENT_TIMEOUT: Duration = Duration::from_secs(400);
/// The default value for the `MaxRespTime` if the query is a V1 query, whose
/// `MaxRespTime` field is 0 in the packet. Please refer to [RFC 2236 section 4]
///
/// [RFC 2236 section 4]: https://tools.ietf.org/html/rfc2236#section-4
const DEFAULT_V1_QUERY_MAX_RESP_TIME: Duration = Duration::from_secs(10);

impl Default for Igmpv2HostConfig {
    fn default() -> Self {
        Igmpv2HostConfig {
            unsolicited_report_interval: DEFAULT_UNSOLICITED_REPORT_INTERVAL,
            send_leave_anyway: false,
            v1_router_present_timeout: DEFAULT_V1_ROUTER_PRESENT_TIMEOUT,
        }
    }
}

impl ProtocolSpecific for Igmpv2ProtocolSpecific {
    type Action = Igmpv2Actions;
    type Config = Igmpv2HostConfig;

    fn cfg_unsolicited_report_interval(cfg: &Self::Config) -> Duration {
        cfg.unsolicited_report_interval
    }

    fn cfg_send_leave_anyway(cfg: &Self::Config) -> bool {
        cfg.send_leave_anyway
    }

    fn get_max_resp_time(resp_time: Duration) -> Duration {
        if resp_time.as_micros() == 0 {
            DEFAULT_V1_QUERY_MAX_RESP_TIME
        } else {
            resp_time
        }
    }

    fn do_query_received_specific(
        cfg: &Self::Config,
        actions: &mut Actions<Self>,
        max_resp_time: Duration,
        old: Igmpv2ProtocolSpecific,
    ) -> Igmpv2ProtocolSpecific {
        // IGMPv2 hosts should be compatible with routers that only speak IGMPv1.
        // When an IGMPv2 host receives an IGMPv1 query (whose `MaxRespCode` is 0),
        // it should set up a timer and only respond with IGMPv1 responses before
        // the timer expires. Please refer to https://tools.ietf.org/html/rfc2236#section-4
        // for details.
        let new_ps = Igmpv2ProtocolSpecific { v1_router_present: max_resp_time.as_micros() == 0 };
        if new_ps.v1_router_present {
            actions.push_specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(
                cfg.v1_router_present_timeout,
            ));
        }
        new_ps
    }
}

type IgmpGroupState<I> = GmpStateMachine<I, Igmpv2ProtocolSpecific>;

impl<I: Instant> IgmpGroupState<I> {
    fn v1_router_present_timer_expired(&mut self) {
        self.update_with_protocol_specific(Igmpv2ProtocolSpecific { v1_router_present: false });
    }
}

/// This is used to represent the groups that an IGMP host is interested in.
pub(crate) struct IgmpInterface<I: Instant> {
    groups: HashMap<MulticastAddr<Ipv4Addr>, IgmpGroupState<I>>,
}

impl<I: Instant> Default for IgmpInterface<I> {
    fn default() -> Self {
        IgmpInterface { groups: HashMap::new() }
    }
}

fn run_actions<C: IgmpContext>(
    ctx: &mut C,
    device: C::DeviceId,
    actions: Actions<Igmpv2ProtocolSpecific>,
    group_addr: MulticastAddr<Ipv4Addr>,
) {
    for action in actions {
        if let Err(err) = run_action(ctx, device, action, group_addr) {
            error!("Error performing action on {} on device {}: {}", group_addr, device, err);
        }
    }
}

/// Interpret the actions
fn run_action<C: IgmpContext>(
    ctx: &mut C,
    device: C::DeviceId,
    action: Action<Igmpv2ProtocolSpecific>,
    group_addr: MulticastAddr<Ipv4Addr>,
) -> IgmpResult<(), C::DeviceId> {
    match action {
        Action::Generic(GmpAction::ScheduleReportTimer(duration)) => {
            ctx.schedule_timer(duration, IgmpTimerId::new_report_delay(device, group_addr));
        }
        Action::Generic(GmpAction::StopReportTimer) => {
            ctx.cancel_timer(IgmpTimerId::new_report_delay(device, group_addr));
        }
        Action::Generic(GmpAction::SendLeave) => {
            send_igmp_message::<_, IgmpLeaveGroup>(
                ctx,
                device,
                group_addr,
                MulticastAddr::new(crate::ip::IPV4_ALL_ROUTERS).unwrap(),
                (),
            )?;
        }
        Action::Generic(GmpAction::SendReport(Igmpv2ProtocolSpecific { v1_router_present })) => {
            if v1_router_present {
                send_igmp_message::<_, IgmpMembershipReportV1>(
                    ctx,
                    device,
                    group_addr,
                    group_addr,
                    (),
                )?;
            } else {
                send_igmp_message::<_, IgmpMembershipReportV2>(
                    ctx,
                    device,
                    group_addr,
                    group_addr,
                    (),
                )?;
            }
        }
        Action::Specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(duration)) => {
            ctx.schedule_timer(duration, IgmpTimerId::new_v1_router_present(device));
        }
    }
    Ok(())
}

impl<I: Instant> IgmpInterface<I> {
    fn join_group<R: Rng>(
        &mut self,
        rng: &mut R,
        addr: MulticastAddr<Ipv4Addr>,
        now: I,
    ) -> Actions<Igmpv2ProtocolSpecific> {
        self.groups.entry(addr).or_insert(GmpStateMachine::default()).join_group(rng, now)
    }

    fn leave_group<D: Display + Debug + Send + Sync>(
        &mut self,
        addr: MulticastAddr<Ipv4Addr>,
    ) -> IgmpResult<Actions<Igmpv2ProtocolSpecific>, D> {
        match self.groups.remove(&addr).as_mut() {
            Some(state) => Ok(state.leave_group()),
            None => Err(IgmpError::NotAMember { addr: addr.get() }),
        }
    }
}

/// Make our host join a multicast group.
pub(crate) fn igmp_join_group<C: IgmpContext>(
    ctx: &mut C,
    device: C::DeviceId,
    group_addr: MulticastAddr<Ipv4Addr>,
) {
    let now = ctx.now();
    // TODO(joshlf): Once we figure out how to access the RNG and the state at
    // the same time, get rid of this hack. For the time being, this is probably
    // fine because, while the `XorShiftRng` isn't cryptographically secure, its
    // seed is, which means that, at worst, an attacker will be able to
    // correlate events generated during this one function call.
    let mut rng = ctx.new_xorshift_rng();
    let actions = ctx.get_state_mut(device).join_group(&mut rng, group_addr, now);
    // actions will be `Nothing` if the the host is not in the `NonMember` state.
    run_actions(ctx, device, actions, group_addr);
}

/// Make our host leave the multicast group.
///
/// If our host is not already a member of the given address, this will result
/// in the `IgmpError::NotAMember` error.
pub(crate) fn igmp_leave_group<C: IgmpContext>(
    ctx: &mut C,
    device: C::DeviceId,
    group_addr: MulticastAddr<Ipv4Addr>,
) -> IgmpResult<(), C::DeviceId> {
    let actions = ctx.get_state_mut(device).leave_group(group_addr)?;
    run_actions(ctx, device, actions, group_addr);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::convert::TryInto;
    use std::time;

    use net_types::ethernet::Mac;
    use net_types::ip::AddrSubnet;
    use packet::serialize::{Buf, Serializer};

    use crate::device::ethernet::*;
    use crate::device::DeviceId;
    use crate::ip::gmp::{Action, GmpAction, MemberState};
    use crate::testutil::{self, *};
    use crate::wire::igmp::messages::IgmpMembershipQueryV2;
    use crate::{Context, EventDispatcher, StackStateBuilder};

    fn at_least_one_action(
        actions: Actions<Igmpv2ProtocolSpecific>,
        action: Action<Igmpv2ProtocolSpecific>,
    ) -> bool {
        actions.into_iter().any(|a| a == action)
    }

    #[test]
    fn test_igmp_state_with_igmpv1_router() {
        let mut rng = new_rng(0);
        let mut s = IgmpGroupState::default();
        s.join_group(&mut rng, time::Instant::now());
        s.query_received(&mut rng, Duration::from_secs(0), time::Instant::now());
        let actions = s.report_timer_expired();
        at_least_one_action(
            actions,
            Action::<Igmpv2ProtocolSpecific>::Generic(GmpAction::SendReport(
                Igmpv2ProtocolSpecific { v1_router_present: true },
            )),
        );
    }

    #[test]
    fn test_igmp_state_igmpv1_router_present_timer_expires() {
        let mut s = IgmpGroupState::default();
        let mut rng = new_rng(0);
        s.join_group(&mut rng, time::Instant::now());
        s.query_received(&mut rng, Duration::from_secs(0), time::Instant::now());
        match s.get_inner() {
            MemberState::Delaying(state) => {
                assert!(state.get_protocol_specific().v1_router_present);
            }
            _ => panic!("Wrong State!"),
        }
        s.v1_router_present_timer_expired();
        match s.get_inner() {
            MemberState::Delaying(state) => {
                assert!(!state.get_protocol_specific().v1_router_present);
            }
            _ => panic!("Wrong State!"),
        }
        s.query_received(&mut rng, Duration::from_secs(0), time::Instant::now());
        s.report_received();
        s.v1_router_present_timer_expired();
        match s.get_inner() {
            MemberState::Idle(state) => {
                assert!(!state.get_protocol_specific().v1_router_present);
            }
            _ => panic!("Wrong State!"),
        }
    }

    const MY_ADDR: Ipv4Addr = Ipv4Addr::new([192, 168, 0, 2]);
    const ROUTER_ADDR: Ipv4Addr = Ipv4Addr::new([192, 168, 0, 1]);
    const OTHER_HOST_ADDR: Ipv4Addr = Ipv4Addr::new([192, 168, 0, 3]);
    const GROUP_ADDR: Ipv4Addr = Ipv4Addr::new([224, 0, 0, 3]);
    const GROUP_ADDR_2: Ipv4Addr = Ipv4Addr::new([224, 0, 0, 4]);

    fn receive_igmp_query<C: IgmpContext>(ctx: &mut C, device: C::DeviceId, resp_time: Duration) {
        let ser = IgmpPacketBuilder::<Buf<Vec<u8>>, IgmpMembershipQueryV2>::new_with_resp_time(
            GROUP_ADDR,
            resp_time.try_into().unwrap(),
        );
        let mut buff = ser.into_serializer().serialize_vec_outer().unwrap();
        receive_igmp_packet(ctx, device, ROUTER_ADDR, MY_ADDR, buff);
    }

    fn receive_igmp_general_query<C: IgmpContext>(
        ctx: &mut C,
        device: C::DeviceId,
        resp_time: Duration,
    ) {
        let ser = IgmpPacketBuilder::<Buf<Vec<u8>>, IgmpMembershipQueryV2>::new_with_resp_time(
            Ipv4Addr::new([0, 0, 0, 0]),
            resp_time.try_into().unwrap(),
        );
        let mut buff = ser.into_serializer().serialize_vec_outer().unwrap();
        receive_igmp_packet(ctx, device, ROUTER_ADDR, MY_ADDR, buff);
    }

    fn receive_igmp_report<D: EventDispatcher>(ctx: &mut Context<D>, device: DeviceId) {
        let ser = IgmpPacketBuilder::<Buf<Vec<u8>>, IgmpMembershipReportV2>::new(GROUP_ADDR);
        let mut buff = ser.into_serializer().serialize_vec_outer().unwrap();
        receive_igmp_packet(ctx, device, OTHER_HOST_ADDR, MY_ADDR, buff);
    }

    fn setup_simple_test_environment() -> (Context<DummyEventDispatcher>, DeviceId) {
        let mut stack_builder = StackStateBuilder::default();

        // Most tests do not need NDP's DAD or router solicitation so disable it here.
        let mut ndp_configs = crate::device::ndp::NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);

        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let dev_id = ctx.state.add_ethernet_device(Mac::new([1, 2, 3, 4, 5, 6]), 1500);
        crate::device::initialize_device(&mut ctx, dev_id);
        set_ip_addr_subnet(&mut ctx, dev_id.id(), AddrSubnet::new(MY_ADDR, 24).unwrap());
        (ctx, dev_id)
    }

    fn ensure_ttl_ihl_rtr(ctx: &mut Context<DummyEventDispatcher>) {
        for (_, frame) in ctx.dispatcher.frames_sent() {
            assert_eq!(frame[22], 1); // TTL,
            assert_eq!(&frame[34..38], &[148, 4, 0, 0]); // RTR
            assert_eq!(frame[14], 0x46); // IHL
        }
    }

    #[test]
    fn test_igmp_simple_integration() {
        let (mut ctx, dev_id) = setup_simple_test_environment();
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());

        receive_igmp_query(&mut ctx, dev_id, Duration::from_secs(10));
        assert!(testutil::trigger_next_timer(&mut ctx));

        // we should get two Igmpv2 reports, one for the unsolicited one for the host
        // to turn into Delay Member state and the other one for the timer being fired.
        assert_eq!(ctx.dispatcher.frames_sent().len(), 2);
    }

    #[test]
    fn test_igmp_integration_fallback_from_idle() {
        let (mut ctx, dev_id) = setup_simple_test_environment();
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);

        assert!(testutil::trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher.frames_sent().len(), 2);

        receive_igmp_query(&mut ctx, dev_id, Duration::from_secs(10));

        // We have received a query, hence we are falling back to Delay Member state.
        let group_state = ctx
            .state
            .ip
            .get_igmp_state_mut(dev_id.id())
            .groups
            .get(&MulticastAddr::new(GROUP_ADDR).unwrap())
            .unwrap();
        match group_state.get_inner() {
            MemberState::Delaying(_) => {}
            _ => panic!("Wrong State!"),
        }

        assert!(testutil::trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher.frames_sent().len(), 3);

        let mac: [u8; 6] = [0x01, 0x00, 0x5e, 0, 0, 3];
        for (_, frame) in ctx.dispatcher.frames_sent() {
            assert_eq!(frame[0..6], mac[..]);
        }
        ensure_ttl_ihl_rtr(&mut ctx);
    }

    #[test]
    fn test_igmp_integration_igmpv1_router_present() {
        let (mut ctx, dev_id) = setup_simple_test_environment();

        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());
        assert_eq!(ctx.dispatcher.timer_events().count(), 1);
        let instant1 = ctx.dispatcher.timer_events().nth(0).unwrap().0.clone();

        receive_igmp_query(&mut ctx, dev_id, Duration::from_secs(0));
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);

        // Since we have heard from the v1 router, we should have set our flag
        let group_state = ctx
            .state
            .ip
            .get_igmp_state_mut(dev_id.id())
            .groups
            .get(&MulticastAddr::new(GROUP_ADDR).unwrap())
            .unwrap();
        match group_state.get_inner() {
            MemberState::Delaying(state) => {
                assert!(state.get_protocol_specific().v1_router_present)
            }
            _ => panic!("Wrong State!"),
        }

        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
        // Two timers: one for the delayed report, one for the v1 router timer
        assert_eq!(ctx.dispatcher.timer_events().count(), 2);
        let instant2 = ctx.dispatcher.timer_events().nth(0).unwrap().0.clone();
        assert_eq!(instant1, instant2);

        assert!(testutil::trigger_next_timer(&mut ctx));
        // After the first timer, we send out our V1 report.
        assert_eq!(ctx.dispatcher.frames_sent().len(), 2);
        // the last frame being sent should be a V1 report.
        let (_, frame) = ctx.dispatcher.frames_sent().last().unwrap();
        // 34 and 0x12 are hacky but they can quickly tell it is a V1 report.
        assert_eq!(frame[38], 0x12);

        assert!(testutil::trigger_next_timer(&mut ctx));
        // After the second timer, we should reset our flag for v1 routers.
        let group_state = ctx
            .state
            .ip
            .get_igmp_state_mut(dev_id.id())
            .groups
            .get(&MulticastAddr::new(GROUP_ADDR).unwrap())
            .unwrap();
        match group_state.get_inner() {
            MemberState::Idle(state) => assert!(!state.get_protocol_specific().v1_router_present),
            _ => panic!("Wrong State!"),
        }

        receive_igmp_query(&mut ctx, dev_id, Duration::from_secs(10));
        assert!(testutil::trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher.frames_sent().len(), 3);
        // Now we should get V2 report
        assert_eq!(ctx.dispatcher.frames_sent().last().unwrap().1[38], 0x16);
        ensure_ttl_ihl_rtr(&mut ctx);
    }

    // TODO(zeling): add this test back once we can have a reliable and
    // deterministic way to make `duration` longer than 100ms.
    #[test]
    #[ignore]
    fn test_igmp_integration_delay_reset_timer() {
        let (mut ctx, dev_id) = setup_simple_test_environment();
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());
        assert_eq!(ctx.dispatcher.timer_events().count(), 1);
        let instant1 = ctx.dispatcher.timer_events().nth(0).unwrap().0.clone();
        let start = ctx.dispatcher.now();
        let duration = Duration::from_micros(((instant1 - start).as_micros() / 2) as u64);
        if duration.as_millis() < 100 {
            return;
        }
        receive_igmp_query(&mut ctx, dev_id, duration);
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
        let instant2 = ctx.dispatcher.timer_events().nth(0).unwrap().0.clone();
        // because of the message, our timer should be reset to a nearer future
        assert!(instant2 <= instant1);
        assert!(trigger_next_timer(&mut ctx));
        assert!(ctx.dispatcher.now() - start <= duration);
        assert_eq!(ctx.dispatcher.frames_sent().len(), 2);
        // make sure it is a V2 report
        assert_eq!(ctx.dispatcher.frames_sent().last().unwrap().1[38], 0x16);
        ensure_ttl_ihl_rtr(&mut ctx);
    }

    #[test]
    fn test_igmp_integration_last_send_leave() {
        let (mut ctx, dev_id) = setup_simple_test_environment();
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());
        assert_eq!(ctx.dispatcher.timer_events().count(), 1);
        // The initial unsolicited report
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
        assert!(testutil::trigger_next_timer(&mut ctx));
        // The report after the delay
        assert_eq!(ctx.dispatcher.frames_sent().len(), 2);
        assert!(igmp_leave_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap()).is_ok());
        // our leave message
        assert_eq!(ctx.dispatcher.frames_sent().len(), 3);

        let leave_frame = &ctx.dispatcher.frames_sent().last().unwrap().1;

        // make sure it is a leave message
        assert_eq!(leave_frame[38], 0x17);
        // and the destination is ALL-ROUTERS (224.0.0.2)
        assert_eq!(leave_frame[30], 224);
        assert_eq!(leave_frame[31], 0);
        assert_eq!(leave_frame[32], 0);
        assert_eq!(leave_frame[33], 2);
        ensure_ttl_ihl_rtr(&mut ctx);
    }

    #[test]
    fn test_igmp_integration_not_last_dont_send_leave() {
        let (mut ctx, dev_id) = setup_simple_test_environment();
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());
        assert_eq!(ctx.dispatcher.timer_events().count(), 1);
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
        receive_igmp_report(&mut ctx, dev_id);
        assert_eq!(ctx.dispatcher.timer_events().count(), 0);
        // The report should be discarded because we have received from someone else.
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
        assert!(igmp_leave_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap()).is_ok());
        // A leave message is not sent
        assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
        ensure_ttl_ihl_rtr(&mut ctx);
    }

    #[test]
    fn test_receive_general_query() {
        let (mut ctx, dev_id) = setup_simple_test_environment();
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR).unwrap());
        igmp_join_group(&mut ctx, dev_id, MulticastAddr::new(GROUP_ADDR_2).unwrap());
        assert_eq!(ctx.dispatcher.timer_events().count(), 2);
        // The initial unsolicited report
        assert_eq!(ctx.dispatcher.frames_sent().len(), 2);
        assert!(trigger_next_timer(&mut ctx));
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher.frames_sent().len(), 4);
        receive_igmp_general_query(&mut ctx, dev_id, Duration::from_secs(10));
        // Two new timers should be there.
        assert_eq!(ctx.dispatcher.timer_events().count(), 2);
        assert!(trigger_next_timer(&mut ctx));
        assert!(trigger_next_timer(&mut ctx));
        // Two new reports should be sent
        assert_eq!(ctx.dispatcher.frames_sent().len(), 6);
        ensure_ttl_ihl_rtr(&mut ctx);
    }
}
