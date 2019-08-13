// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_TESTAPP_TESTS_H_
#define SRC_COBALT_BIN_TESTAPP_TESTS_H_

#include <memory>
#include <sstream>
#include <string>

#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/synchronous_interface_ptr.h"
#include "lib/svc/cpp/services.h"
#include "lib/syslog/cpp/logger.h"
#include "src/cobalt/bin/testapp/cobalt_testapp_logger.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/strings/string_view.h"
#include "third_party/cobalt/util/clock.h"

namespace cobalt {
namespace testapp {

bool CheckMetricIds();

bool TestLogEvent(CobaltTestAppLogger* logger);

bool TestLogEventCount(CobaltTestAppLogger* logger);

bool TestLogElapsedTime(CobaltTestAppLogger* logger);

bool TestLogFrameRate(CobaltTestAppLogger* logger);

bool TestLogMemoryUsage(CobaltTestAppLogger* logger);

bool TestLogIntHistogram(CobaltTestAppLogger* logger);

bool TestLogCustomEvent(CobaltTestAppLogger* logger);

bool TestLogCobaltEvent(CobaltTestAppLogger* logger);

bool TestChannelFiltering(CobaltTestAppLogger* logger, uint32_t expect_more_than,
                          fuchsia::cobalt::ControllerSyncPtr* cobalt_controller,
                          uint32_t* num_added = nullptr);

// Tests of local aggregation.
//
// Each of these tests assumes that the EventAggregator has been updated with
// the ProjectContext of |logger|, but that the EventAggregator's
// AggregatedObservationHistoryStore is empty and that the LocalAggregateStore
// contains no aggregates. One way to ensure this is to reconnect to the Cobalt
// app immediately before running each of these tests.
//
// Each test logs some events for a locally aggregated report, generates
// locally aggregated observations for the current day index in UTC according to
// a system clock, and checks that the expected number of observations were
// generated. Each test then generates locally aggregated observations
// again, for the same day index, and checks that no observations were
// generated.
//
// In addition, TestLogEventWithAggregation attempts to log an event with an
// invalid event code and checks for failure.
bool TestLogEventWithAggregation(CobaltTestAppLogger* logger, util::ClockInterface* clock,
                                 fuchsia::cobalt::ControllerSyncPtr* cobalt_controller,
                                 const size_t backfill_days);

bool TestLogEventCountWithAggregation(CobaltTestAppLogger* logger, util::ClockInterface* clock,
                                      fuchsia::cobalt::ControllerSyncPtr* cobalt_controller,
                                      const size_t backfill_days);

bool TestLogElapsedTimeWithAggregation(CobaltTestAppLogger* logger, util::ClockInterface* clock,
                                       fuchsia::cobalt::ControllerSyncPtr* cobalt_controller,
                                       const size_t backfill_days);

}  // namespace testapp
}  // namespace cobalt

#endif  // SRC_COBALT_BIN_TESTAPP_TESTS_H_
