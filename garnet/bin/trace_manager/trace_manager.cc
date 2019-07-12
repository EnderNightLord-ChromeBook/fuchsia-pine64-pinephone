// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/trace_manager/trace_manager.h"

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/zx/time.h>

#include <algorithm>
#include <iostream>

namespace tracing {
namespace {

// For large traces or when verbosity is on it can take awhile to write out
// all the records. E.g., ipm_provider can take 40 seconds with --verbose=2
constexpr zx::duration kStopTimeout = zx::sec(60);
constexpr uint32_t kMinBufferSizeMegabytes = 1;
constexpr uint32_t kMaxBufferSizeMegabytes = 64;

// These defaults are copied from fuchsia.tracing/trace_controller.fidl.
constexpr uint32_t kDefaultBufferSizeMegabytesHint = 4;
constexpr uint32_t kDefaultStartTimeoutMilliseconds = 5000;
constexpr controller::BufferingMode kDefaultBufferingMode =
    controller::BufferingMode::ONESHOT;

uint32_t ConstrainBufferSize(uint32_t buffer_size_megabytes) {
  return std::min(std::max(buffer_size_megabytes, kMinBufferSizeMegabytes),
                  kMaxBufferSizeMegabytes);
}

}  // namespace

TraceManager::TraceManager(sys::ComponentContext* context, const Config& config)
    : context_(context), config_(config) {
  // TODO(jeffbrown): We should do this in StartTracing() and take care
  // to restart any crashed providers.  We should also wait briefly to ensure
  // that these providers have registered themselves before replying that
  // tracing has started.
  LaunchConfiguredProviders();
}

TraceManager::~TraceManager() = default;

void TraceManager::StartTracing(
    controller::TraceOptions options, zx::socket output,
    StartTracingCallback start_callback) {
  if (session_) {
    FXL_LOG(ERROR) << "Trace already in progress";
    return;
  }

  uint32_t default_buffer_size_megabytes = kDefaultBufferSizeMegabytesHint;
  if (options.has_buffer_size_megabytes_hint()) {
    const uint32_t buffer_size_mb_hint = options.buffer_size_megabytes_hint();
    default_buffer_size_megabytes = ConstrainBufferSize(buffer_size_mb_hint);
  }

  TraceProviderSpecMap provider_specs;
  if (options.has_provider_specs()) {
    for (const auto& it : options.provider_specs()) {
      provider_specs[it.name()] =
          TraceProviderSpec{it.buffer_size_megabytes_hint()};
    }
  }

  controller::BufferingMode tracing_buffering_mode = kDefaultBufferingMode;
  if (options.has_buffering_mode()) {
    tracing_buffering_mode = options.buffering_mode();
  }
  provider::BufferingMode provider_buffering_mode;
  const char* mode_name;
  switch (tracing_buffering_mode) {
    case controller::BufferingMode::ONESHOT:
      provider_buffering_mode = provider::BufferingMode::ONESHOT;
      mode_name = "oneshot";
      break;
    case controller::BufferingMode::CIRCULAR:
      provider_buffering_mode = provider::BufferingMode::CIRCULAR;
      mode_name = "circular";
      break;
    case controller::BufferingMode::STREAMING:
      provider_buffering_mode = provider::BufferingMode::STREAMING;
      mode_name = "streaming";
      break;
    default:
      FXL_LOG(ERROR) << "Invalid buffering mode: "
                     << static_cast<unsigned>(tracing_buffering_mode);
      return;
  }

  FXL_LOG(INFO) << "Starting trace with " << default_buffer_size_megabytes
                << " MB buffers, buffering mode=" << mode_name;
  if (provider_specs.size() > 0) {
    FXL_LOG(INFO) << "Provider overrides:";
    for (const auto& it : provider_specs) {
      FXL_LOG(INFO) << it.first << ": buffer size "
                    << it.second.buffer_size_megabytes << " MB";
    }
  }

  std::vector<::std::string> categories;
  if (options.has_categories()) {
    categories = std::move(options.categories());
  }
  session_ = fxl::MakeRefCounted<TraceSession>(
      std::move(output), std::move(categories), default_buffer_size_megabytes,
      provider_buffering_mode, std::move(provider_specs),
      [this]() { session_ = nullptr; });

  session_->QueueTraceInfo();
  for (auto& bundle : providers_) {
    session_->AddProvider(&bundle);
  }

  trace_running_ = true;

  uint64_t start_timeout_milliseconds = kDefaultStartTimeoutMilliseconds;
  if (options.has_start_timeout_milliseconds()) {
    start_timeout_milliseconds = options.start_timeout_milliseconds();
  }
  session_->WaitForProvidersToStart(std::move(start_callback),
                                    zx::msec(start_timeout_milliseconds));
}

void TraceManager::StopTracing() {
  if (!session_)
    return;
  trace_running_ = false;

  FXL_LOG(INFO) << "Stopping trace";
  session_->Stop(
      [this]() {
        FXL_LOG(INFO) << "Stopped trace";
        session_ = nullptr;
      },
      kStopTimeout);
}

void TraceManager::GetKnownCategories(GetKnownCategoriesCallback callback) {
  fidl::VectorPtr<controller::KnownCategory> known_categories;
  for (const auto& it : config_.known_categories()) {
    known_categories.push_back(
        controller::KnownCategory{it.first, it.second});
  }
  callback(std::move(known_categories));
}

void TraceManager::RegisterProviderWorker(
    fidl::InterfaceHandle<provider::Provider> provider, uint64_t pid,
    fidl::StringPtr name) {
  FXL_VLOG(2) << "Registering provider {" << pid << ":" << name.get() << "}";
  auto it = providers_.emplace(
      providers_.end(),
      provider.Bind(), next_provider_id_++, pid, name.get());

  it->provider.set_error_handler([this, it](zx_status_t status) {
    if (session_)
      session_->RemoveDeadProvider(&(*it));
    providers_.erase(it);
  });

  if (session_)
    session_->AddProvider(&(*it));
}

void TraceManager::RegisterProvider(
    fidl::InterfaceHandle<provider::Provider> provider, uint64_t pid,
    std::string name) {
  RegisterProviderWorker(std::move(provider), pid, std::move(name));
}

void TraceManager::RegisterProviderSynchronously(
    fidl::InterfaceHandle<provider::Provider> provider, uint64_t pid,
    std::string name, RegisterProviderSynchronouslyCallback callback) {
  RegisterProviderWorker(std::move(provider), pid, std::move(name));
  callback(ZX_OK, trace_running_);
}

void TraceManager::LaunchConfiguredProviders() {
  if (config_.providers().empty())
    return;

  fuchsia::sys::LauncherPtr launcher;
  context_->svc()->Connect(launcher.NewRequest());

  for (const auto& pair : config_.providers()) {
    // TODO(jeffbrown): Only do this if the provider isn't already running.
    // Also keep track of the provider so we can kill it when the trace
    // manager exits or restart it if needed.
    FXL_VLOG(1) << "Starting configured provider: " << pair.first;
    FXL_VLOG(2) << "URL: " << pair.second->url;
    if (FXL_VLOG_IS_ON(2)) {
      std::string args;
      for (const auto& arg : *pair.second->arguments) {
        args += " ";
        args += arg;
      }
      FXL_VLOG(2) << "Args:" << args;
    }
    fuchsia::sys::LaunchInfo launch_info;
    launch_info.url = pair.second->url;
    fidl::Clone(pair.second->arguments, &launch_info.arguments);
    launcher->CreateComponent(std::move(launch_info), nullptr);
  }
}

}  // namespace tracing
