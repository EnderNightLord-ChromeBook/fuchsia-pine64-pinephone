// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/boot_log_checker/reboot_log_handler.h"

#include <lib/async/cpp/executor.h>
#include <lib/fit/result.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/developer/feedback/boot_log_checker/tests/stub_crash_reporter.h"
#include "src/developer/feedback/boot_log_checker/tests/stub_network_reachability_provider.h"
#include "src/developer/feedback/testing/cobalt_test_fixture.h"
#include "src/developer/feedback/testing/gpretty_printers.h"
#include "src/developer/feedback/testing/stubs/stub_cobalt_logger.h"
#include "src/developer/feedback/testing/stubs/stub_cobalt_logger_factory.h"
#include "src/developer/feedback/testing/unit_test_fixture.h"
#include "src/developer/feedback/utils/cobalt.h"
#include "src/developer/feedback/utils/cobalt_event.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

constexpr fit::result_state kError = fit::result_state::error;
constexpr fit::result_state kOk = fit::result_state::ok;
constexpr fit::result_state kPending = fit::result_state::pending;

struct TestParam {
  std::string test_name;
  std::string input_reboot_log;
  std::string output_crash_signature;
  std::optional<zx::duration> output_uptime;
  RebootReason output_cobalt_event_code;
};

class RebootLogHandlerTest : public UnitTestFixture,
                             public CobaltTestFixture,
                             public testing::WithParamInterface<TestParam> {
 public:
  RebootLogHandlerTest() : CobaltTestFixture(/*unit_test_fixture=*/this), executor_(dispatcher()) {}

 protected:
  void SetUpNetworkReachabilityProvider(
      std::unique_ptr<StubConnectivity> network_reachability_provider) {
    network_reachability_provider_ = std::move(network_reachability_provider);
    if (network_reachability_provider_) {
      InjectServiceProvider(network_reachability_provider_.get());
    }
  }

  void SetUpCrashReporter(std::unique_ptr<StubCrashReporter> crash_reporter) {
    crash_reporter_ = std::move(crash_reporter);
    if (crash_reporter_) {
      InjectServiceProvider(crash_reporter_.get());
    }
  }

  void WriteRebootLogContents(const std::string& contents =
                                  "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUPTIME (ms)\n74715002") {
    ASSERT_TRUE(tmp_dir_.NewTempFileWithData(contents, &reboot_log_path_));
  }

  fit::result<void> HandleRebootLog() {
    fit::result<void> result;
    executor_.schedule_task(
        feedback::HandleRebootLog(reboot_log_path_, dispatcher(), services())
            .then([&result](fit::result<void>& res) { result = std::move(res); }));
    RunLoopUntilIdle();
    return result;
  }

  fit::result<void> HandleRebootLogTriggerOnNetworkReachable() {
    auto result = HandleRebootLog();
    EXPECT_EQ(result.state(), kPending);

    network_reachability_provider_->TriggerOnNetworkReachable(true);
    // TODO(fxb/46216): remove delay.
    RunLoopFor(zx::sec(30));
    return result;
  }

 private:
  async::Executor executor_;

 protected:
  std::unique_ptr<StubConnectivity> network_reachability_provider_;
  std::unique_ptr<StubCrashReporter> crash_reporter_;
  std::string reboot_log_path_;

 private:
  files::ScopedTempDir tmp_dir_;
};

TEST_F(RebootLogHandlerTest, Succeed_NoRebootLog) {
  // We write nothing in |reboot_log_path_| so no file will exist at that path.
  EXPECT_EQ(HandleRebootLog().state(), kOk);
}

INSTANTIATE_TEST_SUITE_P(WithVariousRebootLogs, RebootLogHandlerTest,
                         ::testing::ValuesIn(std::vector<TestParam>({
                             {
                                 "KernelPanicCrashLog",
                                 "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUPTIME (ms)\n74715002",
                                 "fuchsia-kernel-panic",
                                 zx::msec(74715002),
                                 RebootReason::kKernelPanic,
                             },
                             {
                                 "KernelPanicCrashLogNoUptime",
                                 "ZIRCON REBOOT REASON (KERNEL PANIC)",
                                 "fuchsia-kernel-panic",
                                 std::nullopt,
                                 RebootReason::kKernelPanic,
                             },
                             {
                                 "KernelPanicCrashLogWrongUptime",
                                 "ZIRCON REBOOT REASON (KERNEL PANIC)\n\nUNRECOGNIZED",
                                 "fuchsia-kernel-panic",
                                 std::nullopt,
                                 RebootReason::kKernelPanic,
                             },
                             {
                                 "OutOfMemoryLog",
                                 "ZIRCON REBOOT REASON (OOM)\n\nUPTIME (ms)\n65487494",
                                 "fuchsia-oom",
                                 zx::msec(65487494),
                                 RebootReason::kOOM,
                             },
                             {
                                 "OutOfMemoryLogNoUptime",
                                 "ZIRCON REBOOT REASON (OOM)",
                                 "fuchsia-oom",
                                 std::nullopt,
                                 RebootReason::kOOM,
                             },
                             {
                                 "SoftwareWatchdogFired",
                                 "ZIRCON REBOOT REASON (SW WATCHDOG)",
                                 "fuchsia-sw-watchdog",
                                 std::nullopt,
                                 RebootReason::kSoftwareWatchdog,
                             },
                             {
                                 "HardwareWatchdogFired",
                                 "ZIRCON REBOOT REASON (HW WATCHDOG)",
                                 "fuchsia-hw-watchdog",
                                 std::nullopt,
                                 RebootReason::kHardwareWatchdog,
                             },
                             {
                                 "BrownoutPowerSupplyFailure",
                                 "ZIRCON REBOOT REASON (BROWNOUT)",
                                 "fuchsia-brownout",
                                 std::nullopt,
                                 RebootReason::kBrownout,
                             },
                             {
                                 "UnrecognizedCrashTypeInRebootLog",
                                 "UNRECOGNIZED CRASH TYPE",
                                 "fuchsia-kernel-panic",
                                 std::nullopt,
                                 RebootReason::kKernelPanic,
                             },
                         })),
                         [](const testing::TestParamInfo<TestParam>& info) {
                           return info.param.test_name;
                         });

TEST_P(RebootLogHandlerTest, Succeed) {
  const auto param = GetParam();

  WriteRebootLogContents(param.input_reboot_log);
  SetUpNetworkReachabilityProvider(std::make_unique<StubConnectivity>());
  SetUpCrashReporter(std::make_unique<StubCrashReporter>());
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  fit::result<void> result = HandleRebootLogTriggerOnNetworkReachable();
  EXPECT_EQ(result.state(), kOk);
  EXPECT_STREQ(crash_reporter_->crash_signature().c_str(), param.output_crash_signature.c_str());
  EXPECT_STREQ(crash_reporter_->reboot_log().c_str(), param.input_reboot_log.c_str());
  EXPECT_EQ(crash_reporter_->uptime(), param.output_uptime);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(param.output_cobalt_event_code)));
}

TEST_F(RebootLogHandlerTest, Pending_NetworkNotReachable) {
  WriteRebootLogContents();
  SetUpNetworkReachabilityProvider(std::make_unique<StubConnectivity>());
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  fit::result<void> result = HandleRebootLog();
  EXPECT_EQ(result.state(), kPending);

  network_reachability_provider_->TriggerOnNetworkReachable(false);
  RunLoopUntilIdle();
  EXPECT_EQ(result.state(), kPending);

  network_reachability_provider_->TriggerOnNetworkReachable(false);
  RunLoopUntilIdle();
  EXPECT_EQ(result.state(), kPending);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(RebootReason::kKernelPanic)));
}

TEST_F(RebootLogHandlerTest, Fail_EmptyRebootLog) {
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());
  WriteRebootLogContents("");
  EXPECT_EQ(HandleRebootLog().state(), kError);

  EXPECT_THAT(ReceivedCobaltEvents(), IsEmpty());
}

TEST_F(RebootLogHandlerTest, Fail_NetworkReachabilityProviderNotAvailable) {
  WriteRebootLogContents();
  SetUpNetworkReachabilityProvider(nullptr);
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  EXPECT_EQ(HandleRebootLog().state(), kError);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(RebootReason::kKernelPanic)));
}

TEST_F(RebootLogHandlerTest, Fail_NetworkReachabilityProviderClosesConnection) {
  WriteRebootLogContents();
  SetUpNetworkReachabilityProvider(std::make_unique<StubConnectivity>());
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  fit::result<void> result = HandleRebootLog();
  EXPECT_EQ(result.state(), kPending);

  network_reachability_provider_->CloseConnection();
  RunLoopUntilIdle();
  EXPECT_EQ(result.state(), kError);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(RebootReason::kKernelPanic)));
}

TEST_F(RebootLogHandlerTest, Fail_CrashReporterNotAvailable) {
  WriteRebootLogContents();
  SetUpNetworkReachabilityProvider(std::make_unique<StubConnectivity>());
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  fit::result<void> result = HandleRebootLogTriggerOnNetworkReachable();
  EXPECT_EQ(result.state(), kError);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(RebootReason::kKernelPanic)));
}

TEST_F(RebootLogHandlerTest, Fail_CrashReporterClosesConnection) {
  WriteRebootLogContents();
  SetUpNetworkReachabilityProvider(std::make_unique<StubConnectivity>());
  SetUpCrashReporter(std::make_unique<StubCrashReporterClosesConnection>());
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  fit::result<void> result = HandleRebootLogTriggerOnNetworkReachable();
  EXPECT_EQ(result.state(), kError);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(RebootReason::kKernelPanic)));
}

TEST_F(RebootLogHandlerTest, Fail_CrashReporterFailsToFile) {
  WriteRebootLogContents();
  SetUpNetworkReachabilityProvider(std::make_unique<StubConnectivity>());
  SetUpCrashReporter(std::make_unique<StubCrashReporterAlwaysReturnsError>());
  SetUpCobaltLoggerFactory(std::make_unique<StubCobaltLoggerFactory>());

  fit::result<void> result = HandleRebootLogTriggerOnNetworkReachable();
  EXPECT_EQ(result.state(), kError);

  EXPECT_THAT(ReceivedCobaltEvents(), ElementsAre(CobaltEvent(RebootReason::kKernelPanic)));
}

TEST_F(RebootLogHandlerTest, Fail_CallHandleTwice) {
  internal::RebootLogHandler handler(dispatcher(), services());
  handler.Handle("irrelevant");
  ASSERT_DEATH(handler.Handle("irrelevant"),
               testing::HasSubstr("Handle() is not intended to be called twice"));
}

}  // namespace
}  // namespace feedback
