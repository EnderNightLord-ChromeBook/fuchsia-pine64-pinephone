// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/synchronization/dispatcher_checker.h"

#include <lib/async/cpp/task.h>

#include "gtest/gtest.h"
#include "src/ledger/bin/testing/test_with_environment.h"

namespace ledger {

using DispatcherCheckerTest = ledger::TestWithEnvironment;

TEST_F(DispatcherCheckerTest, Trivial) {
  DispatcherChecker checker;
  EXPECT_TRUE(checker.IsCreationDispatcherCurrent());
}

TEST_F(DispatcherCheckerTest, MainLoopIsDefault) {
  DispatcherChecker checker;
  async::PostTask(environment_.dispatcher(),
                  [&checker] { EXPECT_TRUE(checker.IsCreationDispatcherCurrent()); });
  RunLoopUntilIdle();
}

// TODO(kerneis): enable once https://fuchsia-review.googlesource.com/c/fuchsia/+/299828 has rolled.
TEST_F(DispatcherCheckerTest, DISABLED_IoLoopIsNotDefault) {
  DispatcherChecker checker;
  async::PostTask(environment_.io_dispatcher(),
                  [&checker] { EXPECT_FALSE(checker.IsCreationDispatcherCurrent()); });
  RunLoopUntilIdle();
}

}  // namespace ledger
