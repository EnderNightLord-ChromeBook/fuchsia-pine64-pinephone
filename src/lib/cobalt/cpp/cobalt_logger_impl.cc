// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/cobalt/cpp/cobalt_logger_impl.h"

#include <fuchsia/cobalt/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/backoff/exponential_backoff.h>
#include <lib/callback/waiter.h>
#include <lib/component/cpp/connect.h>
#include <lib/syslog/cpp/logger.h>
#include <lib/zx/time.h>
#include <src/lib/fxl/macros.h>

#include <set>

#include "src/lib/cobalt/cpp/cobalt_logger.h"

using fuchsia::cobalt::LoggerFactory;
using fuchsia::cobalt::ProjectProfile;
using fuchsia::cobalt::ReleaseStage;
using fuchsia::cobalt::Status;

namespace cobalt {

BaseCobaltLoggerImpl::BaseCobaltLoggerImpl(async_dispatcher_t* dispatcher, std::string project_name,
                                           fuchsia::cobalt::ReleaseStage release_stage,
                                           ProjectProfile profile)
    : dispatcher_(dispatcher),
      project_name_(std::move(project_name)),
      release_stage_(release_stage),
      profile_(std::move(profile)) {}

BaseCobaltLoggerImpl::~BaseCobaltLoggerImpl() {
  if (!events_in_transit_.empty() || !events_to_send_.empty()) {
    FX_LOGS(WARNING) << "Disconnecting connection to cobalt with events "
                        "still pending... Events will be lost.";
  }
}

void BaseCobaltLoggerImpl::LogEvent(uint32_t metric_id, uint32_t event_code) {
  LogEvent(std::make_unique<OccurrenceEvent>(metric_id, event_code));
}

void BaseCobaltLoggerImpl::LogEventCount(uint32_t metric_id, uint32_t event_code,
                                         const std::string& component, zx::duration period_duration,
                                         int64_t count) {
  LogEvent(std::make_unique<CountEvent>(metric_id, event_code, component,
                                        period_duration.to_usecs(), count));
}

void BaseCobaltLoggerImpl::LogElapsedTime(uint32_t metric_id, uint32_t event_code,
                                          const std::string& component, zx::duration elapsed_time) {
  LogEvent(std::make_unique<ElapsedTimeEvent>(metric_id, event_code, component,
                                              elapsed_time.to_usecs()));
}

void BaseCobaltLoggerImpl::LogFrameRate(uint32_t metric_id, uint32_t event_code,
                                        const std::string& component, float fps) {
  LogEvent(std::make_unique<FrameRateEvent>(metric_id, event_code, component, fps));
}

void BaseCobaltLoggerImpl::LogMemoryUsage(uint32_t metric_id, uint32_t event_code,
                                          const std::string& component, int64_t bytes) {
  LogEvent(std::make_unique<MemoryUsageEvent>(metric_id, event_code, component, bytes));
}

void BaseCobaltLoggerImpl::LogString(uint32_t metric_id, const std::string& s) {
  LogEvent(std::make_unique<StringUsedEvent>(metric_id, s));
}

void BaseCobaltLoggerImpl::StartTimer(uint32_t metric_id, uint32_t event_code,
                                      const std::string& component, const std::string& timer_id,
                                      zx::time timestamp, zx::duration timeout) {
  LogEvent(std::make_unique<StartTimerEvent>(metric_id, event_code, component, timer_id,
                                             timestamp.get() / ZX_USEC(1), timeout.to_secs()));
}

void BaseCobaltLoggerImpl::EndTimer(const std::string& timer_id, zx::time timestamp,
                                    zx::duration timeout) {
  LogEvent(
      std::make_unique<EndTimerEvent>(timer_id, timestamp.get() / ZX_USEC(1), timeout.to_secs()));
}

void BaseCobaltLoggerImpl::LogIntHistogram(
    uint32_t metric_id, uint32_t event_code, const std::string& component,
    std::vector<fuchsia::cobalt::HistogramBucket> histogram) {
  LogEvent(std::make_unique<IntHistogramEvent>(
      metric_id, event_code, component,
      fidl::VectorPtr<fuchsia::cobalt::HistogramBucket>(std::move(histogram))));
}

void BaseCobaltLoggerImpl::LogCustomEvent(
    uint32_t metric_id, std::vector<fuchsia::cobalt::CustomEventValue> event_values) {
  LogEvent(std::make_unique<CustomEvent>(
      metric_id, std::vector<fuchsia::cobalt::CustomEventValue>(std::move(event_values))));
}

void BaseCobaltLoggerImpl::LogCobaltEvent(fuchsia::cobalt::CobaltEvent event) {
  LogEvent(std::make_unique<CobaltEvent>(std::move(event)));
}

void BaseCobaltLoggerImpl::LogCobaltEvents(std::vector<fuchsia::cobalt::CobaltEvent> events) {
  LogEvent(std::make_unique<CobaltEvents>(std::move(events)));
}

void BaseCobaltLoggerImpl::LogEvent(std::unique_ptr<BaseEvent> event) {
  if (dispatcher_ == async_get_default_dispatcher()) {
    LogEventOnMainThread(std::move(event));
    return;
  }

  // Hop to the main thread, and go back to the global object dispatcher.
  async::PostTask(dispatcher_,
                  [event = std::move(event), this]() mutable { this->LogEvent(std::move(event)); });
}

ProjectProfile BaseCobaltLoggerImpl::CloneProjectProfile() {
  ProjectProfile cloned_profile;
  FXL_CHECK(profile_.config.vmo.duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_READ | ZX_RIGHT_MAP,
                                          &cloned_profile.config.vmo) == ZX_OK)
      << "Could not clone config VMO";
  cloned_profile.config.size = profile_.config.size;

  return cloned_profile;
}

void BaseCobaltLoggerImpl::ConnectToCobaltApplication() {
  auto logger_factory = ConnectToLoggerFactory();
  if (!logger_factory) {
    return;
  }

  if (project_name_.empty()) {
    logger_factory->CreateLogger(
        CloneProjectProfile(), logger_.NewRequest(), [this](Status status) {
          if (status == Status::OK) {
            if (logger_) {
              logger_.set_error_handler([this](zx_status_t status) { OnConnectionError(); });
              SendEvents();
            } else {
              OnConnectionError();
            }
          } else {
            FX_LOGST(ERROR, "cobalt_lib") << "CreateLogger() failed.";
          }
        });
  } else {
    logger_factory->CreateLoggerFromProjectName(
        project_name_, release_stage_, logger_.NewRequest(), [this](Status status) {
          if (status == Status::OK) {
            if (logger_) {
              logger_.set_error_handler([this](zx_status_t status) { OnConnectionError(); });
              SendEvents();
            } else {
              OnConnectionError();
            }
          } else {
            FX_LOGST(ERROR, "cobalt_lib") << "CreateLoggerFromProjectName() failed";
          }
        });
  }
}

void BaseCobaltLoggerImpl::OnTransitFail() {
  // Ugly way to move unique_ptrs between sets
  for (const auto& event : events_in_transit_) {
    events_to_send_.insert(std::move(const_cast<std::unique_ptr<BaseEvent>&>(event)));
  }

  events_in_transit_.clear();
}

void BaseCobaltLoggerImpl::OnConnectionError() {
  FX_LOGS(ERROR) << "Connection to cobalt failed. Reconnecting after a delay.";

  OnTransitFail();
  logger_.Unbind();
  async::PostDelayedTask(
      dispatcher_, [this]() { ConnectToCobaltApplication(); }, backoff_.GetNext());
}

void BaseCobaltLoggerImpl::LogEventOnMainThread(std::unique_ptr<BaseEvent> event) {
  events_to_send_.insert(std::move(event));
  if (!logger_ || !events_in_transit_.empty()) {
    return;
  }

  SendEvents();
}

void BaseCobaltLoggerImpl::SendEvents() {
  FXL_DCHECK(events_in_transit_.empty());

  if (events_to_send_.empty()) {
    return;
  }

  events_in_transit_ = std::move(events_to_send_);
  events_to_send_.clear();

  auto waiter = fxl::MakeRefCounted<callback::CompletionWaiter>();
  for (auto& event : events_in_transit_) {
    auto callback = waiter->NewCallback();
    event->Log(&logger_,
               [this, event_ptr = event.get(), callback = std::move(callback)](Status status) {
                 LogEventCallback(event_ptr, status);
                 callback();
               });
  }

  waiter->Finalize([this]() {
    // No transient errors.
    if (events_in_transit_.empty()) {
      backoff_.Reset();
      // Send any event received while |events_in_transit_| was not
      // empty.
      SendEvents();
      return;
    }

    // A transient error happened, retry after a delay.
    async::PostDelayedTask(
        dispatcher_,
        [this]() {
          OnTransitFail();
          SendEvents();
        },
        backoff_.GetNext());
  });
}

void BaseCobaltLoggerImpl::LogEventCallback(const BaseEvent* event, Status status) {
  switch (status) {
    case Status::INVALID_ARGUMENTS:
    case Status::EVENT_TOO_BIG:  // fall through
      // Log the failure.
      FX_LOGS(WARNING) << "Cobalt rejected event for metric: " << event->metric_id()
                       << " with status: " << fidl::ToUnderlying(status);
    case Status::OK:  // fall through
      // Remove the event from the set of events to send.
      events_in_transit_.erase(std::lower_bound(
          events_in_transit_.begin(), events_in_transit_.end(), event,
          [](const std::unique_ptr<BaseEvent>& a, const BaseEvent* b) { return a.get() < b; }));
      break;
    case Status::INTERNAL_ERROR:
    case Status::BUFFER_FULL:
      // Keep the event for re-queueing.
      break;
  }
}

fidl::InterfacePtr<LoggerFactory> CobaltLoggerImpl::ConnectToLoggerFactory() {
  return context_->svc()->Connect<LoggerFactory>();
}

CobaltLoggerImpl::CobaltLoggerImpl(async_dispatcher_t* dispatcher, sys::ComponentContext* context,
                                   ProjectProfile profile)
    : BaseCobaltLoggerImpl(dispatcher, "", ReleaseStage::GA, std::move(profile)),
      context_(context) {
  ConnectToCobaltApplication();
}

CobaltLoggerImpl::CobaltLoggerImpl(async_dispatcher_t* dispatcher, sys::ComponentContext* context,
                                   std::string project_name,
                                   fuchsia::cobalt::ReleaseStage release_stage)
    : BaseCobaltLoggerImpl(dispatcher, std::move(project_name), release_stage, ProjectProfile()),
      context_(context) {
  ConnectToCobaltApplication();
}

}  // namespace cobalt
