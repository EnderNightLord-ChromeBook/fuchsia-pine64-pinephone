// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The cobalt system metrics collection daemon uses cobalt to log system metrics
// on a regular basis.
#include "src/cobalt/bin/system-metrics/system_metrics_daemon.h"

#include <assert.h>
#include <fcntl.h>

#include <chrono>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

#include <fuchsia/cobalt/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/resource.h>
#include <trace/event.h>
#include <zircon/status.h>

#include "lib/syslog/cpp/logger.h"
#include "src/cobalt/bin/system-metrics/cpu_stats_fetcher_impl.h"
#include "src/cobalt/bin/system-metrics/memory_stats_fetcher_impl.h"
#include "src/cobalt/bin/system-metrics/metrics_registry.cb.h"
#include "src/cobalt/bin/system-metrics/temperature_fetcher_impl.h"
#include "src/cobalt/bin/utils/clock.h"
#include "src/cobalt/bin/utils/status_utils.h"
#include "src/lib/cobalt/cpp/cobalt_event_builder.h"

using cobalt::CobaltEventBuilder;
using cobalt::StatusToString;
using cobalt::TemperatureFetchStatus;
using fuchsia::cobalt::CobaltEvent;
using fuchsia::cobalt::HistogramBucket;
using fuchsia::cobalt::Logger_Sync;
using fuchsia_system_metrics::FuchsiaLifetimeEventsMetricDimensionEvents;
using fuchsia_system_metrics::FuchsiaMemoryExperimental2MetricDimensionMemoryBreakdown;
using fuchsia_system_metrics::FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot;
using fuchsia_system_metrics::FuchsiaMemoryExperimentalMetricDimensionMemoryBreakdown;
using fuchsia_system_metrics::FuchsiaUpPingMetricDimensionUptime;
using fuchsia_system_metrics::FuchsiaUptimeMetricDimensionUptimeRange;
using std::chrono::steady_clock;

namespace {
// Given a number of seconds, return the number of seconds before the next
// multiple of 1 hour.
std::chrono::seconds SecondsBeforeNextHour(std::chrono::seconds uptime) {
  return std::chrono::seconds(3600 - (uptime.count() % 3600));
}
}  // namespace

SystemMetricsDaemon::SystemMetricsDaemon(async_dispatcher_t* dispatcher,
                                         sys::ComponentContext* context)
    : SystemMetricsDaemon(
          dispatcher, context, nullptr,
          std::unique_ptr<cobalt::SteadyClock>(new cobalt::RealSteadyClock()),
          std::unique_ptr<cobalt::MemoryStatsFetcher>(new cobalt::MemoryStatsFetcherImpl()),
          std::unique_ptr<cobalt::CpuStatsFetcher>(new cobalt::CpuStatsFetcherImpl()),
          std::unique_ptr<cobalt::TemperatureFetcher>(new cobalt::TemperatureFetcherImpl())) {
  InitializeLogger();
}

SystemMetricsDaemon::SystemMetricsDaemon(
    async_dispatcher_t* dispatcher, sys::ComponentContext* context,
    fuchsia::cobalt::Logger_Sync* logger, std::unique_ptr<cobalt::SteadyClock> clock,
    std::unique_ptr<cobalt::MemoryStatsFetcher> memory_stats_fetcher,
    std::unique_ptr<cobalt::CpuStatsFetcher> cpu_stats_fetcher,
    std::unique_ptr<cobalt::TemperatureFetcher> temperature_fetcher)
    : dispatcher_(dispatcher),
      context_(context),
      logger_(logger),
      start_time_(clock->Now()),
      clock_(std::move(clock)),
      memory_stats_fetcher_(std::move(memory_stats_fetcher)),
      cpu_stats_fetcher_(std::move(cpu_stats_fetcher)),
      temperature_fetcher_(std::move(temperature_fetcher)) {}

void SystemMetricsDaemon::StartLogging() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::StartLogging");
  // We keep gathering metrics until this process is terminated.
  RepeatedlyLogUpPingAndLifeTimeEvents();
  RepeatedlyLogUptime();
  RepeatedlyLogCpuUsage();
  RepeatedlyLogMemoryUsage();
  LogTemperatureIfSupported(1 /*remaining_attempts*/);
}

void SystemMetricsDaemon::RepeatedlyLogUpPingAndLifeTimeEvents() {
  std::chrono::seconds seconds_to_sleep = LogUpPingAndLifeTimeEvents();
  async::PostDelayedTask(
      dispatcher_, [this]() { RepeatedlyLogUpPingAndLifeTimeEvents(); },
      zx::sec(seconds_to_sleep.count() + 5));
  return;
}

void SystemMetricsDaemon::RepeatedlyLogUptime() {
  std::chrono::seconds seconds_to_sleep = LogFuchsiaUptime();
  async::PostDelayedTask(
      dispatcher_, [this]() { RepeatedlyLogUptime(); }, zx::sec(seconds_to_sleep.count()));
  return;
}

void SystemMetricsDaemon::RepeatedlyLogCpuUsage() {
  std::chrono::seconds seconds_to_sleep = LogCpuUsage();
  async::PostDelayedTask(
      dispatcher_, [this]() { RepeatedlyLogCpuUsage(); }, zx::sec(seconds_to_sleep.count()));
  return;
}

void SystemMetricsDaemon::RepeatedlyLogMemoryUsage() {
  std::chrono::seconds seconds_to_sleep = LogMemoryUsage();
  async::PostDelayedTask(
      dispatcher_, [this]() { RepeatedlyLogMemoryUsage(); }, zx::sec(seconds_to_sleep.count()));
  return;
}

void SystemMetricsDaemon::LogTemperatureIfSupported(int remaining_attempts) {
  int32_t temperature;
  TemperatureFetchStatus status = temperature_fetcher_->FetchTemperature(&temperature);
  switch (status) {
    case TemperatureFetchStatus::NOT_SUPPORTED:
      FX_LOGS(INFO) << "Stop further attempt to read or log temperature as it is not supported.";
      return;
    case TemperatureFetchStatus::SUCCEED:
      RepeatedlyLogTemperature();
      return;
    case TemperatureFetchStatus::FAIL:
      if (remaining_attempts > 0) {
        FX_LOGS(INFO) << "Failed to fetch device temperature. Try again in 1 minute.";
        async::PostDelayedTask(
            dispatcher_,
            [this, remaining_attempts]() { LogTemperatureIfSupported(remaining_attempts - 1); },
            zx::sec(60));
      } else {
        FX_LOGS(INFO) << "Exceeded the number of attempts to check for temperature support."
                      << "Stop further attempt to read or log temperature.";
      }
      return;
  }
}

void SystemMetricsDaemon::RepeatedlyLogTemperature() {
  std::chrono::seconds seconds_to_sleep = LogTemperature();
  async::PostDelayedTask(
      dispatcher_, [this]() { RepeatedlyLogTemperature(); }, zx::sec(seconds_to_sleep.count()));
  return;
}

std::chrono::seconds SystemMetricsDaemon::GetUpTime() {
  // Note(rudominer) We are using the startime of the SystemMetricsDaemon
  // as a proxy for the system start time. This is fine as long as we don't
  // start seeing systematic restarts of the SystemMetricsDaemon. If that
  // starts happening we should look into how to capture actual boot time.
  auto now = clock_->Now();
  return std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
}

std::chrono::seconds SystemMetricsDaemon::LogUpPingAndLifeTimeEvents() {
  std::chrono::seconds uptime = GetUpTime();
  return std::min(LogFuchsiaUpPing(uptime), LogFuchsiaLifetimeEvents());
}

std::chrono::seconds SystemMetricsDaemon::LogFuchsiaUpPing(std::chrono::seconds uptime) {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogFuchsiaUpPing");

  typedef FuchsiaUpPingMetricDimensionUptime Uptime;

  // We always log that we are |Up|.
  // If |uptime| is at least one minute we log that we are |UpOneMinute|.
  // If |uptime| is at least ten minutes we log that we are |UpTenMinutes|.
  // If |uptime| is at least one hour we log that we are |UpOneHour|.
  // If |uptime| is at least 12 hours we log that we are |UpTwelveHours|.
  // If |uptime| is at least 24 hours we log that we are |UpOneDay|.
  //
  // To understand the logic of this function it is important to note that
  // the events we are logging are intended to take advantage of Cobalt's
  // local aggregation feature. Thus, for example, although we log the
  // |Up| event many times throughout a calendar day, only a single
  // Observation per day will be sent from the device to the Cobalt backend
  // indicating that this device was "Up" during the day.

  if (!logger_) {
    FX_LOGS(ERROR) << "No logger present. Reconnecting...";
    InitializeLogger();
    // Something went wrong. Pause for 5 minutes.
    return std::chrono::minutes(5);
  }

  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  // Always log that we are "Up".
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::Up,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::minutes(1)) {
    // If we have been up for less than a minute, come back here after it
    // has been a minute.
    return std::chrono::minutes(1) - uptime;
  }
  // Log UpOneMinute
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpOneMinute,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::minutes(10)) {
    // If we have been up for less than 10 minutes, come back here after it
    // has been 10 minutes.
    return std::chrono::minutes(10) - uptime;
  }
  // Log UpTenMinutes
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpTenMinutes,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::hours(1)) {
    // If we have been up for less than an hour, come back here after it has
    // has been an hour.
    return std::chrono::hours(1) - uptime;
  }
  // Log UpOneHour
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpOneHour,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::hours(12)) {
    // If we have been up for less than 12 hours, come back here after *one*
    // hour. Notice this time we don't wait 12 hours to come back. The reason
    // is that it may be close to the end of the day. When the new day starts
    // we want to come back in a reasonable amount of time (we consider
    // one hour to be reasonable) so that we can log the earlier events
    // in the new day.
    return std::chrono::hours(1);
  }
  // Log UpTwelveHours.
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpTwelveHours,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::hours(24)) {
    // As above, come back in one hour.
    return std::chrono::hours(1);
  }
  // Log UpOneDay.
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpOneDay,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::hours(72)) {
    return std::chrono::hours(1);
  }
  // Log UpThreeDays.
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpThreeDays,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  if (uptime < std::chrono::hours(144)) {
    return std::chrono::hours(1);
  }
  // Log UpSixDays.
  status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (ReinitializeIfPeerClosed(
      logger_->LogEvent(fuchsia_system_metrics::kFuchsiaUpPingMetricId,
                        Uptime::UpSixDays,
                        &status)) != ZX_OK) {
    return std::chrono::minutes(5);
  }
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
  }
  // As above, come back in one hour.
  return std::chrono::hours(1);
}

std::chrono::seconds SystemMetricsDaemon::LogFuchsiaUptime() {
  std::chrono::seconds uptime = GetUpTime();
  if (!logger_) {
    FX_LOGS(ERROR) << "No logger present. Reconnecting...";
    InitializeLogger();
    // Something went wrong. Pause for 5 minutes.
    return std::chrono::minutes(5);
  }
  auto up_hours = std::chrono::duration_cast<std::chrono::hours>(uptime).count();
  uint32_t event_code = (up_hours < 336)
                            ? FuchsiaUptimeMetricDimensionUptimeRange::LessThanTwoWeeks
                            : event_code = FuchsiaUptimeMetricDimensionUptimeRange::TwoWeeksOrMore;

  fuchsia::cobalt::Status status;
  ReinitializeIfPeerClosed(
      logger_->LogElapsedTime(fuchsia_system_metrics::kFuchsiaUptimeMetricId,
                              event_code,
                              "",
                              up_hours,
                              &status));
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogCobaltEvent() returned status=" << StatusToString(status);
  }
  // Schedule a call of this function for the next multiple of an hour.
  return SecondsBeforeNextHour(uptime);
}

std::chrono::seconds SystemMetricsDaemon::LogFuchsiaLifetimeEvents() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogFuchsiaLifetimeEvents");
  if (!logger_) {
    FX_LOGS(ERROR) << "No logger present. Reconnecting...";
    InitializeLogger();
    // Something went wrong. Pause for 5 minutes.
    return std::chrono::minutes(5);
  }

  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  if (!boot_reported_) {
    ReinitializeIfPeerClosed(
        logger_->LogEvent(fuchsia_system_metrics::kFuchsiaLifetimeEventsMetricId,
                          FuchsiaLifetimeEventsMetricDimensionEvents::Boot,
                          &status));
    if (status != fuchsia::cobalt::Status::OK) {
      FX_LOGS(ERROR) << "LogEvent() returned status=" << StatusToString(status);
    } else {
      boot_reported_ = true;
    }
  }
  return std::chrono::seconds::max();
}

std::chrono::seconds SystemMetricsDaemon::LogCpuUsage() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogCpuUsage");
  if (!logger_) {
    FX_LOGS(ERROR) << "No logger present. Reconnecting...";
    InitializeLogger();
    return std::chrono::minutes(1);
  }
  double cpu_percentage;
  if (!cpu_stats_fetcher_->FetchCpuPercentage(&cpu_percentage)) {
    return std::chrono::minutes(1);
  }
  cpu_percentages_.push_back(cpu_percentage);
  if (cpu_percentages_.size() == 60) {  // Flush every minute.
    LogCpuPercentagesToCobalt();
    // Drop the data even if logging does not succeed.
    cpu_percentages_.clear();
  }
  return std::chrono::seconds(1);
}

void SystemMetricsDaemon::LogCpuPercentagesToCobalt() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogCpuPercentagesToCobalt");
  std::vector<CobaltEvent> events;
  auto builder =
      CobaltEventBuilder(fuchsia_system_metrics::kFuchsiaCpuPercentageExperimentalMetricId);
  for (unsigned i = 0; i < cpu_percentages_.size(); i++) {
    // TODO(CB-253) Change to CPU metric type and
    // take away "* 100" if the new metric type supports double.
    events.push_back(builder.Clone().as_memory_usage(cpu_percentages_[i] * 100));
  }
  // call cobalt FIDL
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  ReinitializeIfPeerClosed(logger_->LogCobaltEvents(std::move(events),
                                                    &status));
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogCpuPercentagesToCobalt returned status=" << StatusToString(status);
  }
  return;
}

std::chrono::seconds SystemMetricsDaemon::LogTemperature() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogTemperature");
  if (!logger_) {
    FX_LOGS(ERROR) << "No logger present. Reconnecting...";
    InitializeLogger();
    return std::chrono::minutes(1);
  }
  int32_t temperature;
  TemperatureFetchStatus status = temperature_fetcher_->FetchTemperature(&temperature);
  if (TemperatureFetchStatus::SUCCEED != status) {
    FX_LOGS(ERROR) << "Temperature fetch failed.";
  }
  temperature_map_[temperature]++;
  temperature_map_size_++;
  if (temperature_map_size_ == 6) {  // Flush every minute.
    LogTemperatureToCobalt();
    temperature_map_.clear();  // Drop the data even if logging does not succeed.
    temperature_map_size_ = 0;
  }
  return std::chrono::seconds(10);
}

void SystemMetricsDaemon::LogTemperatureToCobalt() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogTemperatureToCobalt");
  std::vector<HistogramBucket> temperature_buckets_;
  for (const auto& pair : temperature_map_) {
    HistogramBucket bucket;
    bucket.index = pair.first;
    bucket.count = pair.second;
    temperature_buckets_.push_back(std::move(bucket));
  }
  // call cobalt FIDL
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  ReinitializeIfPeerClosed(
      logger_->LogIntHistogram(fuchsia_system_metrics::kFuchsiaTemperatureExperimentalMetricId,
                               0,
                               "",
                               std::move(temperature_buckets_), &status));
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogTemperatureToCobalt returned status=" << StatusToString(status);
  }
  return;
}

std::chrono::seconds SystemMetricsDaemon::LogMemoryUsage() {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogMemoryUsage");
  if (!logger_) {
    FX_LOGS(ERROR) << "No logger present. Reconnecting...";
    InitializeLogger();
    return std::chrono::minutes(5);
  }
  zx_info_kmem_stats_t stats;
  if (!memory_stats_fetcher_->FetchMemoryStats(&stats)) {
    return std::chrono::minutes(5);
  }
  LogMemoryUsageToCobalt(stats);
  auto uptime = GetUpTime();
  LogMemoryUsageToCobalt(stats, uptime);
  return std::chrono::minutes(1);
}

void SystemMetricsDaemon::LogMemoryUsageToCobalt(const zx_info_kmem_stats_t& stats,
                                                 const std::chrono::seconds& uptime) {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogMemoryUsageToCobalt");
  typedef FuchsiaMemoryExperimental2MetricDimensionMemoryBreakdown Breakdown;

  auto builder =
      std::move(CobaltEventBuilder(fuchsia_system_metrics::kFuchsiaMemoryExperimental2MetricId)
                    .with_event_code_at(1, GetUpTimeEventCode(uptime)));

  std::vector<CobaltEvent> events;
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::TotalBytes)
                       .as_memory_usage(stats.total_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::UsedBytes)
                       .as_memory_usage(stats.total_bytes - stats.free_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::FreeBytes)
                       .as_memory_usage(stats.free_bytes));
  events.push_back(
      builder.Clone().with_event_code_at(0, Breakdown::VmoBytes).as_memory_usage(stats.vmo_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::KernelFreeHeapBytes)
                       .as_memory_usage(stats.free_heap_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::MmuBytes)
                       .as_memory_usage(stats.mmu_overhead_bytes));
  events.push_back(
      builder.Clone().with_event_code_at(0, Breakdown::IpcBytes).as_memory_usage(stats.ipc_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::KernelTotalHeapBytes)
                       .as_memory_usage(stats.total_heap_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::WiredBytes)
                       .as_memory_usage(stats.wired_bytes));
  events.push_back(builder.Clone()
                       .with_event_code_at(0, Breakdown::OtherBytes)
                       .as_memory_usage(stats.other_bytes));

  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  ReinitializeIfPeerClosed(logger_->LogCobaltEvents(std::move(events),
                                                    &status));
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogMemoryUsage() returned status=" << StatusToString(status);
  }
  return;
}

FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot SystemMetricsDaemon::GetUpTimeEventCode(
    const std::chrono::seconds& uptime) {
  if (uptime < std::chrono::minutes(1)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::Up;
  } else if (uptime < std::chrono::minutes(30)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpOneMinute;
  } else if (uptime < std::chrono::hours(1)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpThirtyMinutes;
  } else if (uptime < std::chrono::hours(6)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpOneHour;
  } else if (uptime < std::chrono::hours(12)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpSixHours;
  } else if (uptime < std::chrono::hours(24)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpTwelveHours;
  } else if (uptime < std::chrono::hours(48)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpOneDay;
  } else if (uptime < std::chrono::hours(72)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpTwoDays;
  } else if (uptime < std::chrono::hours(144)) {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpThreeDays;
  } else {
    return FuchsiaMemoryExperimental2MetricDimensionTimeSinceBoot::UpSixDays;
  }
}

void SystemMetricsDaemon::LogMemoryUsageToCobalt(const zx_info_kmem_stats_t& stats) {
  TRACE_DURATION("system_metrics", "SystemMetricsDaemon::LogMemoryUsageToCobalt2");
  typedef FuchsiaMemoryExperimentalMetricDimensionMemoryBreakdown Breakdown;

  std::vector<CobaltEvent> events;
  auto builder = CobaltEventBuilder(fuchsia_system_metrics::kFuchsiaMemoryExperimentalMetricId);

  events.push_back(
      builder.Clone().with_event_code(Breakdown::TotalBytes).as_memory_usage(stats.total_bytes));
  events.push_back(builder.Clone()
                       .with_event_code(Breakdown::UsedBytes)
                       .as_memory_usage(stats.total_bytes - stats.free_bytes));
  events.push_back(
      builder.Clone().with_event_code(Breakdown::FreeBytes).as_memory_usage(stats.free_bytes));
  events.push_back(
      builder.Clone().with_event_code(Breakdown::VmoBytes).as_memory_usage(stats.vmo_bytes));
  events.push_back(builder.Clone()
                       .with_event_code(Breakdown::KernelFreeHeapBytes)
                       .as_memory_usage(stats.free_heap_bytes));
  events.push_back(builder.Clone()
                       .with_event_code(Breakdown::MmuBytes)
                       .as_memory_usage(stats.mmu_overhead_bytes));
  events.push_back(
      builder.Clone().with_event_code(Breakdown::IpcBytes).as_memory_usage(stats.ipc_bytes));
  events.push_back(builder.Clone()
                       .with_event_code(Breakdown::KernelTotalHeapBytes)
                       .as_memory_usage(stats.total_heap_bytes));
  events.push_back(
      builder.Clone().with_event_code(Breakdown::WiredBytes).as_memory_usage(stats.wired_bytes));
  events.push_back(
      builder.Clone().with_event_code(Breakdown::OtherBytes).as_memory_usage(stats.other_bytes));
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  ReinitializeIfPeerClosed(logger_->LogCobaltEvents(std::move(events),
                                                    &status));
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogMemoryUsage() returned status=" << StatusToString(status);
  }
  return;
}

zx_status_t
SystemMetricsDaemon::ReinitializeIfPeerClosed(zx_status_t zx_status) {
  if (zx_status == ZX_ERR_PEER_CLOSED) {
    FX_LOGS(ERROR) << "Logger connection closed. Reconnecting...";
    InitializeLogger();
  }
  return zx_status;
}

void SystemMetricsDaemon::InitializeLogger() {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  // Create a Cobalt Logger. The project name is the one we specified in the
  // Cobalt metrics registry. We specify that our release stage is DOGFOOD.
  // This means we are not allowed to use any metrics declared as DEBUG
  // or FISHFOOD.
  static const char kProjectName[] = "fuchsia_system_metrics";
  // Connect to the cobalt fidl service provided by the environment.
  context_->svc()->Connect(factory_.NewRequest());
  if (!factory_) {
    FX_LOGS(ERROR) << "Unable to get LoggerFactory.";
    return;
  }

  factory_->CreateLoggerFromProjectName(kProjectName, fuchsia::cobalt::ReleaseStage::DOGFOOD,
                                        logger_fidl_proxy_.NewRequest(), &status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "Unable to get Logger from factory. Status=" << StatusToString(status);
    return;
  }
  logger_ = logger_fidl_proxy_.get();
  if (!logger_) {
    FX_LOGS(ERROR) << "Unable to get Logger from factory.";
  }
}

void SystemMetricsDaemon::SetTemperatureFetcher(
    std::unique_ptr<cobalt::TemperatureFetcher> fetcher) {
  temperature_fetcher_ = std::move(fetcher);
}
