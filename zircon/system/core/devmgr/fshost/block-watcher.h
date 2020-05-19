// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fuchsia/fshost/llcpp/fidl.h>

#include <memory>

#include <fs/service.h>

#include "fs-manager.h"

namespace devmgr {

struct BlockWatcherOptions {
  // Identifies that only partition containers should be initialized.
  bool netboot;
  // Identifies that filesystems should be verified before being mounted.
  bool check_filesystems;
  // Identifies that the block watcher should wait for a "data" partition
  // to appear before choosing to launch pkgfs.
  bool wait_for_data;
};

class BlockWatcherServer final : public llcpp::fuchsia::fshost::BlockWatcher::Interface {
 public:
  BlockWatcherServer() {}


  // Creates a new fs::Service backed by a new BlockWatcherServer, to be inserted into
  // a pseudo fs.
  static fbl::RefPtr<fs::Service> Create(devmgr::FsManager* fs_manager,
                                         async_dispatcher* dispatcher);

  void Pause(PauseCompleter::Sync completer) override;
  void Resume(ResumeCompleter::Sync completer) override;
};

// Monitors "/dev/class/block" for new devices indefinitely.
void BlockDeviceWatcher(std::unique_ptr<FsManager> fshost,
                        BlockWatcherOptions options);

}  // namespace devmgr
