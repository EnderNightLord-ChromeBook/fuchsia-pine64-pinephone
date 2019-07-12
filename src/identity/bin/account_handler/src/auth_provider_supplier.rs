// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::format_err;
use fidl::endpoints::{create_endpoints, ClientEnd};
use fidl_fuchsia_auth::{AuthProviderMarker, Status};
use fidl_fuchsia_auth_account_internal::AccountHandlerContextProxy;
use futures::future::{ready as fready, FutureObj};
use token_manager::TokenManagerError;

/// A type capable of acquiring `AuthProvider` connections from components implementing the
/// `AuthProviderFactory` interface.
///
/// The functionality is delegated to the component that launched the AccountHandler, through
/// methods it implements on the `AccountHandlerContext` interface.
pub struct AuthProviderSupplier {
    /// The `AccountHandlerContext` interface supplied by the component that launched
    /// AccountHandler
    account_handler_context: AccountHandlerContextProxy,
}

impl AuthProviderSupplier {
    /// Creates a new `AuthProviderSupplier` from the supplied `AccountHandlerContextProxy`.
    pub fn new(account_handler_context: AccountHandlerContextProxy) -> Self {
        AuthProviderSupplier { account_handler_context }
    }
}

impl token_manager::AuthProviderSupplier for AuthProviderSupplier {
    /// Asynchronously creates an `AuthProvider` for the requested `auth_provider_type` and
    /// returns the `ClientEnd` for communication with it.
    fn get<'a>(
        &'a self,
        auth_provider_type: &'a str,
    ) -> FutureObj<'a, Result<ClientEnd<AuthProviderMarker>, TokenManagerError>> {
        let (client_end, server_end) = match create_endpoints() {
            Ok((client_end, server_end)) => (client_end, server_end),
            Err(err) => {
                let tm_err = TokenManagerError::new(Status::UnknownError).with_cause(err);
                return FutureObj::new(Box::new(fready(Err(tm_err))));
            }
        };

        FutureObj::new(Box::new(async move {
            match await!(self
                .account_handler_context
                .get_auth_provider(auth_provider_type, server_end))
            {
                Ok(fidl_fuchsia_auth_account::Status::Ok) => Ok(client_end),
                Ok(stat) => Err(TokenManagerError::new(Status::AuthProviderServiceUnavailable)
                    .with_cause(format_err!("AccountHandlerContext returned {:?}", stat))),
                Err(err) => Err(TokenManagerError::new(Status::UnknownError).with_cause(err)),
            }
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl::endpoints::{create_endpoints, ClientEnd, ServerEnd};
    use fidl_fuchsia_auth_account_internal::{
        AccountHandlerContextMarker, AccountHandlerContextRequest,
    };
    use fuchsia_async as fasync;
    use futures::TryStreamExt;
    use token_manager::AuthProviderSupplier as AuthProviderSupplierTrait;

    const TEST_AUTH_PROVIDER_TYPE: &str = "test_auth_provider";

    /// Runs a asynchronous test on the supplied executor to contruct a new AuthProviderSupplier
    /// and request TEST_AUTH_PROVIDER.
    fn do_get_test(
        mut executor: fasync::Executor,
        client_end: ClientEnd<AccountHandlerContextMarker>,
        expected_error: Option<Status>,
    ) {
        let proxy = client_end.into_proxy().unwrap();
        let auth_provider_supplier = AuthProviderSupplier::new(proxy);
        executor.run_singlethreaded(async move {
            let result = await!(auth_provider_supplier.get(TEST_AUTH_PROVIDER_TYPE));
            match expected_error {
                Some(status) => {
                    assert!(result.is_err());
                    assert_eq!(status, result.unwrap_err().status);
                }
                None => {
                    assert!(result.is_ok());
                }
            }
        });
    }

    /// Spawns a trivial task to respond to the first AccountHandlerContextRequest with the
    /// supplied status, only if the first request is a get request for TEST_AUTH_PROVIDER_TYPE
    fn spawn_account_handler_context_server(
        server_end: ServerEnd<AccountHandlerContextMarker>,
        status: fidl_fuchsia_auth_account::Status,
    ) {
        fasync::spawn(async move {
            let mut request_stream = server_end.into_stream().unwrap();
            // Only respond to the first received message, only when its of the intended type.
            if let Ok(Some(AccountHandlerContextRequest::GetAuthProvider {
                auth_provider_type,
                auth_provider: _,
                responder,
            })) = await!(request_stream.try_next())
            {
                if auth_provider_type == TEST_AUTH_PROVIDER_TYPE {
                    responder.send(status).expect("Failed to send test response");
                }
            }
        });
    }

    #[test]
    fn test_get_valid() {
        let executor = fasync::Executor::new().expect("Failed to create executor");
        let (client_end, server_end) = create_endpoints::<AccountHandlerContextMarker>().unwrap();

        spawn_account_handler_context_server(server_end, fidl_fuchsia_auth_account::Status::Ok);
        do_get_test(executor, client_end, None);
    }

    #[test]
    fn test_get_invalid() {
        let executor = fasync::Executor::new().expect("Failed to create executor");
        let (client_end, server_end) = create_endpoints::<AccountHandlerContextMarker>().unwrap();

        spawn_account_handler_context_server(
            server_end,
            fidl_fuchsia_auth_account::Status::NotFound,
        );
        do_get_test(executor, client_end, Some(Status::AuthProviderServiceUnavailable));
    }
}
