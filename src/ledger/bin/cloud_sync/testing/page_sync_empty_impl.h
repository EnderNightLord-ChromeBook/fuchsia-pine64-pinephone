// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_CLOUD_SYNC_TESTING_PAGE_SYNC_EMPTY_IMPL_H_
#define SRC_LEDGER_BIN_CLOUD_SYNC_TESTING_PAGE_SYNC_EMPTY_IMPL_H_

#include <lib/fit/function.h>

#include "src/ledger/bin/cloud_sync/public/page_sync.h"

namespace cloud_sync {

class PageSyncEmptyImpl : public PageSync {
 public:
  // PageSync:
  void Start() override;
  void SetOnIdle(fit::closure on_idle_callback) override;
  bool IsIdle() override;
  void SetOnBacklogDownloaded(fit::closure on_backlog_downloaded_callback) override;
  void SetSyncWatcher(SyncStateWatcher* watcher) override;
  void SetOnUnrecoverableError(fit::closure on_error) override;
};

}  // namespace cloud_sync

#endif  // SRC_LEDGER_BIN_CLOUD_SYNC_TESTING_PAGE_SYNC_EMPTY_IMPL_H_
