// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_STORAGE_H_
#define SRC_LEDGER_BIN_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_STORAGE_H_

#include <lib/async/dispatcher.h>
#include <lib/callback/capture.h>
#include <lib/fit/function.h>
#include <lib/fsl/socket/strings.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "src/ledger/bin/cloud_sync/impl/testing/test_commit.h"
#include "src/ledger/bin/storage/public/page_storage.h"
#include "src/ledger/bin/storage/testing/page_storage_empty_impl.h"

namespace cloud_sync {
// Fake implementation of storage::PageStorage. Injects the data that PageSync
// asks about: page id, existing unsynced commits to be retrieved through
// GetUnsyncedCommits() and new commits to be retrieved through GetCommit().
// Registers the commits marked as synced.
class TestPageStorage : public storage::PageStorageEmptyImpl {
 public:
  explicit TestPageStorage(async_dispatcher_t* dispatcher);

  std::unique_ptr<TestCommit> NewCommit(std::string id, std::string content, bool unsynced = true);

  storage::PageId GetId() override;

  void SetSyncDelegate(storage::PageSyncDelegate* page_sync_delegate) override;

  ledger::Status GetHeadCommits(
      std::vector<std::unique_ptr<const storage::Commit>>* head_commits) override;

  void GetCommit(storage::CommitIdView commit_id,
                 fit::function<void(ledger::Status, std::unique_ptr<const storage::Commit>)>
                     callback) override;

  void AddCommitsFromSync(
      std::vector<PageStorage::CommitIdAndBytes> ids_and_bytes, storage::ChangeSource source,
      fit::function<void(ledger::Status status, std::vector<storage::CommitId>)> callback) override;

  void GetUnsyncedPieces(fit::function<void(ledger::Status, std::vector<storage::ObjectIdentifier>)>
                             callback) override;

  void AddCommitWatcher(storage::CommitWatcher* watcher) override;

  void RemoveCommitWatcher(storage::CommitWatcher* watcher) override;

  void GetUnsyncedCommits(
      fit::function<void(ledger::Status, std::vector<std::unique_ptr<const storage::Commit>>)>
          callback) override;

  void MarkCommitSynced(const storage::CommitId& commit_id,
                        fit::function<void(ledger::Status)> callback) override;

  void SetSyncMetadata(fxl::StringView key, fxl::StringView value,
                       fit::function<void(ledger::Status)> callback) override;

  void GetSyncMetadata(fxl::StringView key,
                       fit::function<void(ledger::Status, std::string)> callback) override;

  storage::PageId page_id_to_return;
  // Commits to be returned from GetUnsyncedCommits calls.
  std::vector<std::unique_ptr<const storage::Commit>> unsynced_commits_to_return;
  size_t head_count = 1;
  // Commits to be returned from GetCommit() calls.
  std::map<storage::CommitId, std::unique_ptr<const storage::Commit>> new_commits_to_return;
  bool should_fail_get_unsynced_commits = false;
  bool should_fail_get_commit = false;
  bool should_fail_add_commit_from_sync = false;
  bool should_delay_add_commit_confirmation = false;
  std::vector<fit::closure> delayed_add_commit_confirmations;

  unsigned int add_commits_from_sync_calls = 0u;

  storage::PageSyncDelegate* page_sync_delegate_;
  std::set<storage::CommitId> commits_marked_as_synced;
  storage::CommitWatcher* watcher_ = nullptr;
  bool watcher_set = false;
  bool watcher_removed = false;
  std::map<storage::CommitId, std::string> received_commits;
  std::map<std::string, std::string> sync_metadata;

 private:
  async_dispatcher_t* const dispatcher_;
};

}  // namespace cloud_sync

#endif  // SRC_LEDGER_BIN_CLOUD_SYNC_IMPL_TESTING_TEST_PAGE_STORAGE_H_
