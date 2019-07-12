// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_PROCESS_WATCHPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_PROCESS_WATCHPOINT_H_

#include <stdint.h>
#include <zircon/types.h>

#include <set>

#include "src/developer/debug/ipc/records.h"
#include "src/lib/fxl/logging.h"

namespace debug_agent {

class DebuggedProcess;
class DebuggedThread;
class Watchpoint;

class ProcessWatchpoint {
 public:
  ProcessWatchpoint(Watchpoint*, DebuggedProcess*,
                    const debug_ipc::AddressRange& range);
  ~ProcessWatchpoint();

  zx_status_t process_koid() const;
  DebuggedProcess* process() const { return process_; }
  const debug_ipc::AddressRange& range() const { return range_; }

  // Init should be called immediately after construction.
  // If this fails, the process breakpoint is invalid and should not be used.
  zx_status_t Init();

  // Update will look at the settings on the associated Watchpoint and update
  // the HW installations accordingly, removing those threads no longer tracked
  // and adding those that now are.
  //
  // This should be called whenever the associated watchpoint is updated or
  // removed.
  zx_status_t Update();

  // Notification that this watchpoint was just hit. All affected Watchpoints
  // will have their stats updated and placed in the *stats param.
  //
  // IMPORTANT: The caller should check the stats and for any breakpoint
  // with "should_delete" set, remove the breakpoints. This can't conveniently
  // be done within this call because it will cause this ProcessBreakpoint
  // object to be deleted from within itself.
  debug_ipc::BreakpointStats OnHit();

 private:
  // Force uninstallation of the HW watchpoint for all installed threads.
  void Uninstall();

  // Performs the actual arch installation.
  // Will update |installed_threads_| accordingly.
  zx_status_t UpdateWatchpoints(
      const std::vector<DebuggedThread*>& threads_to_remove,
      const std::vector<DebuggedThread*>& threads_to_install);

  // A Process Watchpoint is only related to one abstract watchpoint.
  // This is because watchpoint will differ in range most frequently and having
  // them be merged when possible is more trouble than it's worth.
  Watchpoint* watchpoint_ = nullptr;  // Not-owning.

  // The process this watchpoint is installed on.
  DebuggedProcess* process_ = nullptr;  // Not-owning.

  // The span of addresses this
  debug_ipc::AddressRange range_ = {};

  // List of threads that currently have HW watchpoints installed.
  std::set<zx_koid_t> installed_threads_ = {};

  FXL_DISALLOW_COPY_AND_ASSIGN(ProcessWatchpoint);
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_PROCESS_WATCHPOINT_H_
