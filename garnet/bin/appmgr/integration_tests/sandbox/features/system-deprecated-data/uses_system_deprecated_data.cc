// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/appmgr/integration_tests/sandbox/namespace_test.h"

TEST_F(NamespaceTest, UsesSystemDeprecatedData) {
    ExpectExists("/system/data/test_file.txt");
}
