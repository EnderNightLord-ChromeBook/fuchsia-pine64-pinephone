// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_

#include <stdint.h>
#include <zircon/types.h>

#include <set>

#include "src/developer/debug/ipc/records.h"
#include "src/lib/fxl/logging.h"

namespace debug_agent {

class Watchpoint {
 public:
  // In charge of knowing how to install a watchpoint into the corresponding
  // processes. Having a delegate do it enables to easily mock that
  // functionality.
  //
  // Must outlive the Watchpoint.
  class ProcessDelegate {
   public:
    // Will call AppendProcessWatchpoint will the unique ID that the particular
    // process assigned to that process watchpoint installation.
    virtual zx_status_t RegisterWatchpoint(Watchpoint* wp, zx_koid_t process_koid,
                                           const debug_ipc::AddressRange&) = 0;
    virtual void UnregisterWatchpoint(Watchpoint* wp, zx_koid_t process_koid,
                                      const debug_ipc::AddressRange&) = 0;
  };

  explicit Watchpoint(ProcessDelegate* delegate);
  ~Watchpoint();

  uint32_t id() const { return settings_.id; }

  zx_status_t SetSettings(const debug_ipc::BreakpointSettings& settings);

  // Returns the list of threads that this Watchpoint spans for a process.
  // Returns false if the watchpoint doesn't span the process.
  // If |out| is empty, it means all the threads.
  bool ThreadsToInstall(zx_koid_t process_koid, std::set<zx_koid_t>* out) const;

  debug_ipc::BreakpointStats OnHit();

 private:
  // This is a pair of process and the id of the ProcessWatchpoint installed
  // within it.
  struct WatchpointInstallation {
    zx_koid_t process_koid;
    debug_ipc::AddressRange range;

    bool operator<(const WatchpointInstallation&) const;
  };

  ProcessDelegate* delegate_ = nullptr;  // Not-owning.
  debug_ipc::BreakpointSettings settings_ = {};

  debug_ipc::BreakpointStats stats_;

  std::set<WatchpointInstallation> installed_watchpoints_ = {};

  FXL_DISALLOW_COPY_AND_ASSIGN(Watchpoint);
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_
