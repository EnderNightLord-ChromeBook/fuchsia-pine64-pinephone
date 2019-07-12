// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/debugged_job.h"

#include "src/developer/debug/debug_agent/object_util.h"
#include "src/developer/debug/debug_agent/system_info.h"
#include "src/developer/debug/shared/component_utils.h"
#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/message_loop_target.h"
#include "src/developer/debug/shared/regex.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/logging.h"

namespace debug_agent {

DebuggedJob::DebuggedJob(ProcessStartHandler* handler, zx_koid_t job_koid,
                         zx::job job)
    : handler_(handler), koid_(job_koid), job_(std::move(job)) {}

DebuggedJob::~DebuggedJob() = default;

zx_status_t DebuggedJob::Init() {
  debug_ipc::MessageLoopTarget* loop = debug_ipc::MessageLoopTarget::Current();
  FXL_DCHECK(loop);  // Loop must be created on this thread first.

  // Register for debug exceptions.
  debug_ipc::MessageLoopTarget::WatchJobConfig config;
  config.job_name = NameForObject(job_);
  config.job_handle = job_.get();
  config.job_koid = koid_;
  config.watcher = this;
  return loop->WatchJobExceptions(std::move(config), &job_watch_handle_);
}

void DebuggedJob::OnProcessStarting(zx::exception exception_token,
                                    zx_exception_info_t exception_info) {
  zx::process process = GetProcessFromException(exception_token.get());
  auto proc_name = NameForObject(process);
  zx::thread initial_thread = GetThreadFromException(exception_token.get());

  // Tools like fx serve will connect every second or so to the target, spamming
  // logging for this with a lot of "/boot/bin/sh" starting.
  // We filter this out as it makes debugging much harder.
  if (proc_name != "/boot/bin/sh") {
    DEBUG_LOG(Job) << "Debugged job " << koid_ << ": Process " << proc_name
                   << " starting.";
  }

  // Search through the available filters. If the regex is not valid, fallback
  // to checking if |proc_name| contains the filter.
  FilterInfo* matching_filter = nullptr;
  for (auto& filter : filters_) {
    if (filter.regex.valid()) {
      if (filter.regex.Match(proc_name)) {
        matching_filter = &filter;
        break;
      }
    } else {
      // TODO(DX-953): Job filters should always be valid.
      if (proc_name.find(filter.filter) != std::string::npos) {
        matching_filter = &filter;
        break;
      }
    }
  }

  if (matching_filter) {
    DEBUG_LOG(Job) << "Filter " << matching_filter->filter
                   << " matches process " << proc_name << ". Attaching.";
    handler_->OnProcessStart(matching_filter->filter, std::move(process));
  }

  // Attached to the process. At that point it will get a new thread
  // notification for the initial thread which it can stop or continue as it
  // desires. Therefore, we can always resume the thread in the "new process"
  // exception.
  //
  // Technically it's not necessary to reset the handle, but being explicit here
  // helps readability.
  exception_token.reset();
}

void DebuggedJob::SetFilters(std::vector<std::string> filters) {
  filters_.clear();
  filters_.reserve(filters.size());

  for (auto& filter : filters) {
    // We check if this is a package url. If that is the case, me only need
    // the component as a filter, as the whole URL won't match.
    debug_ipc::ComponentDescription desc;
    if (debug_ipc::ExtractComponentFromPackageUrl(filter, &desc))
      filter = desc.component_name;

    debug_ipc::Regex regex;
    if (!regex.Init(filter))
      FXL_LOG(WARNING) << "Could not initialize regex for filter " << filter;

    DEBUG_LOG(Job) << "Debug job " << koid_ << ": Adding filter " << filter;

    FilterInfo filter_info = {};
    filter_info.filter = std::move(filter);
    filter_info.regex = std::move(regex);
    filters_.push_back(std::move(filter_info));
  }
}

void DebuggedJob::AppendFilter(std::string filter) {
  // We check whether this filter already exists.
  for (auto& existent_filter : filters_) {
    if (existent_filter.filter == filter)
      return;
  }

  debug_ipc::Regex regex;
  if (!regex.Init(filter)) {
    FXL_LOG(WARNING) << "Could not initialize regex for filter " << filter;
  }

  DEBUG_LOG(Job) << "Debugged job " << koid_ << ": Appending filter " << filter;

  FilterInfo filter_info = {};
  filter_info.filter = std::move(filter);
  filter_info.regex = std::move(regex);
  filters_.push_back(std::move(filter_info));
}

}  // namespace debug_agent
