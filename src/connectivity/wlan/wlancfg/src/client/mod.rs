// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Serves Client policy services.
//! Note: This implementation is still under development.
//!       Only connect requests will cause the underlying SME to attempt to connect to a given
//!       network.
//!       Unfortunately, there is currently no way to send an Epitaph in Rust. Thus, inbound
//!       controller and listener requests are simply dropped, causing the underlying channel to
//!       get closed.
use {
    crate::{
        config_management::{
            Credential, NetworkConfigError, NetworkIdentifier, SaveError, SavedNetworksManager,
        },
        util::{fuse_pending::FusePending, listener},
    },
    anyhow::{format_err, Error},
    fidl::epitaph::ChannelEpitaphExt,
    fidl_fuchsia_wlan_common as fidl_common, fidl_fuchsia_wlan_policy as fidl_policy,
    fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_zircon::{self as zx, prelude::*},
    futures::{
        channel::mpsc,
        lock::Mutex,
        prelude::*,
        select,
        stream::{FuturesOrdered, FuturesUnordered},
    },
    log::{error, info},
    scan::handle_scan,
    std::{convert::TryFrom, sync::Arc},
};

mod scan;

/// Max number of network configs that we will send at once through the network config iterator
/// in get_saved_networks. This depends on the maximum size of a FIDL NetworkConfig, so it may
/// need to change if a FIDL NetworkConfig or FIDL Credential changes.
const MAX_CONFIGS_PER_RESPONSE: usize = 100;

/// Wrapper around a Client interface, granting access to the Client SME.
/// A Client might not always be available, for example, if no Client interface was created yet.
#[derive(Debug)]
pub struct Client {
    proxy: Option<fidl_sme::ClientSmeProxy>,
    current_connection: Option<(NetworkIdentifier, Credential)>,
}

impl Client {
    /// Creates a new, empty Client. The returned Client effectively represents the state in which
    /// no client interface is available.
    pub fn new_empty() -> Self {
        Self { proxy: None, current_connection: None }
    }

    pub fn set_sme(&mut self, proxy: fidl_sme::ClientSmeProxy) {
        self.proxy = Some(proxy);
    }

    /// Accesses the Client interface's SME.
    /// Returns None if no Client interface is available.
    fn access_sme(&self) -> Option<&fidl_sme::ClientSmeProxy> {
        self.proxy.as_ref()
    }

    /// Disconnect from the specified network if we are currently connected to it.
    async fn disconnect_from(
        &mut self,
        network: (NetworkIdentifier, Credential),
        update_sender: listener::ClientMessageSender,
    ) {
        if self.current_connection.as_ref() != Some(&network) {
            return;
        }
        if let Some(client_sme) = self.access_sme() {
            client_sme.disconnect().await.unwrap_or_else(|e| {
                info!("Error disconnecting from network: {}", e);
                return;
            });
            self.current_connection = None;
            let update = listener::ClientStateUpdate {
                state: None,
                networks: vec![listener::ClientNetworkState {
                    id: network.0.into(),
                    state: fidl_policy::ConnectionState::Disconnected,
                    status: Some(fidl_policy::DisconnectStatus::ConnectionStopped),
                }],
            };
            if let Err(e) = update_sender.unbounded_send(listener::Message::NotifyListeners(update))
            {
                error!("failed to send disconnect update to listener: {:?}", e);
            };
        }
    }
}

impl From<fidl_sme::ClientSmeProxy> for Client {
    fn from(proxy: fidl_sme::ClientSmeProxy) -> Self {
        Self { proxy: Some(proxy), current_connection: None }
    }
}

#[derive(Debug)]
struct RequestError {
    cause: Error,
    status: fidl_common::RequestStatus,
}

impl RequestError {
    /// Produces a new `RequestError` for internal errors.
    fn new() -> Self {
        RequestError {
            cause: format_err!("internal error"),
            status: fidl_common::RequestStatus::RejectedNotSupported,
        }
    }

    fn with_cause(self, cause: Error) -> Self {
        RequestError { cause, ..self }
    }
}

#[derive(Debug)]
enum InternalMsg {
    /// Sent when a new connection request was issued. Holds the NetworkIdentifier, credential
    /// used to connect, and Transaction which the connection result will be reported through.
    NewPendingConnectRequest(
        fidl_policy::NetworkIdentifier,
        Credential,
        fidl_sme::ConnectTransactionProxy,
    ),
    /// Sent when a new scan request was issued. Holds the output iterator through which the
    /// scan results will be reported.
    NewPendingScanRequest(fidl::endpoints::ServerEnd<fidl_policy::ScanResultIteratorMarker>),
    NewDisconnectWatcher(NetworkIdentifier, Credential),
}
type InternalMsgSink = mpsc::UnboundedSender<InternalMsg>;

// This number was chosen arbitrarily.
const MAX_CONCURRENT_LISTENERS: usize = 1000;

type ClientRequests = fidl::endpoints::ServerEnd<fidl_policy::ClientControllerMarker>;
type SavedNetworksPtr = Arc<SavedNetworksManager>;
pub type ClientPtr = Arc<Mutex<Client>>;

pub fn spawn_provider_server(
    client: ClientPtr,
    update_sender: listener::ClientMessageSender,
    saved_networks: SavedNetworksPtr,
    requests: fidl_policy::ClientProviderRequestStream,
) {
    fasync::spawn(serve_provider_requests(client, update_sender, saved_networks, requests));
}

pub fn spawn_listener_server(
    update_sender: listener::ClientMessageSender,
    requests: fidl_policy::ClientListenerRequestStream,
) {
    fasync::spawn(serve_listener_requests(update_sender, requests));
}

/// Serves the ClientProvider protocol.
/// Only one ClientController can be active. Additional requests to register ClientControllers
/// will result in their channel being immediately closed.
async fn serve_provider_requests(
    client: ClientPtr,
    update_sender: listener::ClientMessageSender,
    saved_networks: SavedNetworksPtr,
    mut requests: fidl_policy::ClientProviderRequestStream,
) {
    let (internal_messages_sink, mut internal_messages_stream) = mpsc::unbounded();
    let mut pending_scans = FuturesUnordered::new();
    let mut controller_reqs = FuturesUnordered::new();
    let mut pending_con_reqs = FusePending(FuturesOrdered::new());
    let mut pending_disconnect_monitors = FuturesUnordered::new();

    loop {
        select! {
            // Progress controller requests.
            _ = controller_reqs.select_next_some() => (),
            // Process provider requests.
            req = requests.select_next_some() => if let Ok(req) = req {
                // If there is an active controller - reject new requests.
                // Rust cannot yet send Epitaphs when closing a channel, thus, simply drop the
                // request.
                if controller_reqs.is_empty() {
                    let fut = handle_provider_request(
                        Arc::clone(&client),
                        internal_messages_sink.clone(),
                        update_sender.clone(),
                        Arc::clone(&saved_networks),
                        req
                    );
                    controller_reqs.push(fut);
                } else {
                    if let Err(e) = reject_provider_request(req) {
                        error!("error sending rejection epitaph");
                    }
                }
            },
            // Progress internal messages.
            msg = internal_messages_stream.select_next_some() => match msg {
                InternalMsg::NewPendingConnectRequest(id, cred, txn) => {
                    let connect_result_fut = txn.take_event_stream().into_future()
                        .map(|(first, _next)| (id, cred, first));
                    pending_con_reqs.push(connect_result_fut);
                }
                InternalMsg::NewPendingScanRequest(output_iterator) => {
                    pending_scans.push(handle_scan(
                        Arc::clone(&client),
                        output_iterator));
                }
                InternalMsg::NewDisconnectWatcher(network_id, credential) => {
                    pending_disconnect_monitors.push(wait_for_disconnection(Arc::clone(&client), update_sender.clone(), network_id, credential));
                }
            },
            // Progress scans.
            () = pending_scans.select_next_some() => (),
            // Progress disconnect monitors.
            () = pending_disconnect_monitors.select_next_some() => (),
            // Pending connect request finished.
            resp = pending_con_reqs.select_next_some() => if let (id, cred, Some(Ok(txn))) = resp {
                handle_sme_connect_response(
                    update_sender.clone(),
                    internal_messages_sink.clone(),
                    id.into(),
                    cred,
                    txn,
                    Arc::clone(&saved_networks),
                    Arc::clone(&client)
                ).await;
            },
        }
    }
}

/// Serves the ClientListener protocol.
async fn serve_listener_requests(
    update_sender: listener::ClientMessageSender,
    requests: fidl_policy::ClientListenerRequestStream,
) {
    let serve_fut = requests
        .try_for_each_concurrent(MAX_CONCURRENT_LISTENERS, |req| {
            handle_listener_request(update_sender.clone(), req)
        })
        .unwrap_or_else(|e| error!("error serving Client Listener API: {}", e));
    let _ignored = serve_fut.await;
}

/// Handle inbound requests to acquire a new ClientController.
async fn handle_provider_request(
    client: ClientPtr,
    internal_msg_sink: InternalMsgSink,
    update_sender: listener::ClientMessageSender,
    saved_networks: SavedNetworksPtr,
    req: fidl_policy::ClientProviderRequest,
) -> Result<(), fidl::Error> {
    match req {
        fidl_policy::ClientProviderRequest::GetController { requests, updates, .. } => {
            register_listener(update_sender.clone(), updates.into_proxy()?);
            handle_client_requests(
                client,
                internal_msg_sink,
                update_sender,
                saved_networks,
                requests,
            )
            .await?;
            Ok(())
        }
    }
}

/// Logs a message for an incoming ClientControllerRequest
fn log_client_request(request: &fidl_policy::ClientControllerRequest) {
    info!(
        "Received policy client request {}",
        match request {
            fidl_policy::ClientControllerRequest::Connect { .. } => "Connect",
            fidl_policy::ClientControllerRequest::StartClientConnections { .. } =>
                "StartClientConnections",
            fidl_policy::ClientControllerRequest::StopClientConnections { .. } =>
                "StopClientConnections",
            fidl_policy::ClientControllerRequest::ScanForNetworks { .. } => "ScanForNetworks",
            fidl_policy::ClientControllerRequest::SaveNetwork { .. } => "SaveNetwork",
            fidl_policy::ClientControllerRequest::RemoveNetwork { .. } => "RemoveNetwork",
            fidl_policy::ClientControllerRequest::GetSavedNetworks { .. } => "GetSavedNetworks",
        }
    );
}

/// Handles all incoming requests from a ClientController.
async fn handle_client_requests(
    client: ClientPtr,
    internal_msg_sink: InternalMsgSink,
    update_sender: listener::ClientMessageSender,
    saved_networks: SavedNetworksPtr,
    requests: ClientRequests,
) -> Result<(), fidl::Error> {
    let mut request_stream = requests.into_stream()?;
    while let Some(request) = request_stream.try_next().await? {
        log_client_request(&request);
        match request {
            fidl_policy::ClientControllerRequest::Connect { id, responder, .. } => {
                match handle_client_request_connect(
                    update_sender.clone(),
                    Arc::clone(&client),
                    Arc::clone(&saved_networks),
                    &id,
                )
                .await
                {
                    Ok((cred, txn)) => {
                        responder.send(fidl_common::RequestStatus::Acknowledged)?;
                        let _ignored = internal_msg_sink
                            .unbounded_send(InternalMsg::NewPendingConnectRequest(id, cred, txn));
                    }
                    Err(error) => {
                        error!("error while connection attempt: {}", error.cause);
                        responder.send(error.status)?;
                    }
                }
            }
            fidl_policy::ClientControllerRequest::StartClientConnections { responder } => {
                let status = handle_client_request_start_connections();
                responder.send(status)?;
            }
            fidl_policy::ClientControllerRequest::StopClientConnections { responder } => {
                let status = handle_client_request_stop_connections();
                responder.send(status)?;
            }
            fidl_policy::ClientControllerRequest::ScanForNetworks { iterator, .. } => {
                if let Err(e) =
                    internal_msg_sink.unbounded_send(InternalMsg::NewPendingScanRequest(iterator))
                {
                    error!("Failed to send internal message: {:?}", e)
                }
            }
            fidl_policy::ClientControllerRequest::SaveNetwork { config, responder } => {
                // If there is an error saving the network, log it and convert to a FIDL value.
                let mut response =
                    handle_client_request_save_network(Arc::clone(&saved_networks), config)
                        .map_err(|e| {
                            error!("Failed to save network: {:?}", e);
                            fidl_policy::NetworkConfigChangeError::from(e)
                        });
                responder.send(&mut response)?;
            }
            fidl_policy::ClientControllerRequest::RemoveNetwork { config, responder } => {
                let mut response = handle_client_request_remove_network(
                    Arc::clone(&saved_networks),
                    config,
                    Arc::clone(&client),
                    update_sender.clone(),
                )
                .await
                .map_err(|e| {
                    error!("Error removing network: {:?}", e);
                    SaveError::GeneralError
                });
                responder.send(&mut response)?;
            }
            fidl_policy::ClientControllerRequest::GetSavedNetworks { iterator, .. } => {
                handle_client_request_get_networks(Arc::clone(&saved_networks), iterator).await?;
            }
        }
    }
    Ok(())
}

const DISCONNECTION_MONITOR_SECONDS: i64 = 10;
async fn wait_for_disconnection(
    client: ClientPtr,
    update_sender: listener::ClientMessageSender,
    network: NetworkIdentifier,
    credential: Credential,
) {
    // Loop until we're no longer connected to the network
    loop {
        fuchsia_async::Timer::new(DISCONNECTION_MONITOR_SECONDS.seconds().after_now()).await;
        let client = client.lock().await;

        // Stop watching for a disconnect if a disconnect or change has already been triggered from
        // the policy layer. current_connection is changed by anything that changes connection
        // state in this layer.
        if client.current_connection != Some((network.clone(), credential.clone())) {
            return;
        }

        let status_fut = match client.access_sme().map(|sme| sme.status()) {
            Some(status_fut) => status_fut,
            _ => break,
        };
        if let Ok(status) = status_fut.await {
            if let Some(current_network) = status.connected_to {
                if current_network.ssid == network.ssid {
                    continue;
                }
            }
        };
        break;
    }

    // Send a notification
    info!("Disconnected from network, sending notification to listeners");
    let update = listener::ClientStateUpdate {
        state: None,
        networks: vec![listener::ClientNetworkState {
            id: network.into(),
            state: fidl_policy::ConnectionState::Disconnected,
            status: None,
        }],
    };
    if let Err(e) = update_sender.unbounded_send(listener::Message::NotifyListeners(update)) {
        error!("failed to send update to listener: {:?}", e);
    };
}

async fn handle_sme_connect_response(
    update_sender: listener::ClientMessageSender,
    internal_msg_sink: InternalMsgSink,
    id: NetworkIdentifier,
    credential: Credential,
    txn_event: fidl_sme::ConnectTransactionEvent,
    saved_networks: SavedNetworksPtr,
    client: ClientPtr,
) {
    match txn_event {
        fidl_sme::ConnectTransactionEvent::OnFinished { code } => {
            use fidl_policy::ConnectionState as policy_state;
            use fidl_policy::DisconnectStatus as policy_dc_status;
            use fidl_sme::ConnectResultCode as sme_code;
            // If we were successful, record it and await disconnection
            if code == sme_code::Success {
                info!("connection request successful to: {:?}", String::from_utf8_lossy(&id.ssid));
                saved_networks.record_connect_success(id.clone(), &credential);
                client.lock().await.current_connection = Some((id.clone(), credential.clone()));
                if let Err(e) = internal_msg_sink
                    .unbounded_send(InternalMsg::NewDisconnectWatcher(id.clone(), credential))
                {
                    error!("failed to queue disconnect watcher: {:?}", e);
                };
            }
            // Send an update to listeners
            let update = listener::ClientStateUpdate {
                state: None,
                networks: vec![listener::ClientNetworkState {
                    id: id.into(),
                    state: match code {
                        sme_code::Success => policy_state::Connected,
                        sme_code::Canceled => policy_state::Disconnected,
                        sme_code::Failed => policy_state::Failed,
                        sme_code::BadCredentials => policy_state::Failed,
                    },
                    status: match code {
                        sme_code::Success => None,
                        sme_code::Canceled => Some(policy_dc_status::ConnectionStopped),
                        sme_code::Failed => Some(policy_dc_status::ConnectionFailed),
                        sme_code::BadCredentials => Some(policy_dc_status::CredentialsFailed),
                    },
                }],
            };
            if let Err(e) = update_sender.unbounded_send(listener::Message::NotifyListeners(update))
            {
                error!("failed to send update to listener: {:?}", e);
            };
        }
    }
}

/// Attempts to issue a new connect request to the currently active Client.
/// The network's configuration must have been stored before issuing a connect request.
async fn handle_client_request_connect(
    update_sender: listener::ClientMessageSender,
    client: ClientPtr,
    saved_networks: SavedNetworksPtr,
    network: &fidl_policy::NetworkIdentifier,
) -> Result<(Credential, fidl_sme::ConnectTransactionProxy), RequestError> {
    let network_config = saved_networks
        .lookup(NetworkIdentifier::new(network.ssid.clone(), network.type_.into()))
        .pop()
        .ok_or_else(|| {
            RequestError::new().with_cause(format_err!(
                "error network not found: {}",
                String::from_utf8_lossy(&network.ssid)
            ))
        })?;

    // TODO(hahnr): Discuss whether every request should verify the existence of a Client, or
    // whether that should be handled by either, closing the currently active controller if a
    // client interface is brought down and not supporting controller requests if no client
    // interface is active.
    let client = client.lock().await;
    let client_sme = client
        .access_sme()
        .ok_or_else(|| RequestError::new().with_cause(format_err!("no active client interface")))?;

    let credential = sme_credential_from_policy(&network_config.credential);
    let mut request = fidl_sme::ConnectRequest {
        ssid: network.ssid.to_vec(),
        credential,
        radio_cfg: fidl_sme::RadioConfig {
            override_phy: false,
            phy: fidl_common::Phy::Vht,
            override_cbw: false,
            cbw: fidl_common::Cbw::Cbw80,
            override_primary_chan: false,
            primary_chan: 0,
        },
        deprecated_scan_type: fidl_common::ScanType::Passive,
    };
    let (local, remote) = fidl::endpoints::create_proxy().map_err(|e| {
        RequestError::new().with_cause(format_err!("failed to create proxy: {:?}", e))
    })?;
    client_sme.connect(&mut request, Some(remote)).map_err(|e| {
        RequestError::new().with_cause(format_err!("failed to connect to sme: {:?}", e))
    })?;

    let update = listener::ClientStateUpdate {
        state: None,
        networks: vec![listener::ClientNetworkState {
            id: network.clone(),
            state: fidl_policy::ConnectionState::Connecting,
            status: None,
        }],
    };
    if let Err(e) = update_sender.unbounded_send(listener::Message::NotifyListeners(update)) {
        error!("failed to send update to listener: {:?}", e);
    };

    Ok((network_config.credential, local))
}

/// This is not yet implemented and just returns that request is not supported
fn handle_client_request_start_connections() -> fidl_common::RequestStatus {
    fidl_common::RequestStatus::RejectedNotSupported
}

/// This is not yet implemented and just returns that the request is not supported
fn handle_client_request_stop_connections() -> fidl_common::RequestStatus {
    fidl_common::RequestStatus::RejectedNotSupported
}

/// This function handles requests to save a network by saving the network and sending back to the
/// controller whether or not we successfully saved the network. There could be a FIDL error in
/// sending the response.
fn handle_client_request_save_network(
    saved_networks: SavedNetworksPtr,
    network_config: fidl_policy::NetworkConfig,
) -> Result<(), NetworkConfigError> {
    // The FIDL network config fields are defined as Options, and we consider it an error if either
    // field is missing (ie None) here.
    let net_id = network_config.id.ok_or_else(|| NetworkConfigError::ConfigMissingId)?;
    let credential = Credential::try_from(
        network_config.credential.ok_or_else(|| NetworkConfigError::ConfigMissingCredential)?,
    )?;
    saved_networks.store(NetworkIdentifier::from(net_id), credential)?;
    Ok(())
}

/// Will remove the specified network and respond to the remove network request with a network
/// config change error if an error occurs while trying to remove the network. If the network
/// config is successfully removed and currently connected, we will disconnect.
async fn handle_client_request_remove_network(
    saved_networks: SavedNetworksPtr,
    network_config: fidl_policy::NetworkConfig,
    client: ClientPtr,
    update_sender: listener::ClientMessageSender,
) -> Result<(), NetworkConfigError> {
    // The FIDL network config fields are defined as Options, and we consider it an error if either
    // field is missing (ie None) here.
    let net_id = NetworkIdentifier::from(
        network_config.id.ok_or_else(|| NetworkConfigError::ConfigMissingId)?,
    );
    let credential = Credential::try_from(
        network_config.credential.ok_or_else(|| NetworkConfigError::ConfigMissingCredential)?,
    )?;
    saved_networks.remove(net_id.clone(), credential.clone())?;

    // If we are currently connected to this network, disconnect from it
    client.lock().await.disconnect_from((net_id, credential), update_sender).await;
    Ok(())
}

/// For now, instead of giving actual results simply give nothing.
async fn handle_client_request_get_networks(
    saved_networks: SavedNetworksPtr,
    iterator: fidl::endpoints::ServerEnd<fidl_policy::NetworkConfigIteratorMarker>,
) -> Result<(), fidl::Error> {
    // make sufficiently small batches of networks to send and convert configs to FIDL values
    let network_configs = saved_networks.get_networks();
    let chunks = network_configs.chunks(MAX_CONFIGS_PER_RESPONSE);
    let fidl_chunks = chunks.into_iter().map(|chunk| {
        chunk
            .iter()
            .map(fidl_policy::NetworkConfig::from)
            .collect::<Vec<fidl_policy::NetworkConfig>>()
    });
    let mut stream = iterator.into_stream()?;
    for chunk in fidl_chunks {
        send_next_chunk(&mut stream, chunk).await?;
    }
    send_next_chunk(&mut stream, vec![]).await
}

/// Send a chunk of saved networks to the specified FIDL iterator
async fn send_next_chunk(
    stream: &mut fidl_policy::NetworkConfigIteratorRequestStream,
    chunk: Vec<fidl_policy::NetworkConfig>,
) -> Result<(), fidl::Error> {
    if let Some(req) = stream.try_next().await? {
        let fidl_policy::NetworkConfigIteratorRequest::GetNext { responder } = req;
        responder.send(&mut chunk.into_iter())
    } else {
        // This will happen if the iterator request stream was closed and we expected to send
        // another response.
        // TODO(45113) Test this error path
        info!("Info: peer closed channel for network config results unexpectedly");
        Ok(())
    }
}

/// convert from policy fidl Credential to sme fidl Credential
pub fn sme_credential_from_policy(cred: &Credential) -> fidl_sme::Credential {
    match cred {
        Credential::Password(pwd) => fidl_sme::Credential::Password(pwd.clone()),
        Credential::Psk(psk) => fidl_sme::Credential::Psk(psk.clone()),
        Credential::None => fidl_sme::Credential::None(fidl_sme::Empty {}),
    }
}

/// Handle inbound requests to register an additional ClientStateUpdates listener.
async fn handle_listener_request(
    update_sender: listener::ClientMessageSender,
    req: fidl_policy::ClientListenerRequest,
) -> Result<(), fidl::Error> {
    match req {
        fidl_policy::ClientListenerRequest::GetListener { updates, .. } => {
            register_listener(update_sender, updates.into_proxy()?);
            Ok(())
        }
    }
}

/// Registers a new update listener.
/// The client's current state will be send to the newly added listener immediately.
fn register_listener(
    update_sender: listener::ClientMessageSender,
    listener: fidl_policy::ClientStateUpdatesProxy,
) {
    let _ignored = update_sender.unbounded_send(listener::Message::NewListener(listener));
}

/// Rejects a ClientProvider request by sending a corresponding Epitaph via the |requests| and
/// |updates| channels.
fn reject_provider_request(req: fidl_policy::ClientProviderRequest) -> Result<(), fidl::Error> {
    match req {
        fidl_policy::ClientProviderRequest::GetController { requests, updates, .. } => {
            requests.into_channel().close_with_epitaph(zx::Status::ALREADY_BOUND)?;
            updates.into_channel().close_with_epitaph(zx::Status::ALREADY_BOUND)?;
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            config_management::{NetworkConfig, SavedNetworksManager, SecurityType, PSK_BYTE_LEN},
            util::logger::set_logger_for_test,
        },
        fidl::{
            endpoints::{create_proxy, create_request_stream},
            Error,
        },
        futures::{channel::mpsc, task::Poll},
        pin_utils::pin_mut,
        tempfile::TempDir,
        wlan_common::assert_variant,
    };

    /// Creates an ESS Store holding entries for protected and unprotected networks.
    async fn create_network_store(stash_id: impl AsRef<str>) -> SavedNetworksPtr {
        let temp_dir = TempDir::new().expect("failed to create temp dir");
        let path = temp_dir.path().join("networks.json");
        let tmp_path = temp_dir.path().join("tmp.json");
        let saved_networks = Arc::new(
            SavedNetworksManager::new_with_stash_or_paths(stash_id, path, tmp_path)
                .await
                .expect("Failed to create a KnownEssStore"),
        );
        let network_id_none = NetworkIdentifier::new(b"foobar".to_vec(), SecurityType::None);
        let network_id_password =
            NetworkIdentifier::new(b"foobar-protected".to_vec(), SecurityType::Wpa2);
        let network_id_psk = NetworkIdentifier::new(b"foobar-psk".to_vec(), SecurityType::Wpa2);

        saved_networks.store(network_id_none, Credential::None).expect("error saving network");
        saved_networks
            .store(network_id_password, Credential::Password(b"supersecure".to_vec()))
            .expect("error saving network");
        saved_networks
            .store(network_id_psk, Credential::Psk(vec![64; PSK_BYTE_LEN].to_vec()))
            .expect("error saving network foobar-psk");

        saved_networks
    }

    /// Requests a new ClientController from the given ClientProvider.
    fn request_controller(
        provider: &fidl_policy::ClientProviderProxy,
    ) -> (fidl_policy::ClientControllerProxy, fidl_policy::ClientStateUpdatesRequestStream) {
        let (controller, requests) = create_proxy::<fidl_policy::ClientControllerMarker>()
            .expect("failed to create ClientController proxy");
        let (update_sink, update_stream) =
            create_request_stream::<fidl_policy::ClientStateUpdatesMarker>()
                .expect("failed to create ClientStateUpdates proxy");
        provider.get_controller(requests, update_sink).expect("error getting controller");
        (controller, update_stream)
    }

    /// Creates a Client wrapper.
    async fn create_client() -> (ClientPtr, fidl_sme::ClientSmeRequestStream) {
        let (client_sme, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        let client = Arc::new(Mutex::new(Client::from(client_sme)));
        (client, remote.into_stream().expect("failed to create stream"))
    }

    struct TestValues {
        saved_networks: SavedNetworksPtr,
        provider: fidl_policy::ClientProviderProxy,
        requests: fidl_policy::ClientProviderRequestStream,
        client: ClientPtr,
        sme_stream: fidl_sme::ClientSmeRequestStream,
        update_sender: mpsc::UnboundedSender<listener::ClientMessage>,
        listener_updates: mpsc::UnboundedReceiver<listener::ClientMessage>,
    }

    // setup channels and proxies needed for the tests to use use the Client Provider and
    // Client Controller APIs in tests. The stash id should be the test name so that each
    // test will have a unique persistent store behind it.
    fn test_setup(stash_id: impl AsRef<str>, exec: &mut fasync::Executor) -> TestValues {
        let saved_networks = exec.run_singlethreaded(create_network_store(stash_id));
        let (provider, requests) = create_proxy::<fidl_policy::ClientProviderMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");
        let (client, sme_stream) = exec.run_singlethreaded(create_client());
        let (update_sender, listener_updates) = mpsc::unbounded();
        set_logger_for_test();
        TestValues {
            saved_networks,
            provider,
            requests,
            client,
            sme_stream,
            update_sender,
            listener_updates,
        }
    }

    #[test]
    fn connect_request_unknown_network() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup("connect_request_unknown_network", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar-unknown".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::RejectedNotSupported))
        );

        // unknown network should not have been saved by saved networks manager
        // since we did not successfully connect
        assert!(test_values
            .saved_networks
            .lookup(NetworkIdentifier::new(b"foobar-unknown".to_vec(), SecurityType::None))
            .is_empty());
    }

    #[test]
    fn connect_request_open_network() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");

        let mut test_values = test_setup("connect_request_open_network", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::Acknowledged))
        );

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                req, ..
            }))) => {
                assert_eq!(b"foobar", &req.ssid[..]);
                assert_eq!(fidl_sme::Credential::None(fidl_sme::Empty), req.credential);
            }
        );
    }

    #[test]
    fn connect_request_protected_network() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("connect_request_protected_network", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar-protected".to_vec(),
            type_: fidl_policy::SecurityType::Wpa2,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::Acknowledged))
        );

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                req, ..
            }))) => {
                assert_eq!(b"foobar-protected", &req.ssid[..]);
                assert_eq!(fidl_sme::Credential::Password(b"supersecure".to_vec()), req.credential);
                // TODO(hahnr): Send connection response.
            }
        );
    }

    #[test]
    fn connect_request_protected_psk_network() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("connect_request_protected_psk_network", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar-psk".to_vec(),
            type_: fidl_policy::SecurityType::Wpa2,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::Acknowledged))
        );

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                req, ..
            }))) => {
                assert_eq!(b"foobar-psk", &req.ssid[..]);
                assert_eq!(fidl_sme::Credential::Psk([64; PSK_BYTE_LEN].to_vec()), req.credential);
                // TODO(hahnr): Send connection response.
            }
        );
    }

    #[test]
    fn connect_request_success() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("connect_request_success", &mut exec);
        let serve_fut = serve_provider_requests(
            Arc::clone(&test_values.client),
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(_)
        );

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut connect_fut), Poll::Ready(Ok(_)));

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Connecting,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                txn, ..
            }))) => {
                // Send connection response.
                let (_stream, ctrl) = txn.expect("connect txn unused")
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_finished(fidl_sme::ConnectResultCode::Success)
                    .expect("failed to send connection completion");
            }
        );

        // Process SME result.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Connected,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);

        // saved network config should reflect that it has connected successfully
        let network_id = NetworkIdentifier::new(b"foobar".to_vec(), SecurityType::None);
        let credential = Credential::None;
        let cfg = get_config(
            Arc::clone(&test_values.saved_networks),
            network_id.clone(),
            credential.clone(),
        );
        assert_variant!(cfg, Some(cfg) => {
            assert!(cfg.has_ever_connected)
        });
        // Verify current_connection of the client is set when we connect.
        assert_variant!(exec.run_until_stalled(&mut test_values.client.lock()), Poll::Ready(client) => {
            assert_eq!(client.current_connection, Some((network_id, credential)));
        });
    }

    #[test]
    fn connect_request_failure() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("connect_request_failure", &mut exec);
        let serve_fut = serve_provider_requests(
            Arc::clone(&test_values.client),
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(_)
        );

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut connect_fut), Poll::Ready(Ok(_)));

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Connecting,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                txn, ..
            }))) => {
                // Send failed connection response.
                let (_stream, ctrl) = txn.expect("connect txn unused")
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_finished(fidl_sme::ConnectResultCode::Failed)
                    .expect("failed to send connection completion");
            }
        );

        // Process SME result.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Failed,
                status: Some(fidl_policy::DisconnectStatus::ConnectionFailed),
            }],
        };
        assert_eq!(summary, expected_summary);

        // Verify network config reflects that we still have not connected successfully
        let cfg = get_config(
            test_values.saved_networks,
            NetworkIdentifier::new(b"foobar".to_vec(), SecurityType::None),
            Credential::None,
        );
        assert_variant!(cfg, Some(cfg) => {
            assert_eq!(false, cfg.has_ever_connected);
        });
    }

    #[test]
    fn connect_request_bad_password() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("connect_request_bad_password", &mut exec);
        let serve_fut = serve_provider_requests(
            Arc::clone(&test_values.client),
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(_)
        );

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut connect_fut), Poll::Ready(Ok(_)));

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Connecting,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                txn, ..
            }))) => {
                // Send failed connection response.
                let (_stream, ctrl) = txn.expect("connect txn unused")
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_finished(fidl_sme::ConnectResultCode::BadCredentials)
                    .expect("failed to send connection completion");
            }
        );

        // Process SME result.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Failed,
                status: Some(fidl_policy::DisconnectStatus::CredentialsFailed),
            }],
        };
        assert_eq!(summary, expected_summary);

        // Verify network config reflects that we still have not connected successfully
        let cfg = get_config(
            test_values.saved_networks,
            NetworkIdentifier::new(b"foobar".to_vec(), SecurityType::None),
            Credential::None,
        );
        assert_variant!(cfg, Some(cfg) => {
            assert_eq!(false, cfg.has_ever_connected);
        });

        // Current connection shouldn't change if we didn't connect
        assert_variant!(exec.run_until_stalled(&mut test_values.client.lock()), Poll::Ready(client) => {
            assert!(client.current_connection.is_none());
        });
    }

    #[test]
    fn start_and_stop_client_connections_should_fail() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup("start_and_stop_client_connections_should_fail", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should now be waiting for request.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue request to start client connections.
        let start_fut = controller.start_client_connections();
        pin_mut!(start_fut);

        // Request should be rejected.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut start_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::RejectedNotSupported))
        );

        // Issue request to stop client connections.
        let stop_fut = controller.stop_client_connections();
        pin_mut!(stop_fut);

        // Request should be rejected.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut stop_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::RejectedNotSupported))
        );
    }

    #[test]
    fn disconnect_update() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("disconnect_update", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(_)
        );

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request and verify connect response.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut connect_fut), Poll::Ready(Ok(_)));

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Connecting,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);

        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                txn, ..
            }))) => {
                // Send connection response.
                let (_stream, ctrl) = txn.expect("connect txn unused")
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_finished(fidl_sme::ConnectResultCode::Success)
                    .expect("failed to send connection completion");
            }
        );

        // Process SME result.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Connected,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);

        // Wake up the disconnect monitor timer and send SME status request.
        exec.wake_next_timer();
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Expect a status request from the disconnect monitor to the SME.
        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Status {
                responder, ..
            }))) => {
                // Send status response.
                let mut resp = fidl_sme::ClientStatusResponse{
                    connected_to: None,
                    connecting_to_ssid: vec![]
                };
                responder.send(&mut resp).expect("failed to send status update");
            }
        );

        // Process SME result.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Verify status update.
        let summary = assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => summary
        );
        let expected_summary = listener::ClientStateUpdate {
            state: None,
            networks: vec![listener::ClientNetworkState {
                id: fidl_policy::NetworkIdentifier {
                    ssid: b"foobar".to_vec(),
                    type_: fidl_policy::SecurityType::None,
                },
                state: fidl_policy::ConnectionState::Disconnected,
                status: None,
            }],
        };
        assert_eq!(summary, expected_summary);
    }

    /// End-to-end test of the scan function, verifying that an incoming
    /// FIDL scan request results in a scan in the SME, and that the results
    /// make it back to the requester.
    #[test]
    fn scan_end_to_end() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup(line!().to_string(), &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            Arc::clone(&test_values.saved_networks),
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(_)
        );

        // Create a set of endpoints.
        let (iter, server) =
            fidl::endpoints::create_proxy::<fidl_policy::ScanResultIteratorMarker>()
                .expect("failed to create iterator");
        let mut output_iter_fut = iter.get_next();

        // Issue request to scan.
        controller.scan_for_networks(server).expect("Failed to call scan for networks");

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);
        // Progress sever side forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back results
        assert_variant!(
            exec.run_until_stalled(&mut test_values.sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, ..
            }))) => {
                // Send the first AP
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_result(&mut vec![].into_iter())
                    .expect("failed to send scan data");
                // Send the end of data
                ctrl.send_on_finished()
                    .expect("failed to send scan data");
            }
        );

        // Process SME result.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // The iterator should have scan results.
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results.len(), 0);
        });
    }

    #[test]
    fn save_network() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let stash_id = "save_network";
        let temp_dir = TempDir::new().expect("failed to create temp dir");
        let path = temp_dir.path().join("networks.json");
        let tmp_path = temp_dir.path().join("tmp.json");
        let saved_networks_fut =
            SavedNetworksManager::new_with_stash_or_paths(stash_id, path, tmp_path);
        pin_mut!(saved_networks_fut);
        let saved_networks = Arc::new(
            exec.run_singlethreaded(saved_networks_fut).expect("Failed to create a KnownEssStore"),
        );

        let (provider, requests) = create_proxy::<fidl_policy::ClientProviderMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");

        let (client, _sme_stream) = exec.run_singlethreaded(create_client());
        let (update_sender, mut listener_updates) = mpsc::unbounded();
        let serve_fut =
            serve_provider_requests(client, update_sender, Arc::clone(&saved_networks), requests);
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut listener_updates.next()), Poll::Ready(_));

        // Save some network
        let network_id = fidl_policy::NetworkIdentifier {
            ssid: b"foo".to_vec(),
            type_: fidl_policy::SecurityType::None,
        };
        let network_config = fidl_policy::NetworkConfig {
            id: Some(network_id.clone()),
            credential: Some(fidl_policy::Credential::None(fidl_policy::Empty)),
        };
        let mut save_fut = controller.save_network(network_config);

        // Run server_provider forward so that it will process the save network request
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Check that the the response says we succeeded.
        assert_variant!(exec.run_until_stalled(&mut save_fut), Poll::Ready(result) => {
            let save_result = result.expect("Failed to get save network response");
            assert_eq!(save_result, Ok(()));
        });

        // Check that the value was actually saved in the saved networks manager.
        let target_id = NetworkIdentifier::from(network_id);
        let target_config = NetworkConfig::new(target_id.clone(), Credential::None, false, false)
            .expect("Failed to create network config");
        assert_eq!(saved_networks.lookup(target_id), vec![target_config]);
    }

    #[test]
    fn save_bad_network_should_fail() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let stash_id = "save_bad_network_should_fail";
        let temp_dir = TempDir::new().expect("failed to create temp dir");
        let path = temp_dir.path().join("networks.json");
        let tmp_path = temp_dir.path().join("tmp.json");

        // Need to create this here so that the temp files will be in scope here.
        let saved_networks_fut =
            SavedNetworksManager::new_with_stash_or_paths(stash_id, path, tmp_path);
        pin_mut!(saved_networks_fut);
        let _saved_networks = Arc::new(
            exec.run_singlethreaded(&mut saved_networks_fut)
                .expect("Failed to create a KnownEssStore"),
        );
        let saved_networks = exec.run_singlethreaded(create_network_store(stash_id));

        let (provider, requests) = create_proxy::<fidl_policy::ClientProviderMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");

        let (client, _sme_stream) = exec.run_singlethreaded(create_client());
        let (update_sender, mut listener_updates) = mpsc::unbounded();
        let serve_fut =
            serve_provider_requests(client, update_sender, Arc::clone(&saved_networks), requests);
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut listener_updates.next()), Poll::Ready(_));

        // Create a network config whose password is too short. FIDL network config does not
        // require valid fields unlike our crate define config. We should not be able to
        // successfully save this network through the API.
        let bad_network_id = fidl_policy::NetworkIdentifier {
            ssid: b"foo".to_vec(),
            type_: fidl_policy::SecurityType::Wpa2,
        };
        let network_config = fidl_policy::NetworkConfig {
            id: Some(bad_network_id.clone()),
            credential: Some(fidl_policy::Credential::Password(b"bar".to_vec())),
        };
        // Attempt to save the config
        let mut save_fut = controller.save_network(network_config);

        // Run server_provider forward so that it will process the save network request
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Check that the the response says we failed to save the network.
        assert_variant!(exec.run_until_stalled(&mut save_fut), Poll::Ready(result) => {
            let error = result.expect("Failed to get save network response");
            assert_eq!(error, Err(SaveError::GeneralError));
        });

        // Check that the value was was not saved in saved networks manager.
        let target_id = NetworkIdentifier::from(bad_network_id);
        assert_eq!(saved_networks.lookup(target_id), vec![]);
    }

    #[test]
    fn test_remove_not_connected() {
        let is_connected = false;
        let stash_id = line!().to_string();
        test_remove_a_network(is_connected, stash_id);
    }

    #[test]
    fn test_remove_connected() {
        let is_connected = true;
        let stash_id = line!().to_string();
        test_remove_a_network(is_connected, stash_id);
    }

    fn test_remove_a_network(is_connected: bool, stash_id: impl AsRef<str>) {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let temp_dir = TempDir::new().expect("failed to create temp dir");
        let path = temp_dir.path().join("networks.json");
        let tmp_path = temp_dir.path().join("tmp.json");

        // Need to create this here so that the temp files will be in scope here.
        let saved_networks_fut =
            SavedNetworksManager::new_with_stash_or_paths(stash_id, path, tmp_path);
        pin_mut!(saved_networks_fut);
        let saved_networks = Arc::new(
            exec.run_singlethreaded(&mut saved_networks_fut)
                .expect("Failed to create a KnownEssStore"),
        );
        let (provider, requests) = create_proxy::<fidl_policy::ClientProviderMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");

        let (client, mut sme_stream) = exec.run_singlethreaded(create_client());
        let (update_sender, mut listener_updates) = mpsc::unbounded();
        let serve_fut = serve_provider_requests(
            Arc::clone(&client),
            update_sender,
            Arc::clone(&saved_networks),
            requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut listener_updates.next()), Poll::Ready(_));

        let network_id = NetworkIdentifier::new("foo", SecurityType::None);
        let credential = Credential::None;
        saved_networks.store(network_id.clone(), credential.clone()).expect("failed to store");

        // If testing a connected network, first connect to it in order to test that we will
        // disconnect and that disconnect watcher will stop checking for disconnects.
        if is_connected {
            let connect_fut =
                controller.connect(&mut fidl_policy::NetworkIdentifier::from(network_id.clone()));
            pin_mut!(connect_fut);

            // Process connect request and verify connect response.
            assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
            assert_variant!(exec.run_until_stalled(&mut connect_fut), Poll::Ready(Ok(_)));
            process_connect(&mut exec, &mut sme_stream);
            // Process SME result.
            assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
            // Get listener update (for connecting and connect) so it isn't in front of the disconnect update later.
            assert_variant!(
                exec.run_until_stalled(&mut listener_updates.next()),
                Poll::Ready(Some(listener::Message::NotifyListeners(_)))
            );
            assert_variant!(
                exec.run_until_stalled(&mut listener_updates.next()),
                Poll::Ready(Some(listener::Message::NotifyListeners(_)))
            );
        }

        // Request to remove some network
        let network_config = fidl_policy::NetworkConfig {
            id: Some(fidl_policy::NetworkIdentifier::from(network_id.clone())),
            credential: Some(fidl_policy::Credential::from(credential.clone())),
        };
        let mut remove_fut = controller.remove_network(network_config);

        // Run server_provider forward so that it will process the remove request
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // If we have connected to a network, we expect a disconnect request and must proccess it.
        if is_connected {
            assert_variant!(
                exec.run_until_stalled(&mut sme_stream.next()),
                Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Disconnect{responder}))) => {
                    responder.send().expect("Failed to send disconnect completion");
                }
            );
            assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
            assert_variant!(exec.run_until_stalled(&mut remove_fut), Poll::Ready(Ok(Ok(()))));
            assert_variant!(
                exec.run_until_stalled(&mut listener_updates.next()),
                Poll::Ready(Some(listener::Message::NotifyListeners(summary))) => {
                    assert_eq!(summary, listener::ClientStateUpdate{state: None, networks: vec![
                        listener::ClientNetworkState {
                            id: network_id.clone().into(),
                            state: fidl_policy::ConnectionState::Disconnected,
                            status: Some(fidl_policy::DisconnectStatus::ConnectionStopped),
                        }
                    ]});
                }
            );

            // Check that the disconnect watcher exits without checking for disconnects
            exec.wake_next_timer();
            assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
            assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);

            // Check that we don't try to disconnect again if we save and remove again.
            saved_networks
                .store(network_id.clone(), credential.clone())
                .expect("Failed to save network");
            let network_config = fidl_policy::NetworkConfig {
                id: Some(fidl_policy::NetworkIdentifier::from(network_id.clone())),
                credential: Some(fidl_policy::Credential::from(credential)),
            };
            remove_fut = controller.remove_network(network_config);
            // Run server_provider forward so that it will process the remove request
            assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
            assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);
        }

        assert_variant!(exec.run_until_stalled(&mut remove_fut), Poll::Ready(Ok(Ok(()))));
        assert!(saved_networks.lookup(network_id).is_empty());
    }

    fn process_connect(
        exec: &mut fasync::Executor,
        sme_stream: &mut fidl_sme::ClientSmeRequestStream,
    ) {
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Connect {
                txn, ..
            }))) => {
                // Send connection response.
                let (_stream, ctrl) = txn.expect("connect txn unused")
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_finished(fidl_sme::ConnectResultCode::Success)
                    .expect("failed to send connection completion");
            }
        );
    }

    #[test]
    fn get_saved_networks_empty() {
        let saved_networks = vec![];
        let expected_configs = vec![];
        let expected_num_sends = 0;
        test_get_saved_networks(
            "get_saved_networks_empty",
            saved_networks,
            expected_configs,
            expected_num_sends,
        );
    }

    #[test]
    fn get_saved_network() {
        // save a network
        let network_id = NetworkIdentifier::new(b"foobar".to_vec(), SecurityType::Wpa2);
        let credential = Credential::Password(b"password".to_vec());
        let saved_networks = vec![(network_id.clone(), credential.clone())];

        let expected_id = network_id.into();
        let expected_credential = credential.into();
        let expected_configs = vec![fidl_policy::NetworkConfig {
            id: Some(expected_id),
            credential: Some(expected_credential),
        }];

        let expected_num_sends = 1;
        test_get_saved_networks(
            "get_saved_network",
            saved_networks,
            expected_configs,
            expected_num_sends,
        );
    }

    #[test]
    fn get_saved_networks_multiple_chunks() {
        // Save MAX_CONFIGS_PER_RESPONSE + 1 configs so that get_saved_networks should respond with
        // 2 chunks of responses plus one response with an empty vector.
        let mut saved_networks = vec![];
        let mut expected_configs = vec![];
        for index in 0..MAX_CONFIGS_PER_RESPONSE + 1 {
            // Create unique network config to be saved.
            let ssid = format!("some_config{}", index).into_bytes();
            let net_id = NetworkIdentifier::new(ssid.clone(), SecurityType::None);
            saved_networks.push((net_id, Credential::None));

            // Create corresponding FIDL value and add to list of expected configs/
            let ssid = format!("some_config{}", index).into_bytes();
            let net_id = fidl_policy::NetworkIdentifier {
                ssid: ssid,
                type_: fidl_policy::SecurityType::None,
            };
            let credential = fidl_policy::Credential::None(fidl_policy::Empty);
            let network_config =
                fidl_policy::NetworkConfig { id: Some(net_id), credential: Some(credential) };
            expected_configs.push(network_config);
        }

        let expected_num_sends = 2;
        test_get_saved_networks(
            "get_saved_networks_multiple_chunks",
            saved_networks,
            expected_configs,
            expected_num_sends,
        );
    }

    /// Test that get saved networks with the given saved networks
    /// test_id: the name of the test to create a unique persistent store for each test
    /// saved_configs: list of NetworkIdentifier and Credential pairs that are to be stored to the
    ///     SavedNetworksManager in the test.
    /// expected_configs: list of FIDL NetworkConfigs that we expect to get from get_saved_networks
    /// expected_num_sends: number of chunks of results we expect to get from get_saved_networks.
    ///     This is not counting the empty vector that signifies no more results.
    fn test_get_saved_networks(
        test_id: impl AsRef<str>,
        saved_configs: Vec<(NetworkIdentifier, Credential)>,
        expected_configs: Vec<fidl_policy::NetworkConfig>,
        expected_num_sends: usize,
    ) {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let temp_dir = TempDir::new().expect("failed to create temp dir");
        let path = temp_dir.path().join("networks.json");
        let tmp_path = temp_dir.path().join("tmp.json");
        let saved_networks = Arc::new(
            exec.run_singlethreaded(SavedNetworksManager::new_with_stash_or_paths(
                test_id, path, tmp_path,
            ))
            .expect("Failed to create a KnownEssStore"),
        );

        let (provider, requests) = create_proxy::<fidl_policy::ClientProviderMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");

        let (client, _sme_stream) = exec.run_singlethreaded(create_client());
        let (update_sender, _listener_updates) = mpsc::unbounded();

        let serve_fut =
            serve_provider_requests(client, update_sender, Arc::clone(&saved_networks), requests);
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Save the networks specified for this test.
        for (net_id, credential) in saved_configs {
            saved_networks.store(net_id, credential).expect("failed to store network");
        }

        // Request a new controller.
        let (controller, _update_stream) = request_controller(&provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue request to get the list of saved networks.
        let (iter, server) =
            fidl::endpoints::create_proxy::<fidl_policy::NetworkConfigIteratorMarker>()
                .expect("failed to create iterator");
        controller.get_saved_networks(server).expect("Failed to call get saved networks");

        // Get responses from iterator. Expect to see the specified number of responses with
        // results plus one response of an empty vector indicating the end of results.
        let mut saved_networks_results = vec![];
        for i in 0..expected_num_sends {
            let mut get_saved_fut = iter.get_next();
            assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
            let results = exec
                .run_singlethreaded(&mut get_saved_fut)
                .expect("Failed to get next chunk of saved networks results");
            // the size of received chunk should either be max chunk size or whatever is left
            // to receive in the last chunk
            if i < expected_num_sends - 1 {
                assert_eq!(results.len(), MAX_CONFIGS_PER_RESPONSE);
            } else {
                assert_eq!(results.len(), expected_configs.len() % MAX_CONFIGS_PER_RESPONSE);
            }
            saved_networks_results.extend(results);
        }
        let mut get_saved_end_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        let results = exec
            .run_singlethreaded(&mut get_saved_end_fut)
            .expect("Failed to get next chunk of saved networks results");
        assert!(results.is_empty());

        // check whether each network we saved is in the results and that nothing else is there.
        for network_config in &expected_configs {
            assert!(saved_networks_results.contains(&network_config));
        }
        assert_eq!(expected_configs.len(), saved_networks_results.len());
    }

    #[test]
    fn register_update_listener() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let mut test_values = test_setup("register_update_listener", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            test_values.saved_networks,
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a new controller.
        let (_controller, _update_stream) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut test_values.listener_updates.next()),
            Poll::Ready(Some(listener::Message::NewListener(_)))
        );
    }

    #[test]
    fn get_listener() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let (listener, requests) = create_proxy::<fidl_policy::ClientListenerMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");

        let (update_sender, mut listener_updates) = mpsc::unbounded();
        let serve_fut = serve_listener_requests(update_sender, requests);
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Register listener.
        let (update_sink, _update_stream) =
            create_request_stream::<fidl_policy::ClientStateUpdatesMarker>()
                .expect("failed to create ClientStateUpdates proxy");
        listener.get_listener(update_sink).expect("error getting listener");

        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut listener_updates.next()),
            Poll::Ready(Some(listener::Message::NewListener(_)))
        );
    }

    #[test]
    fn multiple_controllers_write_attempt() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup("multiple_controllers_write_attempt", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            test_values.saved_networks,
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a controller.
        let (controller1, _) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request another controller.
        let (controller2, _) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Ensure first controller is operable. Issue connect request.
        let connect_fut = controller1.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request from first controller. Verify success.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::Acknowledged))
        );

        // Ensure second controller is not operable. Issue connect request.
        let connect_fut = controller2.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request from second controller. Verify failure.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Err(Error::ClientChannelClosed(zx::Status::ALREADY_BOUND)))
        );

        // Drop first controller. A new controller can now take control.
        drop(controller1);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request another controller.
        let (controller3, _) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Ensure third controller is operable. Issue connect request.
        let connect_fut = controller3.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request from third controller. Verify success.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::Acknowledged))
        );
    }

    #[test]
    fn multiple_controllers_epitaph() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let test_values = test_setup("multiple_controllers_epitaph", &mut exec);
        let serve_fut = serve_provider_requests(
            test_values.client,
            test_values.update_sender,
            test_values.saved_networks,
            test_values.requests,
        );
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a controller.
        let (_controller1, _) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request another controller.
        let (controller2, _) = request_controller(&test_values.provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        let chan = controller2.into_channel().expect("error turning proxy into channel");
        let mut buffer = zx::MessageBuf::new();
        let epitaph_fut = chan.recv_msg(&mut buffer);
        pin_mut!(epitaph_fut);
        assert_variant!(exec.run_until_stalled(&mut epitaph_fut), Poll::Ready(Ok(_)));

        // Verify Epitaph was received.
        use fidl::encoding::{decode_transaction_header, Decodable, Decoder, EpitaphBody};
        let (header, tail) =
            decode_transaction_header(buffer.bytes()).expect("failed decoding header");
        let mut msg = Decodable::new_empty();
        Decoder::decode_into::<EpitaphBody>(&header, tail, &mut [], &mut msg)
            .expect("failed decoding body");
        assert_eq!(msg.error, zx::Status::ALREADY_BOUND);
        assert!(chan.is_closed());
    }

    #[test]
    fn no_client_interface() {
        let mut exec = fasync::Executor::new().expect("failed to create an executor");
        let stash_id = "no_client_interface";
        let saved_networks = exec.run_singlethreaded(create_network_store(stash_id));

        let (provider, requests) = create_proxy::<fidl_policy::ClientProviderMarker>()
            .expect("failed to create ClientProvider proxy");
        let requests = requests.into_stream().expect("failed to create stream");

        let client = Arc::new(Mutex::new(Client::new_empty()));
        let (update_sender, _listener_updates) = mpsc::unbounded();
        let serve_fut = serve_provider_requests(client, update_sender, saved_networks, requests);
        pin_mut!(serve_fut);

        // No request has been sent yet. Future should be idle.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Request a controller.
        let (controller, _) = request_controller(&provider);
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);

        // Issue connect request.
        let connect_fut = controller.connect(&mut fidl_policy::NetworkIdentifier {
            ssid: b"foobar".to_vec(),
            type_: fidl_policy::SecurityType::None,
        });
        pin_mut!(connect_fut);

        // Process connect request from first controller. Verify success.
        assert_variant!(exec.run_until_stalled(&mut serve_fut), Poll::Pending);
        assert_variant!(
            exec.run_until_stalled(&mut connect_fut),
            Poll::Ready(Ok(fidl_common::RequestStatus::RejectedNotSupported))
        );
    }

    // Gets a saved network config with a particular SSID, security type, and credential.
    // If there are more than one configs saved for the same SSID, security type, and credential,
    // the function will panic.
    fn get_config(
        saved_networks: Arc<SavedNetworksManager>,
        id: NetworkIdentifier,
        cred: Credential,
    ) -> Option<NetworkConfig> {
        let mut cfgs = saved_networks
            .lookup(id)
            .into_iter()
            .filter(|cfg| cfg.credential == cred)
            .collect::<Vec<_>>();
        // there should not be multiple configs with the same SSID, security type, and credential.
        assert!(cfgs.len() <= 1);
        cfgs.pop()
    }

    #[fasync::run_singlethreaded(test)]
    async fn get_correct_config() {
        let temp_dir = tempfile::TempDir::new().expect("failed to create temp dir");
        let path = temp_dir.path().join("networks.json");
        let tmp_path = temp_dir.path().join("tmp.json");
        let stash_id = "get_correct_config";
        let saved_networks = Arc::new(
            SavedNetworksManager::new_with_stash_or_paths(stash_id, path, tmp_path)
                .await
                .expect("Failed to create SavedNetworksManager"),
        );
        let network_id = NetworkIdentifier::new(b"foo".to_vec(), SecurityType::Wpa2);
        let cfg = NetworkConfig::new(
            network_id.clone(),
            Credential::Password(b"password".to_vec()),
            false,
            false,
        )
        .expect("Failed to create network config");

        saved_networks
            .store(network_id.clone(), Credential::Password(b"password".to_vec()))
            .expect("Failed to store network config");

        assert_eq!(
            Some(cfg),
            get_config(
                Arc::clone(&saved_networks),
                network_id,
                Credential::Password(b"password".to_vec())
            )
        );
        assert_eq!(
            None,
            get_config(
                Arc::clone(&saved_networks),
                NetworkIdentifier::new(b"foo".to_vec(), SecurityType::Wpa2),
                Credential::Password(b"not-saved".to_vec())
            )
        );
    }
}
