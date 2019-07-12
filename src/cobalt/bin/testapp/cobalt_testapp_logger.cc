// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/testapp/cobalt_testapp_logger.h"

#include <map>
#include <string>

#include "lib/syslog/cpp/logger.h"

namespace cobalt {
namespace testapp {

using fuchsia::cobalt::Status;

bool CobaltTestAppLogger::LogEvent(uint32_t metric_id, uint32_t index) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogEvent(metric_id, index, &status);
  FX_VLOGS(1) << "LogEvent(" << index << ") => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogEventCount(uint32_t metric_id, uint32_t index,
                                        const std::string& component,
                                        int64_t count) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogEventCount(metric_id, index, component, 0, count, &status);
  FX_VLOGS(1) << "LogEventCount(" << index << ") => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogEventCount() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogElapsedTime(uint32_t metric_id, uint32_t index,
                                         const std::string& component,
                                         int64_t elapsed_micros) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogElapsedTime(metric_id, index, component, elapsed_micros, &status);
  FX_VLOGS(1) << "LogElapsedTime() => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogElapsedTime() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogFrameRate(uint32_t metric_id, uint32_t index,
                                       const std::string& component,
                                       float fps) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogFrameRate(metric_id, index, component, fps, &status);
  FX_VLOGS(1) << "LogFrameRate() => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogFrameRate() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogMemoryUsage(uint32_t metric_id, uint32_t index,
                                         const std::string& component,
                                         int64_t bytes) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogMemoryUsage(metric_id, index, component, bytes, &status);
  FX_VLOGS(1) << "LogMemoryUsage() => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogMemoryUsage() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogString(uint32_t metric_id,
                                    const std::string& val) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogString(metric_id, val, &status);
  FX_VLOGS(1) << "LogString(" << val << ") => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogString() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogTimer(uint32_t metric_id, uint32_t start_time,
                                   uint32_t end_time,
                                   const std::string& timer_id,
                                   uint32_t timeout_s) {
  Status status = Status::INTERNAL_ERROR;
  logger_->StartTimer(metric_id, 0, "", timer_id, start_time, timeout_s,
                      &status);
  logger_->EndTimer(timer_id, end_time, timeout_s, &status);

  FX_VLOGS(1) << "LogTimer("
              << "timer_id:" << timer_id << ", start_time:" << start_time
              << ", end_time:" << end_time << ") => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogTimer() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogIntHistogram(
    uint32_t metric_id, uint32_t index, const std::string& component,
    const std::map<uint32_t, uint64_t>& histogram_map) {
  Status status = Status::INTERNAL_ERROR;
  std::vector<fuchsia::cobalt::HistogramBucket> histogram;
  for (auto it = histogram_map.begin(); histogram_map.end() != it; it++) {
    fuchsia::cobalt::HistogramBucket entry;
    entry.index = it->first;
    entry.count = it->second;
    histogram.push_back(std::move(entry));
  }

  logger_->LogIntHistogram(metric_id, index, component, std::move(histogram),
                           &status);
  FX_VLOGS(1) << "LogIntHistogram() => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogIntHistogram() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogCobaltEvent(fuchsia::cobalt::CobaltEvent event) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogCobaltEvent(std::move(event), &status);

  FX_VLOGS(1) << "LogCobaltEvent() => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogCobaltEvent() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogStringPair(uint32_t metric_id,
                                        const std::string& part0,
                                        const std::string& val0,
                                        const std::string& part1,
                                        const std::string& val1) {
  Status status = Status::INTERNAL_ERROR;
  std::vector<fuchsia::cobalt::CustomEventValue> parts(2);
  parts.at(0).dimension_name = part0;
  parts.at(0).value.set_string_value(val0);
  parts.at(1).dimension_name = part1;
  parts.at(1).value.set_string_value(val1);
  logger_->LogCustomEvent(metric_id, std::move(parts), &status);
  FX_VLOGS(1) << "LogCustomEvent(" << val0 << ", " << val1 << ") => "
              << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogCustomEvent() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogCustomMetricsTestProto(
    uint32_t metric_id, const std::string& query_val,
    const int64_t wait_time_val, const uint32_t response_code_val) {
  Status status = Status::INTERNAL_ERROR;
  std::vector<fuchsia::cobalt::CustomEventValue> parts(3);
  parts.at(0).dimension_name = "query";
  parts.at(0).value.set_string_value(query_val);
  parts.at(1).dimension_name = "wait_time_ms";
  parts.at(1).value.set_int_value(wait_time_val);
  parts.at(2).dimension_name = "response_code";
  parts.at(2).value.set_index_value(response_code_val);
  logger_->LogCustomEvent(metric_id, std::move(parts), &status);
  FX_VLOGS(1) << "LogCustomEvent(query=" << query_val
              << ", wait_time_ms=" << wait_time_val
              << ", response_code=" << response_code_val << ") => "
              << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogCustomEvent() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::CheckForSuccessfulSend() {
  if (!use_network_) {
    FX_LOGS(INFO) << "Not using the network because --no_network_for_testing "
                     "was passed.";
    return true;
  }

  bool send_success = false;
  FX_VLOGS(1) << "Invoking RequestSendSoon() now...";
  (*cobalt_controller_)->RequestSendSoon(&send_success);
  FX_VLOGS(1) << "RequestSendSoon => " << send_success;
  return send_success;
}

std::string StatusToString(fuchsia::cobalt::Status status) {
  switch (status) {
    case Status::OK:
      return "OK";
    case Status::INVALID_ARGUMENTS:
      return "INVALID_ARGUMENTS";
    case Status::EVENT_TOO_BIG:
      return "EVENT_TOO_BIG";
    case Status::BUFFER_FULL:
      return "BUFFER_FULL";
    case Status::INTERNAL_ERROR:
      return "INTERNAL_ERROR";
  }
}

}  // namespace testapp
}  // namespace cobalt
