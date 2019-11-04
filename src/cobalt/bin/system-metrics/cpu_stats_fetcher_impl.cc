// Copyright 2019  The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/system-metrics/cpu_stats_fetcher_impl.h"

#include <fcntl.h>
#include <fuchsia/cobalt/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/resource.h>
#include <zircon/status.h>
#include <zircon/time.h>
#include <chrono>

#include <trace/event.h>

#include "src/lib/syslog/cpp/logger.h"

namespace cobalt {

CpuStatsFetcherImpl::CpuStatsFetcherImpl() { InitializeKernelStats(); }

bool CpuStatsFetcherImpl::FetchCpuPercentage(double *cpu_percentage) {
  TRACE_DURATION("system_metrics", "CpuStatsFetcherImpl::FetchCpuPercentage");
  if (FetchCpuStats() == false) {
    return false;
  }
  ReportRaplReadings();
  bool success = CalculateCpuPercentage(cpu_percentage);
  last_cpu_stats_buffer_.swap(cpu_stats_buffer_);
  last_cpu_stats_ = cpu_stats_;

  last_cpu_fetch_time_ = cpu_fetch_time_;
  return success;
}

bool CpuStatsFetcherImpl::FetchCpuStats() {
  if (stats_service_ == nullptr) {
    FX_LOGS(ERROR) << "CpuStatsFetcherImpl: No kernel stats service "
                   << "present. Reconnecting...";
    InitializeKernelStats();
    return false;
  }
  cpu_fetch_time_ = std::chrono::high_resolution_clock::now();
  auto result = stats_service_->GetCpuStats(cpu_stats_buffer_->view());
  if (result.status() != ZX_OK) {
    FX_LOGS(ERROR) << "CpuStatsFetcherImpl: Fetching "
                   << "CpuStats through fuchsia.kernel.Stats returns "
                   << zx_status_get_string(result.status());
    return false;
  }
  cpu_stats_ = &result->stats;
  if (cpu_stats_->actual_num_cpus < cpu_stats_->per_cpu_stats.count()) {
    FX_LOGS(WARNING) << "CpuStatsFetcherImpl:  actual CPUs reported " << cpu_stats_->actual_num_cpus
                     << " is less than available CPUs " << cpu_stats_->per_cpu_stats.count();
    return false;
  }
  if (num_cpu_cores_ == 0) {
    num_cpu_cores_ = cpu_stats_->actual_num_cpus;
  }
  return true;
}

bool CpuStatsFetcherImpl::CalculateCpuPercentage(double *cpu_percentage) {
  if (last_cpu_stats_ == nullptr) {
    return false;
  }
  auto elapsed_time =
      std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_fetch_time_ - last_cpu_fetch_time_)
          .count();
  double cpu_percentage_sum = 0;
  for (size_t i = 0; i < num_cpu_cores_; i++) {
    zx_duration_t delta_idle_time = zx_duration_sub_duration(
        cpu_stats_->per_cpu_stats[i].idle_time(), last_cpu_stats_->per_cpu_stats[i].idle_time());
    zx_duration_t delta_busy_time =
        (delta_idle_time > elapsed_time ? 0 : elapsed_time - delta_idle_time);
    cpu_percentage_sum +=
        static_cast<double>(delta_busy_time) * 100 / static_cast<double>(elapsed_time);
  }
  *cpu_percentage = cpu_percentage_sum / static_cast<double>(num_cpu_cores_);
  TRACE_COUNTER("system_metrics", "cpu_usage", 0, "average_cpu_percentage", *cpu_percentage);
  return true;
}

// TODO(CF-691) When Component Stats (CS) supports cpu metrics,
// switch to Component Stats / iquery, by creating a new class with the
// interface CpuStatsFetcher.
void CpuStatsFetcherImpl::InitializeKernelStats() {
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    return;
  }
  static const char kKernelStatsSvc[] = "/svc/fuchsia.kernel.Stats";
  status = fdio_service_connect(kKernelStatsSvc, remote.release());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Cobalt SystemMetricsDaemon: Error getting kernel stats. "
                   << "Cannot open fuchsia.kernel.Stats: " << zx_status_get_string(status);
    return;
  }
  cpu_stats_buffer_ =
      std::make_unique<fidl::Buffer<llcpp::fuchsia::kernel::Stats::GetCpuStatsResponse>>();
  last_cpu_stats_buffer_ =
      std::make_unique<fidl::Buffer<llcpp::fuchsia::kernel::Stats::GetCpuStatsResponse>>();
  stats_service_ = std::make_unique<llcpp::fuchsia::kernel::Stats::SyncClient>(std::move(local));
}

static uint64_t RaplToMilliWatts(uint64_t rapl_value, uint64_t joule_quotient,
                                 std::chrono::milliseconds duration) {
  uint64_t milli_joules = (rapl_value * 1000) / joule_quotient;
  return (milli_joules / duration.count() / 1000);
}

void CpuStatsFetcherImpl::ReportRaplReadings() {
  uint8_t rapl_energy_unit = 0xFF & (cpu_stats_->per_cpu_stats[0].rapl_unit() >> 8);
  uint64_t joule_quotient = 1 << rapl_energy_unit;
  std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      cpu_fetch_time_ - last_cpu_fetch_time_);
  TRACE_COUNTER("system_metrics", "cpu_power", 0,
                "pkg_power",
                RaplToMilliWatts(
                    cpu_stats_->per_cpu_stats[0].rapl_pkg()
                        - last_cpu_stats_->per_cpu_stats[0].rapl_pkg(),
                    joule_quotient,
                    duration));
  TRACE_COUNTER("system_metrics", "cpu_power", 0,
                "core_power",
                RaplToMilliWatts(
                    cpu_stats_->per_cpu_stats[0].rapl_core()
                        - last_cpu_stats_->per_cpu_stats[0].rapl_core(),
                    joule_quotient,
                    duration));
  TRACE_COUNTER("system_metrics", "cpu_power", 0,
                "dram_power",
                RaplToMilliWatts(
                    cpu_stats_->per_cpu_stats[0].rapl_dram()
                        - last_cpu_stats_->per_cpu_stats[0].rapl_dram(),
                    joule_quotient,
                    duration));
  TRACE_COUNTER("system_metrics", "cpu_power", 0,
                "gpu_power",
                RaplToMilliWatts(
                    cpu_stats_->per_cpu_stats[0].rapl_gpu()
                        - last_cpu_stats_->per_cpu_stats[0].rapl_gpu(),
                    joule_quotient,
                    duration));
}

}  // namespace cobalt
