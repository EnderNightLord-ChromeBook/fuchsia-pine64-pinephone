// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(async_await)]

use {fuchsia_async as fasync, test_utils};

#[fasync::run_singlethreaded(test)]
async fn collections() {
    let events = vec![
        "Creating children",
        "Found children (coll:a,coll:b)",
        "Binding to children",
        "Triggered a",
        "Triggered b",
        "Destroying child",
        "Binding to destroyed child",
        "Found children (coll:b)",
        "Recreating and binding to child",
        "Triggered a",
        "Found children (coll:a,coll:b)",
        "Done",
    ];
    let mut out = String::new();
    events.iter().for_each(|s| {
        out.push_str(&format!("{}\n", s));
    });
    test_utils::launch_and_wait_for_msg(
        "fuchsia-pkg://fuchsia.com/collections_integration_test#meta/component_manager.cmx"
            .to_string(),
        Some(vec![
            "fuchsia-pkg://fuchsia.com/collections_integration_test#meta/collection_realm.cm"
                .to_string(),
        ]),
        out,
    );
}
