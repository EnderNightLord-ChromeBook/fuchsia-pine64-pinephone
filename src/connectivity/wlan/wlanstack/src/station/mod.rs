// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod ap;
pub mod client;
pub mod mesh;

use failure::{bail, format_err};
use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, MlmeEvent, MlmeEventStream, MlmeProxy};
use fidl_fuchsia_wlan_stats::IfaceStats;
use fuchsia_async as fasync;
use futures::channel::mpsc;
use futures::prelude::*;
use futures::select;
use log::warn;
use pin_utils::pin_mut;
use std::marker::Unpin;
use std::sync::{Arc, Mutex};
use void::Void;
use wlan_sme::{
    timer::{TimeEntry, TimedEvent},
    MlmeRequest, MlmeStream, Station,
};

use crate::fidl_util::is_peer_closed;
use crate::stats_scheduler::StatsRequest;

// The returned future successfully terminates when MLME closes the channel
async fn serve_mlme_sme<STA, SRS, TS>(
    proxy: MlmeProxy,
    mut event_stream: MlmeEventStream,
    station: Arc<Mutex<STA>>,
    mut mlme_stream: MlmeStream,
    stats_requests: SRS,
    time_stream: TS,
) -> Result<(), failure::Error>
where
    STA: Station,
    SRS: Stream<Item = StatsRequest> + Unpin,
    TS: Stream<Item = TimeEntry<<STA as wlan_sme::Station>::Event>> + Unpin,
{
    let (mut stats_sender, stats_receiver) = mpsc::channel(1);
    let stats_fut = serve_stats(proxy.clone(), stats_requests, stats_receiver);
    pin_mut!(stats_fut);
    let mut stats_fut = stats_fut.fuse();

    let mut timeout_stream = make_timer_stream(time_stream).fuse();

    loop {
        select! {
            // Fuse rationale: any `none`s in the MLME stream should result in
            // bailing immediately, so we don't need to track if we've seen a
            // `None` or not and can `fuse` directly in the `select` call.
            mlme_event = event_stream.next().fuse() => match mlme_event {
                // Handle the stats response separately since it is SME-independent
                Some(Ok(MlmeEvent::StatsQueryResp{ resp })) => handle_stats_resp(&mut stats_sender, resp)?,
                Some(Ok(other)) => station.lock().unwrap().on_mlme_event(other),
                Some(Err(ref e)) if is_peer_closed(e) => return Ok(()),
                None => return Ok(()),
                Some(Err(e)) => bail!("Error reading an event from MLME channel: {}", e),
            },
            mlme_req = mlme_stream.next().fuse() => match mlme_req {
                Some(req) => match forward_mlme_request(req, &proxy) {
                    Ok(()) => {},
                    Err(ref e) if is_peer_closed(e) => return Ok(()),
                    Err(e) => bail!("Error forwarding a request from SME to MLME: {}", e),
                },
                None => bail!("Stream of requests from SME to MLME has ended unexpectedly"),
            },
            timeout = timeout_stream.next() => match timeout {
                Some(timed_event) => station.lock().unwrap().on_timeout(timed_event),
                None => bail!("SME timer stream has ended unexpectedly"),
            },
            stats = stats_fut => match stats? {},
        }
    }
}

fn make_timer_stream<E>(
    time_stream: impl Stream<Item = TimeEntry<E>>,
) -> impl Stream<Item = TimedEvent<E>> {
    time_stream
        .map(|(deadline, timed_event)| {
            fasync::Timer::new(fasync::Time::from_zx(deadline)).map(|_| timed_event)
        })
        .buffer_unordered(usize::max_value())
}

fn forward_mlme_request(req: MlmeRequest, proxy: &MlmeProxy) -> Result<(), fidl::Error> {
    match req {
        MlmeRequest::Scan(mut req) => proxy.start_scan(&mut req),
        MlmeRequest::Join(mut req) => proxy.join_req(&mut req),
        MlmeRequest::Authenticate(mut req) => proxy.authenticate_req(&mut req),
        MlmeRequest::AuthResponse(mut resp) => proxy.authenticate_resp(&mut resp),
        MlmeRequest::Associate(mut req) => proxy.associate_req(&mut req),
        MlmeRequest::AssocResponse(mut resp) => proxy.associate_resp(&mut resp),
        MlmeRequest::Deauthenticate(mut req) => proxy.deauthenticate_req(&mut req),
        MlmeRequest::Eapol(mut req) => proxy.eapol_req(&mut req),
        MlmeRequest::SetKeys(mut req) => proxy.set_keys_req(&mut req),
        MlmeRequest::SetCtrlPort(mut req) => proxy.set_controlled_port(&mut req),
        MlmeRequest::Start(mut req) => proxy.start_req(&mut req),
        MlmeRequest::Stop(mut req) => proxy.stop_req(&mut req),
        MlmeRequest::SendMpOpenAction(mut req) => proxy.send_mp_open_action(&mut req),
        MlmeRequest::SendMpConfirmAction(mut req) => proxy.send_mp_confirm_action(&mut req),
        MlmeRequest::MeshPeeringEstablished(mut req) => proxy.mesh_peering_established(&mut req),
    }
}

fn handle_stats_resp(
    stats_sender: &mut mpsc::Sender<IfaceStats>,
    resp: fidl_mlme::StatsQueryResponse,
) -> Result<(), failure::Error> {
    stats_sender.try_send(resp.stats).or_else(|e| {
        if e.is_full() {
            // We only expect one response from MLME per each request, so the bounded
            // queue of size 1 should always suffice.
            warn!("Received an extra GetStatsResp from MLME, discarding");
            Ok(())
        } else {
            Err(format_err!("Failed to send a message to stats future"))
        }
    })
}

async fn serve_stats<S>(
    proxy: MlmeProxy,
    mut stats_requests: S,
    mut responses: mpsc::Receiver<IfaceStats>,
) -> Result<Void, failure::Error>
where
    S: Stream<Item = StatsRequest> + Unpin,
{
    while let Some(req) = await!(stats_requests.next()) {
        proxy
            .stats_query_req()
            .map_err(|e| format_err!("Failed to send a StatsReq to MLME: {}", e))?;
        match await!(responses.next()) {
            Some(response) => req.reply(response),
            None => bail!("Stream of stats responses has ended unexpectedly"),
        };
    }
    Err(format_err!("Stream of stats requests has ended unexpectedly"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        fuchsia_zircon::{self as zx, DurationNum},
        futures::channel::mpsc::{self, UnboundedSender},
        futures::Poll,
        pin_utils::pin_mut,
    };

    type Event = u32;

    #[test]
    fn test_timer() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let fut = async {
            let (timer, time_stream) = mpsc::unbounded::<TimeEntry<Event>>();
            let mut timeout_stream = make_timer_stream(time_stream);
            let now = zx::Time::get(zx::ClockId::Monotonic);
            schedule(&timer, now + 40.millis(), 0);
            schedule(&timer, now + 10.millis(), 1);
            schedule(&timer, now + 20.millis(), 2);
            schedule(&timer, now + 30.millis(), 3);

            let mut events = vec![];
            for _ in 0u32..4 {
                match await!(timeout_stream.next()) {
                    Some(event) => events.push(event.event),
                    None => panic!("timer terminates prematurely"),
                }
            }
            events
        };
        pin_mut!(fut);
        for _ in 0u32..4 {
            assert_eq!(Poll::Pending, exec.run_until_stalled(&mut fut));
            assert!(exec.wake_next_timer().is_some());
        }
        match exec.run_until_stalled(&mut fut) {
            Poll::Ready(events) => assert_eq!(events, vec![1, 2, 3, 0]),
            Poll::Pending => panic!("expect future to complete"),
        }
    }

    fn schedule(timer: &UnboundedSender<TimeEntry<Event>>, deadline: zx::Time, event: Event) {
        let entry = (deadline, TimedEvent { id: 0, event });
        timer.unbounded_send(entry).expect("expect send successful");
    }
}
