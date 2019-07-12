// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_TESTING_TEST_WITH_ENVIRONMENT_H_
#define SRC_LEDGER_BIN_TESTING_TEST_WITH_ENVIRONMENT_H_

#include <lib/fit/function.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/component_context_provider.h>

#include "src/ledger/bin/environment/environment.h"
#include "src/lib/fxl/macros.h"

namespace ledger {

class TestWithEnvironment : public gtest::TestLoopFixture {
 public:
  TestWithEnvironment();

 protected:
  // Runs the given test code in a coroutine.
  ::testing::AssertionResult RunInCoroutine(
      fit::function<void(coroutine::CoroutineHandler*)> run_test, zx::duration delay = zx::sec(0));

  sys::testing::ComponentContextProvider component_context_provider_;
  std::unique_ptr<async::LoopInterface> io_loop_interface_;
  Environment environment_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(TestWithEnvironment);
};

}  // namespace ledger

#endif  // SRC_LEDGER_BIN_TESTING_TEST_WITH_ENVIRONMENT_H_
