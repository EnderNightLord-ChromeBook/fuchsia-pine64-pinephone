// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::account_handler_context::AccountHandlerContext;
use account_common::{AccountManagerError, LocalAccountId, ResultExt as AccountResultExt};
use failure::{format_err, ResultExt};
use fidl::endpoints::{ClientEnd, RequestStream};
use fidl_fuchsia_auth_account::{Lifetime, Status};
use fidl_fuchsia_auth_account_internal::{
    AccountHandlerContextMarker, AccountHandlerContextRequestStream, AccountHandlerControlMarker,
    AccountHandlerControlProxy,
};
use fidl_fuchsia_sys::EnvironmentControllerProxy;
use fuchsia_async as fasync;
use fuchsia_component::client::App;
use fuchsia_component::fuchsia_single_component_package_url;
use fuchsia_component::server::ServiceFs;
use fuchsia_zircon as zx;
use futures::prelude::*;
use log::{error, info, warn};
use std::sync::Arc;

/// The url used to launch new AccountHandler component instances.
const ACCOUNT_HANDLER_URL: &str = fuchsia_single_component_package_url!("account_handler");

/// The url used to launch new ephemeral AccountHandler component instances.
const ACCOUNT_HANDLER_EPHEMERAL_URL: &str =
    "fuchsia-pkg://fuchsia.com/account_handler#meta/account_handler_ephemeral.cmx";

/// The information necessary to maintain a connection to an AccountHandler component instance.
pub struct AccountHandlerConnection {
    /// An `App` object for the launched AccountHandler.
    ///
    /// Note: This must remain in scope for the component to remain running, but never needs to be
    /// read.
    _app: App,

    /// An `EnvController` object for the launched AccountHandler.
    ///
    /// Note: This must remain in scope for the component to remain running, but never needs to be
    /// read.
    _env_controller: EnvironmentControllerProxy,

    /// The lifetime of the account.
    lifetime: Lifetime,

    /// A `Proxy` connected to the AccountHandlerControl interface on the launched AccountHandler.
    proxy: AccountHandlerControlProxy,
}

impl AccountHandlerConnection {
    /// Launches a new AccountHandler component instance and establishes a connection to its
    /// control channel.
    ///
    /// Note: This method is not public. Callers should use one of the factory methods that also
    /// sends an initialization call to the AccountHandler after connection, such as `load_account`
    /// or `create_account`
    fn new(account_id: LocalAccountId, lifetime: Lifetime) -> Result<Self, AccountManagerError> {
        let account_handler_url = if lifetime == Lifetime::Ephemeral {
            info!("Launching new ephemeral AccountHandler instance");
            ACCOUNT_HANDLER_EPHEMERAL_URL
        } else {
            info!("Launching new persistent AccountHandler instance");
            ACCOUNT_HANDLER_URL
        };

        // Note: The combination of component URL and environment label determines the location of
        // the data directory for the launched component. It is critical that the label is unique
        // and stable per-account, which we achieve through using the local account id as
        // the environment name.
        let env_label = account_id.to_canonical_string();
        let mut fs_for_account_handler = ServiceFs::new();
        let (env_controller, app) = fs_for_account_handler
            .launch_component_in_nested_environment(
                account_handler_url.to_string(),
                None,
                env_label.as_ref(),
            )
            .context("Failed to start launcher")
            .account_manager_status(Status::IoError)?;
        fasync::spawn(fs_for_account_handler.collect());
        let proxy = app
            .connect_to_service::<AccountHandlerControlMarker>()
            .context("Failed to connect to AccountHandlerControl")
            .account_manager_status(Status::IoError)?;

        Ok(AccountHandlerConnection { _app: app, _env_controller: env_controller, lifetime, proxy })
    }

    /// Returns the lifetime of the account that this handler manages.
    pub fn get_lifetime(&self) -> &Lifetime {
        &self.lifetime
    }

    /// Creates a new `AccountHandlerContext` channel, spawns a task to handle requests received on
    /// this channel using the supplied `AccountHandlerContext`, and returns the `ClientEnd`.
    fn spawn_context_channel(
        context: Arc<AccountHandlerContext>,
    ) -> Result<ClientEnd<AccountHandlerContextMarker>, AccountManagerError> {
        let (server_chan, client_chan) = zx::Channel::create()
            .context("Failed to create channel")
            .account_manager_status(Status::IoError)?;
        let server_async_chan = fasync::Channel::from_channel(server_chan)
            .context("Failed to create async channel")
            .account_manager_status(Status::IoError)?;
        let request_stream = AccountHandlerContextRequestStream::from_channel(server_async_chan);
        let context_clone = Arc::clone(&context);
        fasync::spawn(async move {
            context_clone.handle_requests_from_stream(request_stream).await
                .unwrap_or_else(|err| error!("Error handling AccountHandlerContext: {:?}", err))
        });
        Ok(ClientEnd::new(client_chan))
    }

    /// Launches a new AccountHandler component instance, establishes a connection to its control
    /// channel, and requests that it loads an existing account.
    pub async fn load_account(
        account_id: &LocalAccountId,
        context: Arc<AccountHandlerContext>,
    ) -> Result<Self, AccountManagerError> {
        let connection = Self::new(account_id.clone(), Lifetime::Persistent)?;
        let context_client_end = Self::spawn_context_channel(context)?;
        match connection
            .proxy
            .load_account(context_client_end, account_id.clone().as_mut().into()).await
        .account_manager_status(Status::IoError)?
        {
            Status::Ok => Ok(connection),
            stat => Err(AccountManagerError::new(stat)
                .with_cause(format_err!("Error loading existing account"))),
        }
    }

    /// Launches a new AccountHandler component instance, establishes a connection to its control
    /// channel, and requests that it create a new account.
    pub async fn create_account(
        context: Arc<AccountHandlerContext>,
        lifetime: Lifetime,
    ) -> Result<(Self, LocalAccountId), AccountManagerError> {
        let account_id = LocalAccountId::new(rand::random::<u64>());
        let connection = Self::new(account_id.clone(), lifetime)?;
        let context_client_end = Self::spawn_context_channel(context)?;

        match connection
            .proxy()
            .create_account(context_client_end, account_id.clone().as_mut().into()).await
        .account_manager_status(Status::IoError)?
        {
            Status::Ok => {
                // TODO(jsankey): Longer term, local ID may need to be related to the global ID
                // rather than just a random number.
                Ok((connection, account_id))
            }
            status => Err(AccountManagerError::new(status)
                .with_cause(format_err!("Account handler returned error"))),
        }
    }

    /// Returns the AccountHandlerControlProxy for this connection
    pub fn proxy(&self) -> &AccountHandlerControlProxy {
        &self.proxy
    }

    /// Requests that the AccountHandler component instance terminate gracefully.
    ///
    /// Any subsequent operations that attempt to use `proxy()` will fail after this call. The
    /// resources associated with the connection when only be freed once the
    /// `AccountHandlerConnection` is dropped.
    pub async fn terminate(&self) {
        let mut event_stream = self.proxy.take_event_stream();
        if let Err(err) = self.proxy.terminate() {
            warn!("Error gracefully terminating account handler {:?}", err);
        } else {
            while let Ok(Some(_)) = event_stream.try_next().await {}
            info!("Gracefully terminated AccountHandler instance");
        }
    }
}
