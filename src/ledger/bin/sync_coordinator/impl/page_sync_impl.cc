// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/sync_coordinator/impl/page_sync_impl.h"

#include <lib/callback/waiter.h>
#include <lib/fit/function.h>

#include "src/lib/fxl/memory/ref_ptr.h"

namespace sync_coordinator {
namespace {
// Holder for a synchronization provider (cloud or peer-to-peer).
//
// This object handles communication between storage and the page synchronizer.
class SyncProviderHolderBase : public storage::PageSyncClient, public storage::PageSyncDelegate {
 public:
  SyncProviderHolderBase();
  ~SyncProviderHolderBase() override;

  // storage::PageSyncClient:
  void SetSyncDelegate(storage::PageSyncDelegate* page_sync) override;

  // PageSyncDelegate:
  void GetObject(storage::ObjectIdentifier object_identifier, storage::ObjectType object_type,
                 fit::function<void(ledger::Status status, storage::ChangeSource change_source,
                                    storage::IsObjectSynced is_object_synced,
                                    std::unique_ptr<storage::DataSource::DataChunk>)>
                     callback) override;
  void GetDiff(storage::CommitId commit_id, std::vector<storage::CommitId> possible_bases,
               fit::function<void(ledger::Status status, storage::CommitId base_commit,
                                  std::vector<storage::EntryChange> diff_entries)>
                   callback) override;

 private:
  storage::PageSyncDelegate* page_sync_delegate_;
};

SyncProviderHolderBase::SyncProviderHolderBase() {}

SyncProviderHolderBase::~SyncProviderHolderBase() {}

void SyncProviderHolderBase::SetSyncDelegate(storage::PageSyncDelegate* page_sync) {
  page_sync_delegate_ = page_sync;
}

void SyncProviderHolderBase::GetObject(
    storage::ObjectIdentifier object_identifier, storage::ObjectType object_type,
    fit::function<void(ledger::Status, storage::ChangeSource, storage::IsObjectSynced,
                       std::unique_ptr<storage::DataSource::DataChunk>)>
        callback) {
  page_sync_delegate_->GetObject(std::move(object_identifier), object_type, std::move(callback));
}

void SyncProviderHolderBase::GetDiff(
    storage::CommitId commit_id, std::vector<storage::CommitId> possible_bases,
    fit::function<void(ledger::Status, storage::CommitId, std::vector<storage::EntryChange>)>
        callback) {
  page_sync_delegate_->GetDiff(std::move(commit_id), std::move(possible_bases),
                               std::move(callback));
}
}  // namespace

class PageSyncImpl::CloudSyncHolder : public SyncProviderHolderBase {
 public:
  CloudSyncHolder();
  ~CloudSyncHolder() override;

  void SetCloudSync(std::unique_ptr<cloud_sync::PageSync> cloud_sync);
  cloud_sync::PageSync* GetCloudSync();

 private:
  std::unique_ptr<cloud_sync::PageSync> cloud_sync_;
};

PageSyncImpl::CloudSyncHolder::CloudSyncHolder() {}

PageSyncImpl::CloudSyncHolder::~CloudSyncHolder() {}

void PageSyncImpl::CloudSyncHolder::SetCloudSync(std::unique_ptr<cloud_sync::PageSync> cloud_sync) {
  FXL_DCHECK(!cloud_sync_);
  cloud_sync_ = std::move(cloud_sync);
}

cloud_sync::PageSync* PageSyncImpl::CloudSyncHolder::GetCloudSync() {
  FXL_DCHECK(cloud_sync_);
  return cloud_sync_.get();
}

class PageSyncImpl::P2PSyncHolder : public SyncProviderHolderBase {
 public:
  P2PSyncHolder();
  ~P2PSyncHolder() override;

  void SetP2PSync(std::unique_ptr<p2p_sync::PageCommunicator> p2p_sync);
  p2p_sync::PageCommunicator* GetP2PSync();

 private:
  std::unique_ptr<p2p_sync::PageCommunicator> p2p_sync_;
};

PageSyncImpl::P2PSyncHolder::P2PSyncHolder() {}

PageSyncImpl::P2PSyncHolder::~P2PSyncHolder() {}

void PageSyncImpl::P2PSyncHolder::SetP2PSync(std::unique_ptr<p2p_sync::PageCommunicator> p2p_sync) {
  FXL_DCHECK(!p2p_sync_);
  p2p_sync_ = std::move(p2p_sync);
}

p2p_sync::PageCommunicator* PageSyncImpl::P2PSyncHolder::GetP2PSync() {
  FXL_DCHECK(p2p_sync_);
  return p2p_sync_.get();
}

PageSyncImpl::PageSyncImpl(storage::PageStorage* storage, storage::PageSyncClient* sync_client)
    : storage_(storage), sync_client_(sync_client) {
  FXL_DCHECK(storage_);
  FXL_DCHECK(sync_client_);
}

PageSyncImpl::~PageSyncImpl() {}

storage::PageSyncClient* PageSyncImpl::CreateCloudSyncClient() {
  FXL_DCHECK(!cloud_sync_);
  cloud_sync_ = std::make_unique<CloudSyncHolder>();
  return cloud_sync_.get();
}

void PageSyncImpl::SetCloudSync(std::unique_ptr<cloud_sync::PageSync> cloud_sync) {
  FXL_DCHECK(cloud_sync_);
  if (!cloud_sync) {
    // Cloud sync failed to produce an initialized |cloud_sync| instance - e.g.
    // because cloud provider is disconnected. Unset the entire cloud sync
    // holder to disable the cloud sync logic.
    cloud_sync_.reset();
    return;
  }

  cloud_sync->SetOnUnrecoverableError([this] {
    FXL_LOG(WARNING) << "Shutting down page cloud sync.";
    // TODO(ppi): handle recovery from cloud provider disconnection, LE-567.
    cloud_sync_.reset();
  });
  cloud_sync_->SetCloudSync(std::move(cloud_sync));
}

storage::PageSyncClient* PageSyncImpl::CreateP2PSyncClient() {
  FXL_DCHECK(!p2p_sync_);
  p2p_sync_ = std::make_unique<P2PSyncHolder>();
  return p2p_sync_.get();
}

void PageSyncImpl::SetP2PSync(std::unique_ptr<p2p_sync::PageCommunicator> p2p_sync) {
  FXL_DCHECK(p2p_sync_);
  p2p_sync_->SetP2PSync(std::move(p2p_sync));
}

void PageSyncImpl::Start() {
  sync_client_->SetSyncDelegate(this);
  if (cloud_sync_) {
    cloud_sync_->GetCloudSync()->Start();
  }
  if (p2p_sync_) {
    p2p_sync_->GetP2PSync()->Start();
  }
}

void PageSyncImpl::SetOnIdle(fit::closure on_idle) {
  // Only handle cloud sync for now.
  if (cloud_sync_) {
    cloud_sync_->GetCloudSync()->SetOnIdle(std::move(on_idle));
  }
}

bool PageSyncImpl::IsIdle() {
  if (cloud_sync_) {
    return cloud_sync_->GetCloudSync()->IsIdle();
  }
  return true;
}

void PageSyncImpl::SetOnBacklogDownloaded(fit::closure on_backlog_downloaded) {
  if (cloud_sync_) {
    cloud_sync_->GetCloudSync()->SetOnBacklogDownloaded(std::move(on_backlog_downloaded));
  }
}

void PageSyncImpl::SetSyncWatcher(SyncStateWatcher* watcher) {
  watcher_ = std::make_unique<SyncWatcherConverter>(watcher);
  if (cloud_sync_) {
    cloud_sync_->GetCloudSync()->SetSyncWatcher(watcher_.get());
  }
}

void PageSyncImpl::GetObject(storage::ObjectIdentifier object_identifier,
                             storage::ObjectType object_type,
                             fit::function<void(ledger::Status, storage::ChangeSource,
                                                storage::IsObjectSynced is_object_synced,
                                                std::unique_ptr<storage::DataSource::DataChunk>)>
                                 callback) {
  // AnyWaiter returns the first successful value to its Finalize callback. For
  // example, if P2P returns before cloud with a NOT_FOUND status, then we will
  // wait for Cloud to return; if P2P returns with an OK status, we will pass
  // the P2P-returned value immediately.
  auto waiter = fxl::MakeRefCounted<callback::AnyWaiter<
      ledger::Status, std::tuple<storage::ChangeSource, storage::IsObjectSynced,
                                 std::unique_ptr<storage::DataSource::DataChunk>>>>(
      ledger::Status::OK, ledger::Status::INTERNAL_NOT_FOUND,
      std::tuple<storage::ChangeSource, storage::IsObjectSynced,
                 std::unique_ptr<storage::DataSource::DataChunk>>());
  if (cloud_sync_) {
    cloud_sync_->GetObject(
        object_identifier, object_type,
        [callback = waiter->NewCallback()](ledger::Status status, storage::ChangeSource source,
                                           storage::IsObjectSynced is_object_synced,
                                           std::unique_ptr<storage::DataSource::DataChunk> data) {
          callback(status, std::make_tuple(source, is_object_synced, std::move(data)));
        });
  }
  if (p2p_sync_) {
    p2p_sync_->GetObject(
        std::move(object_identifier), object_type,
        [callback = waiter->NewCallback()](ledger::Status status, storage::ChangeSource source,
                                           storage::IsObjectSynced is_object_synced,
                                           std::unique_ptr<storage::DataSource::DataChunk> data) {
          callback(status, std::make_tuple(source, is_object_synced, std::move(data)));
        });
  }
  waiter->Finalize(
      [callback = std::move(callback)](ledger::Status status,
                                       std::tuple<storage::ChangeSource, storage::IsObjectSynced,
                                                  std::unique_ptr<storage::DataSource::DataChunk>>
                                           data) {
        callback(status, std::get<0>(data), std::get<1>(data), std::move(std::get<2>(data)));
      });
}

void PageSyncImpl::GetDiff(storage::CommitId /*commit_id*/,
                           std::vector<storage::CommitId> /*possible_bases*/,
                           fit::function<void(ledger::Status status, storage::CommitId base_commit,
                                              std::vector<storage::EntryChange> diff_entries)>
                               callback) {
  FXL_NOTIMPLEMENTED();
  callback(ledger::Status::NOT_IMPLEMENTED, {}, {});
}

}  // namespace sync_coordinator
