// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_SYNC_COORDINATOR_PUBLIC_USER_SYNC_H_
#define SRC_LEDGER_BIN_SYNC_COORDINATOR_PUBLIC_USER_SYNC_H_

#include <memory>

#include "src/ledger/bin/encryption/public/encryption_service.h"
#include "src/ledger/bin/sync_coordinator/public/ledger_sync.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/strings/string_view.h"

namespace sync_coordinator {

// Top level factory for every object sync related for a given user.
class UserSync {
 public:
  UserSync() {}
  virtual ~UserSync() {}

  // Starts the user-level synchronization.
  virtual void Start() = 0;

  // Sets a watcher aggregating the synchronization state of all operations
  // under this user. Set to nullptr for unregistering.
  virtual void SetWatcher(SyncStateWatcher* watcher) = 0;

  // Returns the Ledger-level synchronization object. The user-level
  // synchronization must be started before calling this method.
  virtual std::unique_ptr<LedgerSync> CreateLedgerSync(
      fxl::StringView app_id, encryption::EncryptionService* encryption_service) = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(UserSync);
};

}  // namespace sync_coordinator

#endif  // SRC_LEDGER_BIN_SYNC_COORDINATOR_PUBLIC_USER_SYNC_H_
