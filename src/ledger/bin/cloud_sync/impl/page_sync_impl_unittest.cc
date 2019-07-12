// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/page_sync_impl.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <lib/async/cpp/task.h>
#include <lib/backoff/testing/test_backoff.h>
#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fsl/socket/strings.h>
#include <lib/gtest/test_loop_fixture.h>

#include <memory>
#include <utility>
#include <vector>

#include "src/ledger/bin/cloud_sync/impl/constants.h"
#include "src/ledger/bin/cloud_sync/impl/testing/test_page_cloud.h"
#include "src/ledger/bin/cloud_sync/impl/testing/test_page_storage.h"
#include "src/ledger/bin/cloud_sync/public/sync_state_watcher.h"
#include "src/ledger/bin/encryption/fake/fake_encryption_service.h"
#include "src/ledger/bin/storage/public/page_storage.h"
#include "src/ledger/bin/storage/testing/commit_empty_impl.h"
#include "src/ledger/bin/storage/testing/page_storage_empty_impl.h"
#include "src/lib/fxl/macros.h"

namespace cloud_sync {
namespace {

using testing::ElementsAre;

// Creates a dummy continuation token.
cloud_provider::PositionToken MakeToken(convert::ExtendedStringView token_id) {
  cloud_provider::PositionToken token;
  token.opaque_id = convert::ToArray(token_id);
  return token;
}

class TestSyncStateWatcher : public SyncStateWatcher {
 public:
  TestSyncStateWatcher() {}
  ~TestSyncStateWatcher() override{};

  void Notify(SyncStateContainer sync_state) override {
    if (!states.empty() && sync_state == *states.rbegin()) {
      return;
    }
    states.push_back(sync_state);
  }

  std::vector<SyncStateContainer> states;
};

class PageSyncImplTest : public gtest::TestLoopFixture {
 public:
  PageSyncImplTest()
      : storage_(dispatcher()),
        encryption_service_(dispatcher()),
        page_cloud_(page_cloud_ptr_.NewRequest()) {
    std::unique_ptr<TestSyncStateWatcher> watcher = std::make_unique<TestSyncStateWatcher>();
    state_watcher_ = watcher.get();

    auto download_backoff = std::make_unique<backoff::TestBackoff>(zx::msec(50));
    download_backoff_ptr_ = download_backoff.get();
    auto upload_backoff = std::make_unique<backoff::TestBackoff>(zx::msec(50));
    upload_backoff_ptr_ = upload_backoff.get();

    page_sync_ = std::make_unique<PageSyncImpl>(
        dispatcher(), &storage_, &storage_, &encryption_service_, std::move(page_cloud_ptr_),
        std::move(download_backoff), std::move(upload_backoff), std::move(watcher));
  }
  ~PageSyncImplTest() override {}

 protected:
  enum class UploadStatus {
    ENABLED,
    DISABLED,
  };
  void StartPageSync(UploadStatus status = UploadStatus::ENABLED) {
    if (status == UploadStatus::ENABLED) {
      page_sync_->EnableUpload();
    }
    page_sync_->Start();
  }

  TestPageStorage storage_;
  encryption::FakeEncryptionService encryption_service_;
  cloud_provider::PageCloudPtr page_cloud_ptr_;
  TestPageCloud page_cloud_;
  backoff::TestBackoff* download_backoff_ptr_;
  backoff::TestBackoff* upload_backoff_ptr_;
  TestSyncStateWatcher* state_watcher_;
  std::unique_ptr<PageSyncImpl> page_sync_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageSyncImplTest);
};

SyncStateWatcher::SyncStateContainer MakeStates(DownloadSyncState download,
                                                UploadSyncState upload) {
  return {download, upload};
}

// Verifies that the backlog of commits to upload returned from
// GetUnsyncedCommits() is uploaded to PageCloudHandler.
TEST_F(PageSyncImplTest, UploadBacklog) {
  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");
  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));
  StartPageSync();

  RunLoopUntilIdle();
  ASSERT_TRUE(called);

  ASSERT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ("id1", page_cloud_.received_commits[0].id);
  EXPECT_EQ("content1",
            encryption_service_.DecryptCommitSynchronous(page_cloud_.received_commits[0].data));
  EXPECT_EQ("id2", page_cloud_.received_commits[1].id);
  EXPECT_EQ("content2",
            encryption_service_.DecryptCommitSynchronous(page_cloud_.received_commits[1].data));
  EXPECT_EQ(2u, storage_.commits_marked_as_synced.size());
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id1"));
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id2"));

  EXPECT_THAT(state_watcher_->states,
              ElementsAre(MakeStates(DOWNLOAD_BACKLOG, UPLOAD_NOT_STARTED),
                          MakeStates(DOWNLOAD_BACKLOG, UPLOAD_WAIT_REMOTE_DOWNLOAD),
                          MakeStates(DOWNLOAD_SETTING_REMOTE_WATCHER, UPLOAD_WAIT_REMOTE_DOWNLOAD),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_WAIT_REMOTE_DOWNLOAD),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_PENDING),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_IN_PROGRESS),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_IDLE)));
}

// Verifies that the backlog of commits to upload returned from
// GetUnsyncedCommits() is uploaded to PageCloudHandler.
TEST_F(PageSyncImplTest, PageWatcher) {
  TestSyncStateWatcher watcher;
  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");
  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));
  page_sync_->SetSyncWatcher(&watcher);
  StartPageSync();

  RunLoopUntilIdle();
  ASSERT_TRUE(called);

  EXPECT_THAT(watcher.states,
              ElementsAre(MakeStates(DOWNLOAD_NOT_STARTED, UPLOAD_NOT_STARTED),
                          MakeStates(DOWNLOAD_BACKLOG, UPLOAD_NOT_STARTED),
                          MakeStates(DOWNLOAD_BACKLOG, UPLOAD_WAIT_REMOTE_DOWNLOAD),
                          MakeStates(DOWNLOAD_SETTING_REMOTE_WATCHER, UPLOAD_WAIT_REMOTE_DOWNLOAD),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_WAIT_REMOTE_DOWNLOAD),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_PENDING),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_IN_PROGRESS),
                          MakeStates(DOWNLOAD_IDLE, UPLOAD_IDLE)));
}

// Verifies that sync pauses uploading commits when it is downloading a commit.
TEST_F(PageSyncImplTest, NoUploadWhenDownloading) {
  storage_.should_delay_add_commit_confirmation = true;

  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));
  StartPageSync();
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_TRUE(page_cloud_.set_watcher.is_bound());
  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id1", "content1"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("44"), [] {});
  RunLoopUntilIdle();
  EXPECT_LT(0u, storage_.add_commits_from_sync_calls);

  storage_.watcher_->OnNewCommits(storage_.NewCommit("id2", "content2")->AsList(),
                                  storage::ChangeSource::LOCAL);

  RunLoopUntilIdle();
  EXPECT_FALSE(storage_.delayed_add_commit_confirmations.empty());
  EXPECT_TRUE(page_cloud_.received_commits.empty());

  storage_.delayed_add_commit_confirmations.front()();

  RunLoopUntilIdle();
  EXPECT_FALSE(page_cloud_.received_commits.empty());
}

TEST_F(PageSyncImplTest, UploadExistingCommitsOnlyAfterBacklogDownload) {
  // Verify that two local commits are not uploaded when download is in
  // progress.
  storage_.NewCommit("local1", "content1");
  storage_.NewCommit("local2", "content2");

  page_cloud_.commits_to_return.push_back(
      MakeTestCommit(&encryption_service_, "remote3", "content3"));
  page_cloud_.commits_to_return.push_back(
      MakeTestCommit(&encryption_service_, "remote4", "content4"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));
  bool backlog_downloaded_called = false;
  page_sync_->SetOnBacklogDownloaded([this, &backlog_downloaded_called] {
    EXPECT_EQ(0u, page_cloud_.received_commits.size());
    EXPECT_EQ(0u, storage_.commits_marked_as_synced.size());
    backlog_downloaded_called = true;
  });
  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));
  StartPageSync();

  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  EXPECT_TRUE(backlog_downloaded_called);
  ASSERT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ("local1", page_cloud_.received_commits[0].id);
  EXPECT_EQ("content1",
            encryption_service_.DecryptCommitSynchronous(page_cloud_.received_commits[0].data));
  EXPECT_EQ("local2", page_cloud_.received_commits[1].id);
  EXPECT_EQ("content2",
            encryption_service_.DecryptCommitSynchronous(page_cloud_.received_commits[1].data));
  ASSERT_EQ(2u, storage_.commits_marked_as_synced.size());
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("local1"));
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("local2"));
}

// Verifies that existing commits are uploaded before the new ones.
TEST_F(PageSyncImplTest, UploadExistingAndNewCommits) {
  storage_.NewCommit("id1", "content1");

  page_sync_->SetOnBacklogDownloaded([this] {
    async::PostTask(dispatcher(), [this] {
      auto commit = storage_.NewCommit("id2", "content2");
      storage_.new_commits_to_return["id2"] = commit->Clone();
      storage_.watcher_->OnNewCommits(commit->AsList(), storage::ChangeSource::LOCAL);
    });
  });
  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));

  StartPageSync();
  RunLoopUntilIdle();
  ASSERT_TRUE(called);

  ASSERT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ("id1", page_cloud_.received_commits[0].id);
  EXPECT_EQ("content1",
            encryption_service_.DecryptCommitSynchronous(page_cloud_.received_commits[0].data));
  EXPECT_EQ("id2", page_cloud_.received_commits[1].id);
  EXPECT_EQ("content2",
            encryption_service_.DecryptCommitSynchronous(page_cloud_.received_commits[1].data));
  EXPECT_EQ(2u, storage_.commits_marked_as_synced.size());
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id1"));
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id2"));
}

// Verifies that the on idle callback is called when there is no pending upload
// tasks.
TEST_F(PageSyncImplTest, UploadIdleCallback) {
  int on_idle_calls = 0;

  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");

  page_sync_->SetOnIdle([&on_idle_calls] { on_idle_calls++; });
  StartPageSync();

  // Verify that the idle callback is called once both commits are uploaded.
  RunLoopUntilIdle();
  EXPECT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ(1, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());

  // Notify about a new commit to upload and verify that the idle callback was
  // called again on completion.
  auto commit3 = storage_.NewCommit("id3", "content3");
  storage_.new_commits_to_return["id3"] = commit3->Clone();
  storage_.watcher_->OnNewCommits(commit3->AsList(), storage::ChangeSource::LOCAL);
  EXPECT_FALSE(page_sync_->IsIdle());
  RunLoopUntilIdle();
  EXPECT_EQ(3u, page_cloud_.received_commits.size());
  EXPECT_EQ(2, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
}

// Verifies that a failure to persist the remote commit stops syncing remote
// commits and calls the error callback.
TEST_F(PageSyncImplTest, FailToStoreRemoteCommit) {
  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));
  int error_callback_calls = 0;
  page_sync_->SetOnUnrecoverableError([&error_callback_calls] { error_callback_calls++; });
  StartPageSync();
  RunLoopUntilIdle();
  ASSERT_TRUE(called);
  ASSERT_TRUE(page_cloud_.set_watcher.is_bound());

  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id1", "content1"}});
  ASSERT_TRUE(commit_pack);
  storage_.should_fail_add_commit_from_sync = true;
  EXPECT_EQ(0, error_callback_calls);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("42"), [] {});

  RunLoopUntilIdle();
  EXPECT_FALSE(page_cloud_.set_watcher.is_bound());
  EXPECT_EQ(1, error_callback_calls);
}

// Verifies that the on idle callback is called when there is no download in
// progress.
TEST_F(PageSyncImplTest, DownloadIdleCallback) {
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id2", "content2"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));

  int on_idle_calls = 0;
  page_sync_->SetOnIdle([&on_idle_calls] { on_idle_calls++; });
  StartPageSync();
  EXPECT_EQ(0, on_idle_calls);
  EXPECT_FALSE(page_sync_->IsIdle());

  // Run the message loop and verify that the sync is idle after all remote
  // commits are added to storage.
  RunLoopUntilIdle();
  EXPECT_EQ(1, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
  EXPECT_EQ(2u, storage_.received_commits.size());

  // Notify about a new commit to download and verify that the idle callback was
  // called again on completion.
  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id3", "content3"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("44"), [] {});
  RunLoopUntilIdle();
  EXPECT_EQ(3u, storage_.received_commits.size());
  EXPECT_EQ(2, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
}

// Verifies that uploads are paused until EnableUpload is called.
TEST_F(PageSyncImplTest, UploadIsPaused) {
  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");
  bool called;
  page_sync_->SetOnIdle(callback::SetWhenCalled(&called));

  StartPageSync(UploadStatus::DISABLED);
  RunLoopUntilIdle();
  ASSERT_TRUE(called);

  ASSERT_EQ(0u, page_cloud_.received_commits.size());

  page_sync_->EnableUpload();
  RunLoopUntilIdle();

  ASSERT_EQ(2u, page_cloud_.received_commits.size());
}

// Merge commits are deterministic, so can already be in the cloud when we try
// to upload it. The upload will then fail. However, we should stop retrying to
// upload the commit once we received a notification for it through the cloud
// sync watcher.
TEST_F(PageSyncImplTest, UploadCommitAlreadyInCloud) {
  // Complete the initial sync.
  StartPageSync();
  RunLoopUntilIdle();
  EXPECT_EQ(1u, page_cloud_.get_commits_calls);

  // Create a local commit, but make the upload fail.
  page_cloud_.commit_status_to_return = cloud_provider::Status::SERVER_ERROR;
  auto commit1 = storage_.NewCommit("id1", "content1");
  storage_.new_commits_to_return["id1"] = commit1->Clone();
  storage_.watcher_->OnNewCommits(commit1->AsList(), storage::ChangeSource::LOCAL);

  // We need to wait for the callback to be executed on the PageSync side.
  RunLoopUntilIdle();
  EXPECT_EQ(1u, page_cloud_.add_commits_calls);
  EXPECT_EQ(1, upload_backoff_ptr_->get_next_count);

  // Verify that the commit is still not marked as synced in storage.
  EXPECT_TRUE(storage_.commits_marked_as_synced.empty());

  // Let's receive the same commit from the remote side.
  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id1", "content1"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("44"), [] {});
  RunLoopUntilIdle();
  EXPECT_TRUE(page_sync_->IsIdle());

  // No additional calls.
  EXPECT_EQ(1u, page_cloud_.add_commits_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
}

TEST_F(PageSyncImplTest, UnrecoverableError) {
  int on_error_calls = 0;
  page_sync_->SetOnUnrecoverableError([&on_error_calls] { on_error_calls++; });
  // Complete the initial sync.
  StartPageSync();
  RunLoopUntilIdle();
  EXPECT_EQ(0, on_error_calls);

  page_cloud_.Unbind();
  RunLoopUntilIdle();
  EXPECT_EQ(1, on_error_calls);
}

}  // namespace
}  // namespace cloud_sync
