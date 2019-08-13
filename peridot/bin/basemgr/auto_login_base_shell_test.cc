// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/testing/cpp/fidl.h>
#include <lib/modular/testing/cpp/test_harness_builder.h>
#include <lib/modular_test_harness/cpp/test_harness_fixture.h>
#include <sdk/lib/sys/cpp/service_directory.h>
#include <sdk/lib/sys/cpp/testing/test_with_environment.h>

class AutoLoginBaseShellTest : public modular::testing::TestHarnessFixture {};

// Use auto_login_base_shell and expect the user to be automatically logged into
// session shell.
TEST_F(AutoLoginBaseShellTest, AutoLoginBaseShellLaunchesSessionShell) {
  constexpr char kAutoLoginBaseShellUrl[] =
      "fuchsia-pkg://fuchsia.com/auto_login_base_shell#meta/"
      "auto_login_base_shell.cmx";

  fuchsia::modular::testing::TestHarnessSpec spec;
  spec.mutable_basemgr_config()->mutable_base_shell()->mutable_app_config()->set_url(
      kAutoLoginBaseShellUrl);

  modular_testing::TestHarnessBuilder builder(std::move(spec));
  // Listen for session shell interception.
  bool intercepted = false;
  builder.InterceptSessionShell(
      [&intercepted](
          fuchsia::sys::StartupInfo startup_info,
          fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent> component) {
        intercepted = true;
      });
  builder.BuildAndRun(test_harness());

  RunLoopUntil([&] { return intercepted; });
}
