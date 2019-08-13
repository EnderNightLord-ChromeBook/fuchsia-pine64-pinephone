// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]
#![recursion_limit = "256"]

mod controller;
mod filter;
mod serialization;
mod session;
mod state;
mod utils;

use failure::Error;
use fidl_fuchsia_ledger_cloud::CloudProviderRequestStream;
use fidl_fuchsia_ledger_cloud_test::CloudControllerFactoryRequestStream;
use fuchsia_async as fasync;
use fuchsia_component::server::ServiceFs;
use futures::future::LocalFutureObj;
use futures::prelude::*;
use std::cell::RefCell;
use std::rc::Rc;

use crate::controller::CloudControllerFactory;
use crate::session::{CloudSession, CloudSessionShared};
use crate::state::Cloud;

/// An factory for instances of the cloud provider sharing the same
/// data storage.
struct CloudFactory(Rc<RefCell<Cloud>>);

impl CloudFactory {
    /// Create a factory with empty storage.
    fn new() -> CloudFactory {
        CloudFactory(Rc::new(RefCell::new(Cloud::new())))
    }

    /// Returns a future that handles the request stream.
    fn spawn(&self, stream: CloudProviderRequestStream) -> LocalFutureObj<'static, ()> {
        CloudSession::new(Rc::new(CloudSessionShared::new(Rc::clone(&self.0))), stream).run()
    }
}

enum IncomingServices {
    CloudProvider(CloudProviderRequestStream),
    CloudControllerFactory(CloudControllerFactoryRequestStream),
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let cloud_factory = CloudFactory::new();
    let mut fs = ServiceFs::new_local();
    fs.dir("svc")
        .add_fidl_service(IncomingServices::CloudProvider)
        .add_fidl_service(IncomingServices::CloudControllerFactory);

    fs.take_and_serve_directory_handle()?;

    let fut = fs.for_each_concurrent(None, |req| match req {
        IncomingServices::CloudProvider(stream) => cloud_factory.spawn(stream),
        IncomingServices::CloudControllerFactory(stream) => {
            CloudControllerFactory::new(stream).run()
        }
    });

    fut.await;
    Ok(())
}
