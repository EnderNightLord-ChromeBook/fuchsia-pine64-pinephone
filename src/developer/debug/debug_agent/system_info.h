// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_SYSTEM_INFO_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_SYSTEM_INFO_H_

#include <lib/zx/process.h>
#include <zircon/types.h>

#include <vector>

#include "src/developer/debug/ipc/records.h"

namespace debug_agent {

// Fills the root with the process tree of the current system.
zx_status_t GetProcessTree(debug_ipc::ProcessTreeRecord* root);

// Returns a process handle for the given process koid. The process will be
// not is_valid() on failure.
zx::process GetProcessFromKoid(zx_koid_t koid);

// Returns a job handle for the given job koid. The job will be not is_valid()
// on failure.
zx::job GetJobFromKoid(zx_koid_t koid);

// Returns the KOID associated with the given job. Returns 0 on failure.
zx_koid_t GetRootJobKoid();
zx_koid_t GetComponentJobKoid();

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_SYSTEM_INFO_H_
