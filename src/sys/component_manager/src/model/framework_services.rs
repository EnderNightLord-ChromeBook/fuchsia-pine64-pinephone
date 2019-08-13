// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::clonable_error::ClonableError,
    crate::model::*,
    cm_rust::CapabilityPath,
    failure::{Error, Fail},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::future::BoxFuture,
    lazy_static::lazy_static,
    log::*,
    std::{convert::TryInto, sync::Arc},
};

lazy_static! {
    pub static ref FRAMEWORK_SERVICES: Vec<&'static CapabilityPath> = vec![&*REALM_SERVICE];
    pub static ref REALM_SERVICE: CapabilityPath = "/svc/fuchsia.sys2.Realm".try_into().unwrap();
}

/// Represents component manager's framework_services environment, which provides
/// framework_services services to components. An framework_services service is a service that is
/// offered by component manager itself, which any component may use.
///
/// The following framework_services services are currently implemented:
/// - fuchsia.sys2.Realm
pub trait FrameworkServiceHost: Send + Sync {
    /// Serve the framework_services `fuchsia.sys2.Realm` service for `realm` over `stream`.
    fn serve_realm_service(
        &self,
        model: Model,
        realm: Arc<Realm>,
        stream: fsys::RealmRequestStream,
    ) -> BoxFuture<Result<(), FrameworkServiceError>>;
}

/// Errors produced by `FrameworkServiceHost`.
#[derive(Debug, Fail, Clone)]
pub enum FrameworkServiceError {
    #[fail(display = "framework_services service unsupported: {}", path)]
    ServiceUnsupported { path: String },
    #[fail(display = "framework_services service `{}` failed: {}", path, err)]
    ServiceError {
        path: String,
        #[fail(cause)]
        err: ClonableError,
    },
}

impl FrameworkServiceError {
    pub fn service_unsupported(path: impl Into<String>) -> FrameworkServiceError {
        FrameworkServiceError::ServiceUnsupported { path: path.into() }
    }
    pub fn service_error(path: impl Into<String>, err: impl Into<Error>) -> FrameworkServiceError {
        FrameworkServiceError::ServiceError { path: path.into(), err: err.into().into() }
    }
}

impl dyn FrameworkServiceHost {
    /// Serve the framework_services service denoted by `path` over `server_chan`.
    pub async fn serve<'a>(
        framework_services: Arc<dyn FrameworkServiceHost>,
        model: Model,
        realm: Arc<Realm>,
        path: CapabilityPath,
        server_chan: zx::Channel,
    ) -> Result<(), FrameworkServiceError> {
        if path == *REALM_SERVICE {
            let stream = ServerEnd::<fsys::RealmMarker>::new(server_chan)
                .into_stream()
                .expect("could not convert channel into stream");
            fasync::spawn(async move {
                if let Err(e) =
                    framework_services.serve_realm_service(model.clone(), realm, stream).await
                {
                    // TODO: Set an epitaph to indicate this was an unexpected error.
                    warn!("serve_realm failed: {}", e);
                }
            });
            Ok(())
        } else {
            Err(FrameworkServiceError::service_unsupported(path.to_string()))
        }
    }
}
