// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/cobalt/cpp/fidl.h>
#include <fuchsia/sysinfo/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fsl/syslogger/init.h>
#include <lib/zx/channel.h>
#include <stdlib.h>
#include <trace-provider/provider.h>
#include <zircon/boot/image.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "lib/syslog/cpp/logger.h"
#include "src/cobalt/bin/app/cobalt_app.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/split_string.h"
#include "third_party/cobalt/encoder/file_observation_store.h"
#include "third_party/cobalt/encoder/memory_observation_store.h"
#include "third_party/cobalt/util/posix_file_system.h"

// Command-line flags

// Used to override kScheduleIntervalDefault;
constexpr fxl::StringView kScheduleIntervalSecondsFlagName =
    "schedule_interval_seconds";

constexpr fxl::StringView kInitialIntervalSecondsFlagName =
    "initial_interval_seconds";

// Used to override kMinIntervalDefault;
constexpr fxl::StringView kMinIntervalSecondsFlagName = "min_interval_seconds";

// Used to override kEventAggregatorBackfillDaysDefault
constexpr fxl::StringView kEventAggregatorBackfillDaysFlagName =
    "event_aggregator_backfill_days";

// Used to override kStartEventAggregatorWorkerDefault
constexpr fxl::StringView kStartEventAggregatorWorkerFlagName =
    "start_event_aggregator_worker";

constexpr fxl::StringView kUseMemoryObservationStore =
    "use_memory_observation_store";

constexpr fxl::StringView kMaxBytesTotalFlagName =
    "max_bytes_per_observation_store";

// We want to only upload every hour. This is the interval that will be
// approached by the uploader.
const std::chrono::hours kScheduleIntervalDefault(1);

// We start uploading every minute and exponentially back off until we reach 1
// hour.
const std::chrono::minutes kInitialIntervalDefault(1);

// We send Observations to the Shuffler more frequently than kScheduleInterval
// under some circumstances, namely, if there is memory pressure or if we
// are explicitly asked to do so via the RequestSendSoon() method. This value
// is a safety parameter. We do not make two attempts within a period of this
// specified length.
const std::chrono::seconds kMinIntervalDefault(10);

// The EventAggregator looks back 2 days, in addition to the previous day, to
// make sure that all locally aggregated observations have been generated.
const size_t kEventAggregatorBackfillDaysDefault(2);

// We normally start the EventAggregator's worker thread after constructing the
// EventAggregator.
constexpr bool kStartEventAggregatorWorkerDefault(true);

// This is a set of channel names that are known to have DEBUG semantics.
//
// - devhost is the channel for development devices.
// - fishfood-release is the main fishfood channel.
// - qa-daily is a daily QA release.
const std::vector<std::string> kDebugChannels({"devhost", "fishfood-release",
                                               "qa-daily"});

// ReadBoardName returns the board name of the currently running device.
//
// At the time of this writing, this will either be 'pc' for x86 devices, or an
// appropriate board name for ARM devices (hikey960, sherlock, qemu).
//
// This uses the fuchsia-sysinfo fidl service to read the board_name field out
// of the ZBI. This string will never exceed a length of 32.
//
// If the reading of the board name fails for any reason, this will return "".
std::string ReadBoardName() {
  const char kSysInfoPath[] = "/dev/misc/sysinfo";
  const int fd = open(kSysInfoPath, O_RDWR);
  if (fd < 0) {
    return "";
  }

  // Connect to the fuchsia-sysinfo service through the file system API.
  zx::channel channel;
  zx_status_t status =
      fdio_get_service_handle(fd, channel.reset_and_get_address());
  if (status != ZX_OK) {
    return "";
  }

  // Read the board name out of the ZBI.
  char board_name[ZBI_BOARD_NAME_LEN];
  size_t actual_size = 0;
  zx_status_t fidl_status = fuchsia_sysinfo_DeviceGetBoardName(
      channel.get(), &status, board_name, sizeof(board_name), &actual_size);
  if (fidl_status != ZX_OK || status != ZX_OK) {
    return "";
  }

  return std::string(board_name, actual_size);
}

std::string ReadBuildInfo(std::string value) {
  std::ifstream file("/config/build-info/" + value);
  if (file.is_open()) {
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  } else {
    return "";
  }
}

int main(int argc, const char** argv) {
  // Parse the flags.
  const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fxl::SetLogSettingsFromCommandLine(command_line);
  fsl::InitLoggerFromCommandLine(command_line, {"cobalt", "fidl_service"});

  // Parse the schedule_interval_seconds flag.
  std::chrono::seconds schedule_interval =
      std::chrono::duration_cast<std::chrono::seconds>(
          kScheduleIntervalDefault);
  std::chrono::seconds initial_interval =
      std::chrono::duration_cast<std::chrono::seconds>(kInitialIntervalDefault);
  std::string flag_value;
  if (command_line.GetOptionValue(kScheduleIntervalSecondsFlagName,
                                  &flag_value)) {
    int num_seconds = std::stoi(flag_value);
    if (num_seconds > 0) {
      schedule_interval = std::chrono::seconds(num_seconds);
      // Set initial_interval, it can still be overridden by a flag.
      initial_interval = std::chrono::seconds(num_seconds);
    }
  }

  // Parse the initial_interval_seconds flag.
  flag_value.clear();
  if (command_line.GetOptionValue(kInitialIntervalSecondsFlagName,
                                  &flag_value)) {
    int num_seconds = std::stoi(flag_value);
    if (num_seconds > 0) {
      initial_interval = std::chrono::seconds(num_seconds);
    }
  }

  // Parse the min_interval_seconds flag.
  std::chrono::seconds min_interval =
      std::chrono::duration_cast<std::chrono::seconds>(kMinIntervalDefault);
  flag_value.clear();
  if (command_line.GetOptionValue(kMinIntervalSecondsFlagName, &flag_value)) {
    int num_seconds = std::stoi(flag_value);
    // We allow min_interval = 0.
    if (num_seconds >= 0) {
      min_interval = std::chrono::seconds(num_seconds);
    }
  }

  // Parse the event_aggregator_backfill_days flag.
  size_t event_aggregator_backfill_days = kEventAggregatorBackfillDaysDefault;
  flag_value.clear();
  if (command_line.GetOptionValue(kEventAggregatorBackfillDaysFlagName,
                                  &flag_value)) {
    int num_days = std::stoi(flag_value);
    // We allow num_days = 0.
    if (num_days >= 0) {
      event_aggregator_backfill_days = num_days;
    }
  }

  // Parse the start_event_aggregator_worker flag.
  bool start_event_aggregator_worker = kStartEventAggregatorWorkerDefault;
  flag_value.clear();
  if (command_line.GetOptionValue(kStartEventAggregatorWorkerFlagName,
                                  &flag_value)) {
    if (flag_value == "true") {
      start_event_aggregator_worker = true;
    } else if (flag_value == "false") {
      start_event_aggregator_worker = false;
    }
  }

  bool use_memory_observation_store =
      command_line.HasOption(kUseMemoryObservationStore);

  // Parse the max_bytes_per_observation_store
  size_t max_bytes_per_observation_store = 1024 * 1024;  // 1 MiB
  flag_value.clear();
  if (command_line.GetOptionValue(kMaxBytesTotalFlagName, &flag_value)) {
    int num_bytes = std::stoi(flag_value);
    if (num_bytes > 0) {
      max_bytes_per_observation_store = num_bytes;
    }
  }

  FX_LOGS(INFO) << "Cobalt is starting with the following parameters: "
                << "schedule_interval=" << schedule_interval.count()
                << " seconds, min_interval=" << min_interval.count()
                << " seconds, initial_interval=" << initial_interval.count()
                << " seconds, max_bytes_per_observation_store="
                << max_bytes_per_observation_store
                << ", event_aggregator_backfill_days="
                << event_aggregator_backfill_days
                << ", start_event_aggregator_worker="
                << start_event_aggregator_worker << ".";

  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher(),
                                              "cobalt_fidl_provider");
  cobalt::CobaltApp app(
      loop.dispatcher(), schedule_interval, min_interval, initial_interval,
      event_aggregator_backfill_days, start_event_aggregator_worker,
      use_memory_observation_store, max_bytes_per_observation_store,
      ReadBuildInfo("product"), ReadBoardName(), ReadBuildInfo("version"),
      kDebugChannels);
  loop.Run();
  return 0;
}
