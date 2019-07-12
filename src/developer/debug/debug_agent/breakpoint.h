// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_BREAKPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_BREAKPOINT_H_

#include <zircon/types.h>

#include <set>
#include <string>

#include "src/developer/debug/ipc/records.h"
#include "src/lib/fxl/macros.h"

namespace debug_agent {

class ProcessMemoryAccessor;

// A single breakpoint may apply to many processes and many addresses (even in
// the same process). These instances are called ProcessBreakpoints.
//
// Multiple Breakpoints can also correspond to the same ProcessBreakpoint if
// they have the same process/address.
class Breakpoint {
 public:
  enum class HitResult {
    // Breakpoint was hit and the hit count was incremented.
    kHit,

    // One-shot breakpoint hit. The caller should delete this breakpoint
    // when it sees this result.
    kOneShotHit,

    // This will need to be expanded to include "kContinue" to indicate that
    // this doesn't count as hitting the breakpoint (for when we implement
    // "break on hit count == 5" or "multiple of 7").
  };

  // The process delegate should outlive the Breakpoint object. It allows
  // Breakpoint dependencies to be mocked for testing.
  class ProcessDelegate {
   public:
    // Called to register a new ProcessBreakpoint with the appropriate
    // location. If this fails, the breakpoint has not been set.
    virtual zx_status_t RegisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid,
                                           uint64_t address) = 0;

    // Called When the breakpoint no longer applies to this location.
    virtual void UnregisterBreakpoint(Breakpoint* bp, zx_koid_t process_koid, uint64_t address) = 0;
  };

  explicit Breakpoint(ProcessDelegate* process_delegate);
  ~Breakpoint();

  const debug_ipc::BreakpointStats& stats() const { return stats_; }

  // Sets the initial settings, or updates settings.
  zx_status_t SetSettings(debug_ipc::BreakpointType, const debug_ipc::BreakpointSettings& settings);

  debug_ipc::BreakpointType type() const { return type_; }

  // The setter is used mostly for testing. Normal setting should go through
  // SetSettings.
  void set_type(debug_ipc::BreakpointType type) { type_ = type; }
  const debug_ipc::BreakpointSettings& settings() const { return settings_; }

  // A breakpoint can be set to apply to a specific set of threads. A thread
  // hitting an exception needs to query whether it should apply to it or not.
  bool AppliesToThread(zx_koid_t process_koid, zx_koid_t thread_koid) const;

  // Notification that this breakpoint was just hit.
  HitResult OnHit();

 private:
  // A process koid + address identifies one unique location.
  using LocationPair = std::pair<zx_koid_t, uint64_t>;

  ProcessDelegate* process_delegate_;  // Non-owning.

  debug_ipc::BreakpointType type_ = debug_ipc::BreakpointType::kLast;
  debug_ipc::BreakpointSettings settings_;

  debug_ipc::BreakpointStats stats_;

  std::set<LocationPair> locations_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Breakpoint);
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_BREAKPOINT_H_
