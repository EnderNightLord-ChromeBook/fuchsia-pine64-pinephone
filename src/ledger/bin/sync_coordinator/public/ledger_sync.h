// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_SYNC_COORDINATOR_PUBLIC_LEDGER_SYNC_H_
#define SRC_LEDGER_BIN_SYNC_COORDINATOR_PUBLIC_LEDGER_SYNC_H_

#include <lib/fit/function.h>

#include <functional>
#include <memory>

#include "src/ledger/bin/cloud_sync/public/ledger_sync.h"
#include "src/ledger/bin/p2p_sync/public/ledger_communicator.h"
#include "src/ledger/bin/storage/public/page_storage.h"
#include "src/ledger/bin/sync_coordinator/public/page_sync.h"
#include "src/lib/fxl/macros.h"

namespace sync_coordinator {

// Manages synchronization for a ledger.
class LedgerSync {
 public:
  LedgerSync() {}
  virtual ~LedgerSync() {}

  // Creates a new page sync for the given page.
  virtual std::unique_ptr<PageSync> CreatePageSync(storage::PageStorage* page_storage,
                                                   storage::PageSyncClient* page_sync_client) = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(LedgerSync);
};

}  // namespace sync_coordinator

#endif  // SRC_LEDGER_BIN_SYNC_COORDINATOR_PUBLIC_LEDGER_SYNC_H_
