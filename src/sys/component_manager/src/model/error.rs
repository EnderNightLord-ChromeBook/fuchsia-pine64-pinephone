// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::*,
    failure::{Error, Fail},
};

/// Errors produced by `Model`.
#[derive(Debug, Fail)]
pub enum ModelError {
    #[fail(display = "component instance not found with moniker {}", moniker)]
    InstanceNotFound { moniker: AbsoluteMoniker },
    #[fail(display = "component instance with moniker {} already exists", moniker)]
    InstanceAlreadyExists { moniker: AbsoluteMoniker },
    #[fail(display = "component collection not found with name {}", name)]
    CollectionNotFound { name: String },
    #[fail(display = "{} is not supported", feature)]
    Unsupported { feature: String },
    #[fail(display = "component declaration invalid")]
    ComponentInvalid,
    #[fail(display = "component manifest invalid")]
    ManifestInvalid {
        url: String,
        #[fail(cause)]
        err: Error,
    },
    #[fail(display = "namespace creation failed: {}", err)]
    NamespaceCreationFailed {
        #[fail(cause)]
        err: Error,
    },
    #[fail(display = "resolver error")]
    ResolverError {
        #[fail(cause)]
        err: ResolverError,
    },
    #[fail(display = "runner error")]
    RunnerError {
        #[fail(cause)]
        err: RunnerError,
    },
    #[fail(display = "ambient error")]
    AmbientError {
        #[fail(cause)]
        err: AmbientError,
    },
    #[fail(display = "capability discovery error")]
    CapabilityDiscoveryError {
        #[fail(cause)]
        err: Error,
    },
    #[fail(display = "add entry error")]
    AddEntryError { moniker: AbsoluteMoniker, entry_name: String },
}

impl ModelError {
    pub fn instance_not_found(moniker: AbsoluteMoniker) -> ModelError {
        ModelError::InstanceNotFound { moniker }
    }

    pub fn instance_already_exists(moniker: AbsoluteMoniker) -> ModelError {
        ModelError::InstanceAlreadyExists { moniker }
    }

    pub fn collection_not_found(name: impl Into<String>) -> ModelError {
        ModelError::CollectionNotFound { name: name.into() }
    }

    pub fn unsupported(feature: impl Into<String>) -> ModelError {
        ModelError::Unsupported { feature: feature.into() }
    }

    pub fn namespace_creation_failed(err: impl Into<Error>) -> ModelError {
        ModelError::NamespaceCreationFailed { err: err.into() }
    }

    pub fn manifest_invalid(url: impl Into<String>, err: impl Into<Error>) -> ModelError {
        ModelError::ManifestInvalid { url: url.into(), err: err.into() }
    }

    pub fn capability_discovery_error(err: impl Into<Error>) -> ModelError {
        ModelError::CapabilityDiscoveryError { err: err.into() }
    }

    pub fn add_entry_error(moniker: AbsoluteMoniker, entry_name: impl Into<String>) -> ModelError {
        ModelError::AddEntryError { moniker, entry_name: entry_name.into() }
    }
}

impl From<ResolverError> for ModelError {
    fn from(err: ResolverError) -> Self {
        ModelError::ResolverError { err }
    }
}

impl From<RunnerError> for ModelError {
    fn from(err: RunnerError) -> Self {
        ModelError::RunnerError { err }
    }
}

impl From<AmbientError> for ModelError {
    fn from(err: AmbientError) -> Self {
        ModelError::AmbientError { err }
    }
}
