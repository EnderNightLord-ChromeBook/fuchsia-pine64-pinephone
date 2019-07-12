// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/breakpoint_impl.h"

#include <inttypes.h>

#include <algorithm>
#include <map>

#include "src/developer/debug/shared/message_loop.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/developer/debug/zxdb/client/breakpoint_location_impl.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/remote_api.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/system.h"
#include "src/developer/debug/zxdb/client/target.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/symbols/loaded_module_symbols.h"
#include "src/developer/debug/zxdb/symbols/module_symbols.h"
#include "src/developer/debug/zxdb/symbols/process_symbols.h"
#include "src/developer/debug/zxdb/symbols/resolve_options.h"
#include "src/developer/debug/zxdb/symbols/target_symbols.h"

namespace zxdb {

namespace {

uint32_t next_breakpoint_id = 1;

Err ValidateSettings(const BreakpointSettings& settings) {
  switch (settings.scope) {
    case BreakpointSettings::Scope::kSystem:
      if (settings.scope_thread || settings.scope_target)
        return Err("System scopes can't take a thread or target.");
      break;
    case BreakpointSettings::Scope::kTarget:
      if (!settings.scope_target)
        return Err(ErrType::kClientApi, "Target scopes require a target.");
      if (settings.scope_thread)
        return Err(ErrType::kClientApi, "Target scopes can't take a thread.");
      break;
    case BreakpointSettings::Scope::kThread:
      if (!settings.scope_target || !settings.scope_thread) {
        return Err(ErrType::kClientApi, "Thread scopes require a target and a thread.");
      }
  }
  return Err();
}

debug_ipc::Stop SettingsStopToIpcStop(BreakpointSettings::StopMode mode) {
  switch (mode) {
    case BreakpointSettings::StopMode::kNone:
      return debug_ipc::Stop::kNone;
    case BreakpointSettings::StopMode::kThread:
      return debug_ipc::Stop::kThread;
    case BreakpointSettings::StopMode::kProcess:
      return debug_ipc::Stop::kProcess;
    case BreakpointSettings::StopMode::kAll:
      return debug_ipc::Stop::kAll;
  }
}

}  // namespace

struct BreakpointImpl::ProcessRecord {
  // Helper to return whether there are any enabled locations for this process.
  bool HasEnabledLocation() const {
    for (const auto& loc : locs) {
      if (loc.second.IsEnabled())
        return true;
    }
    return false;
  }

  // Helper to add a list of locations to the locs array. Returns true if
  // anything was added (this makes the call site cleaner).
  bool AddLocations(BreakpointImpl* bp, Process* process, const std::vector<Location>& locations) {
    for (const auto& loc : locations) {
      locs.emplace(std::piecewise_construct, std::forward_as_tuple(loc.address()),
                   std::forward_as_tuple(bp, process, loc.address()));
    }
    return !locations.empty();
  }

  // Set when we're registered as an observer for this process.
  bool observing = false;

  // All resolved locations indexed by address.
  std::map<uint64_t, BreakpointLocationImpl> locs;
};

BreakpointImpl::BreakpointImpl(Session* session, bool is_internal)
    : Breakpoint(session),
      is_internal_(is_internal),
      backend_id_(next_breakpoint_id++),
      impl_weak_factory_(this) {
  session->system().AddObserver(this);
}

BreakpointImpl::~BreakpointImpl() {
  if (backend_installed_ && settings_.enabled) {
    // Breakpoint was installed and the process still exists.
    settings_.enabled = false;
    SendBackendRemove(std::function<void(const Err&)>());
  }

  session()->system().RemoveObserver(this);
  for (auto& pair : procs_) {
    if (pair.second.observing) {
      pair.first->RemoveObserver(this);
      pair.second.observing = false;
    }
  }
}

BreakpointSettings BreakpointImpl::GetSettings() const { return settings_; }

void BreakpointImpl::SetSettings(const BreakpointSettings& settings,
                                 std::function<void(const Err&)> callback) {
  Err err = ValidateSettings(settings);
  if (err.has_error()) {
    debug_ipc::MessageLoop::Current()->PostTask(FROM_HERE, [callback, err]() { callback(err); });
    return;
  }

  settings_ = settings;

  for (Target* target : session()->system().GetTargets()) {
    Process* process = target->GetProcess();
    if (process && CouldApplyToProcess(process))
      RegisterProcess(process);
  }

  SyncBackend(std::move(callback));
}

bool BreakpointImpl::IsInternal() const { return is_internal_; }

std::vector<BreakpointLocation*> BreakpointImpl::GetLocations() {
  std::vector<BreakpointLocation*> result;
  for (auto& proc : procs_) {
    for (auto& pair : proc.second.locs)
      result.push_back(&pair.second);
  }
  return result;
}

void BreakpointImpl::UpdateStats(const debug_ipc::BreakpointStats& stats) { stats_ = stats; }

void BreakpointImpl::BackendBreakpointRemoved() { backend_installed_ = false; }

void BreakpointImpl::WillDestroyThread(Process* process, Thread* thread) {
  if (settings_.scope_thread == thread) {
    // When the thread is destroyed that the breakpoint is associated with,
    // disable the breakpoint and convert to a target-scoped breakpoint. This
    // will preserve its state without us having to maintain some "defunct
    // thread" association. The user can associate it with a new thread and
    // re-enable as desired.
    settings_.scope = BreakpointSettings::Scope::kTarget;
    settings_.scope_target = process->GetTarget();
    settings_.scope_thread = nullptr;
    settings_.enabled = false;
  }
}

void BreakpointImpl::DidLoadModuleSymbols(Process* process, LoadedModuleSymbols* module) {
  // Should only get this notification for relevant processes.
  FXL_DCHECK(CouldApplyToProcess(process));

  // Resolve addresses.
  ResolveOptions options;
  options.symbolize = false;  // Just want the addresses back.
  if (procs_[process].AddLocations(this, process,
                                   module->ResolveInputLocation(settings_.location, options)))
    SyncBackend();
}

void BreakpointImpl::WillUnloadModuleSymbols(Process* process, LoadedModuleSymbols* module) {
  // TODO(brettw) need to get the address range of this module and then
  // remove all breakpoints in that range.
}

void BreakpointImpl::WillDestroyTarget(Target* target) {
  if (target == settings_.scope_target) {
    // As with threads going away, when the target goes away for a
    // target-scoped breakpoint, convert to a disabled system-wide breakpoint.
    settings_.scope = BreakpointSettings::Scope::kSystem;
    settings_.scope_target = nullptr;
    settings_.scope_thread = nullptr;
    settings_.enabled = false;
  }
}

void BreakpointImpl::GlobalDidCreateProcess(Process* process) {
  if (CouldApplyToProcess(process)) {
    if (RegisterProcess(process))
      SyncBackend();
  }
}

void BreakpointImpl::GlobalWillDestroyProcess(Process* process) {
  auto found = procs_.find(process);
  if (found == procs_.end())
    return;

  if (found->second.observing)
    process->RemoveObserver(this);

  // Only need to update the backend if there was an enabled address associated
  // with this process.
  bool send_update = found->second.HasEnabledLocation();

  // When the process exits, disable breakpoints that are address-based since
  // the addresses will normally change when a process is loaded.
  if (settings_.location.type == InputLocation::Type::kAddress) {
    // Should only have one process for address-based breakpoints.
    FXL_DCHECK(procs_.size() == 1u);
    FXL_DCHECK(process->GetTarget() == settings_.scope_target);
    settings_.enabled = false;
  }

  procs_.erase(found);

  // Needs to be done after the ProcessRecord is removed.
  if (send_update)
    SyncBackend();
}

void BreakpointImpl::SyncBackend(std::function<void(const Err&)> callback) {
  bool has_locations = HasEnabledLocation();

  if (backend_installed_ && !has_locations) {
    SendBackendRemove(std::move(callback));
  } else if (has_locations) {
    SendBackendAddOrChange(std::move(callback));
  } else {
    // Backend doesn't know about it and we don't require anything.
    if (callback) {
      debug_ipc::MessageLoop::Current()->PostTask(FROM_HERE, [callback]() { callback(Err()); });
    }
  }
}

void BreakpointImpl::SendBackendAddOrChange(std::function<void(const Err&)> callback) {
  backend_installed_ = true;

  debug_ipc::AddOrChangeBreakpointRequest request;
  request.breakpoint_type = settings_.type;
  request.breakpoint.id = backend_id_;
  request.breakpoint.stop = SettingsStopToIpcStop(settings_.stop_mode);
  request.breakpoint.one_shot = settings_.one_shot;

  for (const auto& proc : procs_) {
    for (const auto& pair : proc.second.locs) {
      if (!pair.second.IsEnabled())
        continue;

      debug_ipc::ProcessBreakpointSettings addition;
      addition.process_koid = proc.first->GetKoid();

      if (settings_.scope == BreakpointSettings::Scope::kThread)
        addition.thread_koid = settings_.scope_thread->GetKoid();

      switch (settings_.type) {
        case debug_ipc::BreakpointType::kSoftware:
        case debug_ipc::BreakpointType::kHardware:
          addition.address = pair.second.address();
          break;
        // TODO(donosoc): This should receive a range within input location,
        //                but x64 doesn't allow big ranges so this works as a
        //                first pass.
        case debug_ipc::BreakpointType::kWatchpoint: {
          uint64_t address = pair.second.address();
          addition.address_range = {address, address};
          break;
        }
        case debug_ipc::BreakpointType::kLast:
          FXL_NOTREACHED();
          break;
      }
      request.breakpoint.locations.push_back(addition);
    }
  }

  session()->remote_api()->AddOrChangeBreakpoint(
      request, [callback, breakpoint = impl_weak_factory_.GetWeakPtr()](
                   const Err& err, debug_ipc::AddOrChangeBreakpointReply reply) {
        // Be sure to issue the callback even if the breakpoint no longer
        // exists.
        if (err.has_error()) {
          // Transport error. We don't actually know what state the agent is in
          // since it never got the message. In general this means things were
          // disconnected and the agent no longer exists, so mark the breakpoint
          // disabled.
          if (breakpoint) {
            breakpoint->settings_.enabled = false;
            breakpoint->backend_installed_ = false;
          }
          if (callback)
            callback(err);
        } else if (reply.status != 0) {
          // Backend error. The protocol specifies that errors adding or
          // changing will result in any existing breakpoints with that ID
          // being removed. So mark the breakpoint disabled but keep the
          // settings to the user can fix the problem from the current state if
          // desired.
          if (breakpoint) {
            breakpoint->settings_.enabled = false;
            breakpoint->backend_installed_ = false;
          }
          if (callback) {
            std::stringstream ss;
            ss << "Error setting breakpoint: " << debug_ipc::ZxStatusToString(reply.status);
            if (reply.status == debug_ipc::kZxErrNoResources) {
              ss << std::endl
                 << "Is this a hardware breakpoint? Check \"sys-info\" to "
                    "verify the amount available within the system.";
            }
            callback(Err(ss.str()));
          }
        } else {
          // Success.
          if (callback)
            callback(Err());
        }
      });
}

void BreakpointImpl::SendBackendRemove(std::function<void(const Err&)> callback) {
  debug_ipc::RemoveBreakpointRequest request;
  request.breakpoint_id = backend_id_;

  session()->remote_api()->RemoveBreakpoint(
      request, [callback](const Err& err, debug_ipc::RemoveBreakpointReply reply) {
        if (callback)
          callback(err);
      });

  backend_installed_ = false;
}

void BreakpointImpl::DidChangeLocation() { SyncBackend(); }

bool BreakpointImpl::CouldApplyToProcess(Process* process) const {
  // When applied to all processes, we need all notifications.
  if (settings_.scope == BreakpointSettings::Scope::kSystem)
    return true;

  // Target- and thread-specific breakpoints only watch their process.
  return settings_.scope_target == process->GetTarget();
}

bool BreakpointImpl::HasEnabledLocation() const {
  if (!settings_.enabled)
    return false;
  for (const auto& proc : procs_) {
    if (proc.second.HasEnabledLocation())
      return true;
  }
  return false;
}

bool BreakpointImpl::RegisterProcess(Process* process) {
  if (!procs_[process].observing) {
    procs_[process].observing = true;
    process->AddObserver(this);
  }

  // Clear existing locations for this process.
  ProcessRecord& record = procs_[process];
  bool changed = record.locs.empty();
  record.locs.clear();

  // Resolve addresses.
  ProcessSymbols* symbols = process->GetSymbols();
  ResolveOptions options;
  options.symbolize = false;  // Only need addresses.
  changed |= record.AddLocations(this, process,
                                 symbols->ResolveInputLocation(settings_.location, options));
  return changed;
}

}  // namespace zxdb
