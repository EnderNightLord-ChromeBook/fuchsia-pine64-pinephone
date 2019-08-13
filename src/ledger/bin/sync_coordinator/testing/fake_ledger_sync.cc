// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/sync_coordinator/testing/fake_ledger_sync.h"

#include <lib/fit/function.h>

#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/logging.h"

namespace sync_coordinator {

class FakeLedgerSync::FakePageSync : public PageSync {
 public:
  FakePageSync(storage::PageId page_id, std::map<storage::PageId, int>* sync_page_calls)
      : page_id_(page_id), sync_page_calls_(sync_page_calls) {}

  // PageSync:
  void Start() override {
    started_ = true;
    ++(*sync_page_calls_)[page_id_];

    if (on_backlog_downloaded_) {
      on_backlog_downloaded_();
    }
    if (watcher_) {
      watcher_->Notify({});
    }
    if (on_idle_) {
      on_idle_();
    }
  }

  void SetOnIdle(fit::closure on_idle_callback) override { on_idle_ = std::move(on_idle_callback); }

  bool IsIdle() override { return true; }

  // For this fake, downloads complete immediately, so the on_backlog_downloaded_callback is called
  // right away to avoid waiting for a timeout before fetching the page.
  void SetOnBacklogDownloaded(fit::closure on_backlog_downloaded_callback) override {
    on_backlog_downloaded_ = std::move(on_backlog_downloaded_callback);
  }

  void SetSyncWatcher(SyncStateWatcher* watcher) override {
    watcher_ = watcher;
    if (started_ && watcher_) {
      watcher_->Notify({});
    }
  }

 private:
  fit::closure on_idle_;
  fit::closure on_backlog_downloaded_;
  bool started_ = false;
  storage::PageId page_id_;
  // Pointer to the storage with counters of sync calls to be updated when the Start() method is
  // called for the given page.
  std::map<storage::PageId, int>* sync_page_calls_;
  sync_coordinator::SyncStateWatcher* watcher_ = nullptr;
};

FakeLedgerSync::FakeLedgerSync() = default;

FakeLedgerSync::~FakeLedgerSync() = default;

bool FakeLedgerSync::IsCalled() { return called_; }

int FakeLedgerSync::GetSyncCallsCount(const storage::PageId& page_id) {
  return sync_page_start_calls_[page_id];
}

std::unique_ptr<sync_coordinator::PageSync> FakeLedgerSync::CreatePageSync(
    storage::PageStorage* page_storage, storage::PageSyncClient* /*page_sync_client*/) {
  called_ = true;
  return std::make_unique<FakeLedgerSync::FakePageSync>(page_storage->GetId(),
                                                        &sync_page_start_calls_);
}

}  // namespace sync_coordinator
