// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_PUBLIC_COMMIT_WATCHER_H_
#define SRC_LEDGER_BIN_STORAGE_PUBLIC_COMMIT_WATCHER_H_

#include "src/ledger/bin/storage/public/commit.h"
#include "src/ledger/bin/storage/public/types.h"

namespace storage {

class CommitWatcher {
 public:
  CommitWatcher() {}
  virtual ~CommitWatcher() {}

  // Called when new commits have been created.
  virtual void OnNewCommits(const std::vector<std::unique_ptr<const Commit>>& commits,
                            ChangeSource source) = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(CommitWatcher);
};

}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_PUBLIC_COMMIT_WATCHER_H_
