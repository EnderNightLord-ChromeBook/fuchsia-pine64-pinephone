// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_BOOT_LOG_CHECKER_REBOOT_LOG_HANDLER_H_
#define SRC_DEVELOPER_FEEDBACK_BOOT_LOG_CHECKER_REBOOT_LOG_HANDLER_H_

#include <fuchsia/cobalt/cpp/fidl.h>
#include <fuchsia/feedback/cpp/fidl.h>
#include <fuchsia/net/cpp/fidl.h>
#include <lib/fit/bridge.h>
#include <lib/fit/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>
#include <sys/stat.h>

#include <memory>
#include <string>

#include "src/developer/feedback/utils/cobalt.h"
#include "src/lib/fsl/vmo/sized_vmo.h"
#include "src/lib/fxl/functional/cancelable_callback.h"

namespace feedback {

// Checks the presence of a reboot log at |filepath|. If present, wait for the network to be
// reachable and hands it off to the crash analyzer as today we only stow something in the reboot
// log in case of OOM or kernel panic.
//
// fuchsia.net.Connectivity, fuchsia.feedback.CrashReporter and fuchsia.cobalt.LoggerFactory are
// expected to be in |services|.
fit::promise<void> HandleRebootLog(const std::string& filepath, async_dispatcher_t* dispatcher,
                                   std::shared_ptr<sys::ServiceDirectory> services);

namespace internal {

// The information extracted from the reboot log.
struct RebootInfo {
  RebootReason reboot_reason;
  std::optional<zx::duration> uptime;
};

// Wraps around fuchsia.net.Connectivity, fuchsia.feedback.CrashReporter, fuchsia.cobalt.Logger and
// fuchsia.cobalt.LoggerFactory to handle establishing the connection, losing the connection,
// waiting for the callback, etc.
//
// Handle() is expected to be called only once.
class RebootLogHandler {
 public:
  RebootLogHandler(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services);

  fit::promise<void> Handle(const std::string& filepath);

 private:
  fit::promise<void> WaitForNetworkToBeReachable();
  fit::promise<void> FileCrashReport(RebootInfo info);

  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  // Enforces the one-shot nature of Handle().
  bool has_called_handle_ = false;

  fsl::SizedVmo reboot_log_;

  fuchsia::net::ConnectivityPtr connectivity_;
  fit::bridge<void> network_reachable_;

  fuchsia::feedback::CrashReporterPtr crash_reporter_;
  fit::bridge<void> crash_reporting_done_;
  // We wrap the delayed task we post on the async loop to delay the crash reporting in a
  // CancelableClosure so we can cancel it if we are done another way.
  fxl::CancelableClosure delayed_crash_reporting_;

  Cobalt cobalt_;
  fit::bridge<void> cobalt_logging_done_;
};

}  // namespace internal
}  // namespace feedback

#endif  // SRC_DEVELOPER_FEEDBACK_BOOT_LOG_CHECKER_REBOOT_LOG_HANDLER_H_
