// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gather_inspectable.h"

#include <lib/inspect_deprecated/query/discover.h>
#include <zircon/status.h>

#include "harvester.h"
#include "src/lib/fxl/logging.h"

namespace harvester {

// Gather a list of inspectable components.
void GatherInspectable::Gather() {
  // Gather a list of components that contain inspect data.
  const std::string path = "/hub";
  StringSampleList string_sample_list;
  for (auto& location : inspect_deprecated::SyncFindPaths(path)) {
    std::ostringstream label;
    label << "inspectable:" << location.AbsoluteFilePath();
    string_sample_list.emplace_back(label.str(), location.file_name);
  }
  Dockyard().SendStringSampleList(string_sample_list);
}

}  // namespace harvester
