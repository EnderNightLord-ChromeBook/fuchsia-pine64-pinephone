// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/sync_coordinator/impl/user_sync_impl.h"

#include "src/ledger/bin/sync_coordinator/impl/ledger_sync_impl.h"

namespace sync_coordinator {

UserSyncImpl::UserSyncImpl(std::unique_ptr<cloud_sync::UserSync> cloud_sync,
                           std::unique_ptr<p2p_sync::UserCommunicator> p2p_sync)
    : cloud_sync_(std::move(cloud_sync)), p2p_sync_(std::move(p2p_sync)) {}

UserSyncImpl::~UserSyncImpl() {}

void UserSyncImpl::Start() {
  FXL_DCHECK(!started_);
  started_ = true;
  if (cloud_sync_) {
    cloud_sync_->Start();
  }
  if (p2p_sync_) {
    p2p_sync_->Start();
  }
}
void UserSyncImpl::SetWatcher(SyncStateWatcher* watcher) {
  watcher_ = std::make_unique<SyncWatcherConverter>(watcher);
  cloud_sync_->SetSyncWatcher(watcher_.get());
}

std::unique_ptr<LedgerSync> UserSyncImpl::CreateLedgerSync(
    fxl::StringView app_id, encryption::EncryptionService* encryption_service) {
  FXL_DCHECK(started_);
  std::unique_ptr<cloud_sync::LedgerSync> cloud_ledger_sync;
  if (cloud_sync_) {
    cloud_ledger_sync = cloud_sync_->CreateLedgerSync(app_id, encryption_service);
  }
  std::unique_ptr<p2p_sync::LedgerCommunicator> p2p_ledger_sync;
  if (p2p_sync_) {
    // FIXME(etiennej): fix the API
    p2p_ledger_sync = p2p_sync_->GetLedgerCommunicator(app_id.ToString());
  }
  auto combined_sync =
      std::make_unique<LedgerSyncImpl>(std::move(cloud_ledger_sync), std::move(p2p_ledger_sync));
  return combined_sync;
}

}  // namespace sync_coordinator
