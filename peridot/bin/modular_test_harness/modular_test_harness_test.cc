// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/testing/cpp/fidl.h>
#include <lib/modular_test_harness/cpp/test_harness_fixture.h>
#include <sdk/lib/sys/cpp/service_directory.h>
#include <sdk/lib/sys/cpp/testing/test_with_environment.h>

class ModularTestHarnessTest : public modular::testing::TestHarnessFixture {};

// Ensure that the TestHarnessFixture is able to launch the modular runtime.
TEST_F(ModularTestHarnessTest, SimpleSuccess) {
  constexpr char kFakeBaseShellUrl[] =
      "fuchsia-pkg://example.com/FAKE_BASE_SHELL_PKG/fake_base_shell.cmx";

  // Setup base shell interception.
  fuchsia::modular::testing::InterceptSpec shell_intercept_spec;
  shell_intercept_spec.set_component_url(kFakeBaseShellUrl);

  fuchsia::modular::testing::TestHarnessSpec spec;
  spec.mutable_basemgr_config()->mutable_base_shell()->mutable_app_config()->set_url(
      kFakeBaseShellUrl);
  spec.mutable_components_to_intercept()->push_back(std::move(shell_intercept_spec));

  // Listen for base shell interception.
  bool intercepted = false;

  test_harness().events().OnNewComponent =
      [&](fuchsia::sys::StartupInfo startup_info,
          fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent> component) {
        ASSERT_EQ(kFakeBaseShellUrl, startup_info.launch_info.url);
        intercepted = true;
      };

  test_harness()->Run(std::move(spec));

  RunLoopUntil([&] { return intercepted; });
}
