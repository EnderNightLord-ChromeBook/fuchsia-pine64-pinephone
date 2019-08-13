// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_TESTING_FAKE_DISK_CLEANUP_MANAGER_H_
#define SRC_LEDGER_BIN_TESTING_FAKE_DISK_CLEANUP_MANAGER_H_

#include <lib/fit/function.h>

#include "src/ledger/bin/app/disk_cleanup_manager.h"
#include "src/ledger/bin/app/page_usage_listener.h"
#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/strings/string_view.h"

namespace ledger {

class FakeDiskCleanupManager : public DiskCleanupManager, public PageUsageListener {
 public:
  FakeDiskCleanupManager() = default;
  ~FakeDiskCleanupManager() override = default;

  void set_on_OnExternallyUnused(fit::closure on_OnExternallyUnused_callback) {
    on_OnExternallyUnused_callback_ = std::move(on_OnExternallyUnused_callback);
  }

  void set_on_OnInternallyUnused(fit::closure on_OnInternallyUnused_callback) {
    on_OnInternallyUnused_callback_ = std::move(on_OnInternallyUnused_callback);
  }

  void set_on_empty(fit::closure on_empty_callback) override {}

  bool IsEmpty() override { return true; }

  void TryCleanUp(fit::function<void(Status)> callback) override {
    // Do not call the callback directly.
    cleanup_callback = std::move(callback);
  }

  void OnExternallyUsed(fxl::StringView /*ledger_name*/, storage::PageIdView /*page_id*/) override {
    ++externally_used_count;
  }

  void OnExternallyUnused(fxl::StringView /*ledger_name*/,
                          storage::PageIdView /*page_id*/) override {
    ++externally_unused_count;
    if (on_OnExternallyUnused_callback_) {
      on_OnExternallyUnused_callback_();
    }
  }

  void OnInternallyUsed(fxl::StringView /*ledger_name*/, storage::PageIdView /*page_id*/) override {
    ++internally_used_count;
  }

  void OnInternallyUnused(fxl::StringView /*ledger_name*/,
                          storage::PageIdView /*page_id*/) override {
    ++internally_unused_count;
    if (on_OnInternallyUnused_callback_) {
      on_OnInternallyUnused_callback_();
    }
  }

  // Resets all the counters in this fake. Can be useful when checking a number of steps in a test.
  void ResetCounters() {
    externally_used_count = 0;
    externally_unused_count = 0;
    internally_used_count = 0;
    internally_unused_count = 0;
  }

  int externally_used_count = 0;
  int externally_unused_count = 0;
  int internally_used_count = 0;
  int internally_unused_count = 0;
  fit::closure on_OnExternallyUnused_callback_;
  fit::closure on_OnInternallyUnused_callback_;
  fit::function<void(Status)> cleanup_callback;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(FakeDiskCleanupManager);
};

}  // namespace ledger

#endif  // SRC_LEDGER_BIN_TESTING_FAKE_DISK_CLEANUP_MANAGER_H_
