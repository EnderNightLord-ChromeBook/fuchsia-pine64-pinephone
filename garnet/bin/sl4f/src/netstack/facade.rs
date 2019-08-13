// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::Error;
use fidl_fuchsia_net_stack::{StackMarker, StackProxy};
use fuchsia_component as app;
use serde_json::{to_value, Value};

/// Perform Netstack operations.
///
/// Note this object is shared among all threads created by server.
///
#[derive(Debug)]
pub struct NetstackFacade {
    stack: StackProxy,
}

impl NetstackFacade {
    pub fn new() -> NetstackFacade {
        NetstackFacade {
            stack: app::client::connect_to_service::<StackMarker>()
                .expect("failed to connect to netstack"),
        }
    }

    pub async fn list_interfaces(&self) -> Result<Value, Error> {
        // TODO(eyalsoha): Include more than just the paths.
        let interface_list = self.stack.list_interfaces().await?;
        let names = interface_list.into_iter().map(|x| x.properties.topopath).collect::<Vec<_>>();
        Ok(to_value(names)?)
    }
}
