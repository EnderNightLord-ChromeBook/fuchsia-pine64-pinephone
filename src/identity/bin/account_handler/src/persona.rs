// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::account_handler::AccountHandler;
use crate::common::AccountLifetime;
use crate::inspect;
use crate::TokenManager;
use account_common::{LocalAccountId, LocalPersonaId};
use failure::Error;
use fidl::encoding::OutOfLine;
use fidl::endpoints::{ClientEnd, ServerEnd};
use fidl_fuchsia_auth::{
    AuthChangeGranularity, AuthState, AuthenticationContextProviderProxy, TokenManagerMarker,
};
use fidl_fuchsia_auth_account::{
    AuthListenerMarker, Lifetime, PersonaRequest, PersonaRequestStream, Status,
};
use fuchsia_inspect::{Node, NumericProperty};
use futures::prelude::*;
use identity_common::{cancel_or, TaskGroup, TaskGroupCancel};
use log::{error, warn};
use std::sync::Arc;
use token_manager::TokenManagerContext;

/// The context that a particular request to a Persona should be executed in, capturing
/// information that was supplied upon creation of the channel.
pub struct PersonaContext {
    /// An `AuthenticationContextProviderProxy` capable of generating new `AuthenticationUiContext`
    /// channels.
    pub auth_ui_context_provider: AuthenticationContextProviderProxy,
}

/// Information about one of the Personae withing the Account that this AccountHandler instance is
/// responsible for.
///
/// This state is only available once the Handler has been initialized to a particular account via
/// the AccountHandlerControl channel.
// TODO(dnordstrom): Factor out items that are accessed by both account and persona into its own
// type so they don't need to be individually copied or Arc-wrapped. Here, `token_manager`,
// `lifetime` and `account_id` are candidates.
pub struct Persona {
    /// A device-local identifier for this persona.
    id: LocalPersonaId,

    /// The device-local identitier that this persona is a facet of.
    _account_id: LocalAccountId,

    /// Lifetime for this persona's account (ephemeral or persistent with a path).
    lifetime: Arc<AccountLifetime>,

    /// The token manager to be used for authentication token requests.
    token_manager: Arc<TokenManager>,

    /// Collection of tasks that are using this instance.
    task_group: TaskGroup,

    /// Helper for outputting persona information via fuchsia_inspect.
    inspect: inspect::Persona,
}

impl Persona {
    /// Returns a task group which can be used to spawn and cancel tasks that use this instance.
    pub fn task_group(&self) -> &TaskGroup {
        &self.task_group
    }

    /// Constructs a new Persona.
    pub fn new(
        id: LocalPersonaId,
        account_id: LocalAccountId,
        lifetime: Arc<AccountLifetime>,
        token_manager: Arc<TokenManager>,
        task_group: TaskGroup,
        inspect_parent: &Node,
    ) -> Persona {
        let persona_inspect = inspect::Persona::new(inspect_parent, &id);
        Self {
            id,
            _account_id: account_id,
            lifetime,
            token_manager,
            task_group,
            inspect: persona_inspect,
        }
    }

    /// Returns the device-local identifier for this persona.
    pub fn id(&self) -> &LocalPersonaId {
        &self.id
    }

    /// Asynchronously handles the supplied stream of `PersonaRequest` messages.
    pub async fn handle_requests_from_stream<'a>(
        &'a self,
        context: &'a PersonaContext,
        mut stream: PersonaRequestStream,
        cancel: TaskGroupCancel,
    ) -> Result<(), Error> {
        self.inspect.open_client_channels.add(1);
        scopeguard::defer!(self.inspect.open_client_channels.subtract(1));
        while let Some(result) = cancel_or(&cancel, stream.try_next()).await {
            if let Some(request) = result? {
                self.handle_request(context, request).await?;
            } else {
                break;
            }
        }
        Ok(())
    }

    /// Dispatches a `PersonaRequest` message to the appropriate handler method
    /// based on its type.
    pub async fn handle_request<'a>(
        &'a self,
        context: &'a PersonaContext,
        req: PersonaRequest,
    ) -> Result<(), fidl::Error> {
        match req {
            PersonaRequest::GetLifetime { responder } => {
                let response = self.get_lifetime();
                responder.send(response)?;
            }
            PersonaRequest::GetAuthState { responder } => {
                let mut response = self.get_auth_state();
                responder.send(response.0, response.1.as_mut().map(OutOfLine))?;
            }
            PersonaRequest::RegisterAuthListener {
                listener,
                initial_state,
                granularity,
                responder,
            } => {
                let response = self.register_auth_listener(listener, initial_state, granularity);
                responder.send(response)?;
            }
            PersonaRequest::GetTokenManager { application_url, token_manager, responder } => {
                let response =
                    self.get_token_manager(context, application_url, token_manager).await;
                responder.send(response)?;
            }
        }
        Ok(())
    }

    fn get_lifetime(&self) -> Lifetime {
        Lifetime::from(self.lifetime.as_ref())
    }

    fn get_auth_state(&self) -> (Status, Option<AuthState>) {
        // TODO(jsankey): Return real authentication state once authenticators exist to create it.
        (Status::Ok, Some(AccountHandler::DEFAULT_AUTH_STATE))
    }

    fn register_auth_listener(
        &self,
        _listener: ClientEnd<AuthListenerMarker>,
        _initial_state: bool,
        _granularity: AuthChangeGranularity,
    ) -> Status {
        // TODO(jsankey): Implement this method.
        warn!("RegisterAuthListener not yet implemented");
        Status::InternalError
    }

    async fn get_token_manager<'a>(
        &'a self,
        context: &'a PersonaContext,
        application_url: String,
        token_manager_server_end: ServerEnd<TokenManagerMarker>,
    ) -> Status {
        let token_manager_clone = Arc::clone(&self.token_manager);
        let token_manager_context = TokenManagerContext {
            application_url,
            auth_ui_context_provider: context.auth_ui_context_provider.clone(),
        };
        match token_manager_server_end.into_stream() {
            Ok(stream) => {
                match self.token_manager.task_group().spawn(|cancel| async move {
                    token_manager_clone.handle_requests_from_stream(
                        &token_manager_context,
                        stream,
                        cancel
                    ).await
                    .unwrap_or_else(|e| error!("Error handling TokenManager channel {:?}", e))
                }).await {
                    Err(_) => Status::RemovalInProgress,
                    Ok(()) => Status::Ok,
                }
            }
            Err(e) => {
                error!("Error opening TokenManager channel {:?}", e);
                Status::IoError
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::auth_provider_supplier::AuthProviderSupplier;
    use crate::test_util::*;
    use fidl::endpoints::create_endpoints;
    use fidl_fuchsia_auth::AuthenticationContextProviderMarker;
    use fidl_fuchsia_auth_account::{PersonaMarker, PersonaProxy};
    use fidl_fuchsia_auth_account_internal::AccountHandlerContextMarker;
    use fuchsia_async as fasync;
    use fuchsia_inspect::Inspector;
    use std::path::PathBuf;

    /// Type to hold the common state require during construction of test objects and execution
    /// of a test, including an async executor and a temporary location in the filesystem.
    struct Test {
        _location: TempLocation,
        token_manager: Arc<TokenManager>,
    }

    impl Test {
        fn new() -> Test {
            let location = TempLocation::new();
            let (account_handler_context_client_end, _) =
                create_endpoints::<AccountHandlerContextMarker>().unwrap();
            let token_manager = Arc::new(
                TokenManager::new(
                    &location.test_path(),
                    AuthProviderSupplier::new(
                        account_handler_context_client_end.into_proxy().unwrap(),
                    ),
                    TaskGroup::new(),
                )
                .unwrap(),
            );
            Test { _location: location, token_manager }
        }

        fn create_persona(&self) -> Persona {
            let inspector = Inspector::new();
            Persona::new(
                TEST_PERSONA_ID.clone(),
                TEST_ACCOUNT_ID.clone(),
                Arc::new(AccountLifetime::Persistent { account_dir: PathBuf::from("/nowhere") }),
                Arc::clone(&self.token_manager),
                TaskGroup::new(),
                inspector.root(),
            )
        }

        fn create_ephemeral_persona(&self) -> Persona {
            let inspector = Inspector::new();
            Persona::new(
                TEST_PERSONA_ID.clone(),
                TEST_ACCOUNT_ID.clone(),
                Arc::new(AccountLifetime::Ephemeral),
                Arc::clone(&self.token_manager),
                TaskGroup::new(),
                inspector.root(),
            )
        }

        async fn run<TestFn, Fut>(&mut self, test_object: Persona, test_fn: TestFn)
        where
            TestFn: FnOnce(PersonaProxy) -> Fut,
            Fut: Future<Output = Result<(), Error>>,
        {
            let (persona_client_end, persona_server_end) =
                create_endpoints::<PersonaMarker>().unwrap();
            let persona_proxy = persona_client_end.into_proxy().unwrap();
            let request_stream = persona_server_end.into_stream().unwrap();

            let (ui_context_provider_client_end, _) =
                create_endpoints::<AuthenticationContextProviderMarker>().unwrap();
            let persona_context = PersonaContext {
                auth_ui_context_provider: ui_context_provider_client_end.into_proxy().unwrap(),
            };

            let task_group = TaskGroup::new();
            task_group.spawn(|cancel| async move {
                test_object.handle_requests_from_stream(
                    &persona_context,
                    request_stream,
                    cancel
                ).await
                .unwrap_or_else(|err| panic!("Fatal error handling test request: {:?}", err));
            },).await
            .expect("Unable to spawn task");
            test_fn(persona_proxy).await.expect("Failed running test fn.")
        }
    }

    #[fasync::run_until_stalled(test)]
    async fn test_id() {
        let test = Test::new();
        assert_eq!(test.create_persona().id(), &*TEST_PERSONA_ID);
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_lifetime_persistent() {
        let test = Test::new();
        assert_eq!(test.create_persona().get_lifetime(), Lifetime::Persistent);
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_lifetime_ephemeral() {
        let test = Test::new();
        assert_eq!(test.create_ephemeral_persona().get_lifetime(), Lifetime::Ephemeral);
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_auth_state() {
        let mut test = Test::new();
        test.run(test.create_persona(), |proxy| async move {
            assert_eq!(
                proxy.get_auth_state().await?,
                (Status::Ok, Some(Box::new(AccountHandler::DEFAULT_AUTH_STATE)))
            );
            Ok(())
        }).await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_register_auth_listener() {
        let mut test = Test::new();
        test.run(test.create_persona(), |proxy| async move {
            let (auth_listener_client_end, _) = create_endpoints().unwrap();
            assert_eq!(
                proxy.register_auth_listener(
                    auth_listener_client_end,
                    true, /* include initial state */
                    &mut AuthChangeGranularity { summary_changes: true }
                ).await?,
                Status::InternalError
            );
            Ok(())
        }).await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_token_manager() {
        let mut test = Test::new();
        test.run(test.create_persona(), |proxy| async move {
            let (token_manager_client_end, token_manager_server_end) = create_endpoints().unwrap();
            assert_eq!(
                proxy.get_token_manager(&TEST_APPLICATION_URL, token_manager_server_end).await?,
                Status::Ok
            );

            // The token manager channel should now be usable.
            let token_manager_proxy = token_manager_client_end.into_proxy().unwrap();
            let mut app_config = create_dummy_app_config();
            assert_eq!(
                token_manager_proxy.list_profile_ids(&mut app_config).await?,
                (fidl_fuchsia_auth::Status::Ok, vec![])
            );

            Ok(())
        }).await;
    }
}
