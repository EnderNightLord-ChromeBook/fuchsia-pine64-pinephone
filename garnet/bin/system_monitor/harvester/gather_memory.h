// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_SYSTEM_MONITOR_HARVESTER_GATHER_MEMORY_H_
#define GARNET_BIN_SYSTEM_MONITOR_HARVESTER_GATHER_MEMORY_H_

#include "gather_category.h"

namespace harvester {

class GatherMemory : public GatherCategory {
 public:
  GatherMemory(zx_handle_t root_resource, harvester::DockyardProxy& dockyard_proxy)
      : GatherCategory(root_resource, dockyard_proxy) {}

  void Gather() override;
};

}  // namespace harvester

#endif  // GARNET_BIN_SYSTEM_MONITOR_HARVESTER_GATHER_MEMORY_H_
