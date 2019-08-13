// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/page_download.h"

#include <lib/async/dispatcher.h>
#include <lib/backoff/testing/test_backoff.h>
#include <lib/callback/capture.h>
#include <lib/callback/set_when_called.h>
#include <lib/fidl/cpp/optional.h>
#include <lib/fit/function.h>
#include <lib/fsl/socket/strings.h>
#include <lib/gtest/test_loop_fixture.h>

#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
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

// Creates a dummy continuation token.
cloud_provider::PositionToken MakeToken(convert::ExtendedStringView token_id) {
  cloud_provider::PositionToken token;
  token.opaque_id = convert::ToArray(token_id);
  return token;
}

// Creates a dummy object identifier.
storage::ObjectIdentifier MakeObjectIdentifier() {
  // The returned value does not need to be valid (wrt. internal storage constraints) as it is only
  // used as an opaque identifier for cloud_sync. It does not need to be tracked either because we
  // use |TestPageStorage|, a fake storage that does not perform garbage collection.
  return storage::ObjectIdentifier(1u, 1u, storage::ObjectDigest("object_digest"), nullptr);
}

constexpr zx::duration kTestBackoffInterval = zx::msec(50);
std::unique_ptr<backoff::TestBackoff> NewTestBackoff() {
  auto result = std::make_unique<backoff::TestBackoff>(kTestBackoffInterval);
  return result;
}

// Dummy implementation of a backoff policy, which always returns zero backoff
// time.
template <typename E>
class BasePageDownloadTest : public gtest::TestLoopFixture, public PageDownload::Delegate {
 public:
  BasePageDownloadTest()
      : storage_(dispatcher()),
        encryption_service_(dispatcher()),
        page_cloud_(page_cloud_ptr_.NewRequest()),
        task_runner_(dispatcher()) {
    page_download_ =
        std::make_unique<PageDownload>(&task_runner_, &storage_, &storage_, &encryption_service_,
                                       &page_cloud_ptr_, this, NewTestBackoff());
  }
  ~BasePageDownloadTest() override {}

 protected:
  void SetOnNewStateCallback(fit::closure callback) { new_state_callback_ = std::move(callback); }

  // Starts download and runs the loop until the download state is idle. Returns
  // true iff the download state went to idle as expected.
  ::testing::AssertionResult StartDownloadAndWaitForIdle() {
    bool on_idle_called = false;
    SetOnNewStateCallback([this, &on_idle_called] {
      if (states_.back() == DOWNLOAD_IDLE) {
        on_idle_called = true;
      }
    });
    page_download_->StartDownload();
    RunLoopUntilIdle();
    SetOnNewStateCallback([] {});

    if (on_idle_called) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "The download state never reached idle.";
  }

  TestPageStorage storage_;
  E encryption_service_;
  cloud_provider::PageCloudPtr page_cloud_ptr_;
  TestPageCloud page_cloud_;
  std::vector<DownloadSyncState> states_;
  std::unique_ptr<PageDownload> page_download_;
  int error_callback_calls_ = 0;

 private:
  void SetDownloadState(DownloadSyncState sync_state) override {
    if (!states_.empty() && sync_state == states_.back()) {
      // Skip identical states.
      return;
    }
    states_.push_back(sync_state);
    if (new_state_callback_) {
      new_state_callback_();
    }
  }

  fit::closure new_state_callback_;
  callback::ScopedTaskRunner task_runner_;
  FXL_DISALLOW_COPY_AND_ASSIGN(BasePageDownloadTest);
};

using PageDownloadTest = BasePageDownloadTest<encryption::FakeEncryptionService>;

// Verifies that the backlog of unsynced commits is retrieved from the cloud
// provider and saved in storage.
TEST_F(PageDownloadTest, DownloadBacklog) {
  EXPECT_EQ(storage_.received_commits.size(), 0u);
  EXPECT_EQ(storage_.sync_metadata.count(kTimestampKey.ToString()), 0u);

  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id2", "content2"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));

  ASSERT_TRUE(StartDownloadAndWaitForIdle());

  EXPECT_EQ(storage_.received_commits.size(), 2u);
  EXPECT_EQ(storage_.received_commits["id1"], "content1");
  EXPECT_EQ(storage_.received_commits["id2"], "content2");
  EXPECT_EQ(storage_.sync_metadata[kTimestampKey.ToString()], "43");
  EXPECT_EQ(states_.back(), DOWNLOAD_IDLE);
}

TEST_F(PageDownloadTest, DownloadLongBacklog) {
  EXPECT_EQ(storage_.received_commits.size(), 0u);
  EXPECT_EQ(storage_.sync_metadata.count(kTimestampKey.ToString()), 0u);

  const size_t commit_count = 100'000;
  for (size_t i = 0; i < commit_count; i++) {
    page_cloud_.commits_to_return.push_back(MakeTestCommit(
        &encryption_service_, "id" + std::to_string(i), "content" + std::to_string(i)));
  }
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));

  ASSERT_TRUE(StartDownloadAndWaitForIdle());

  EXPECT_EQ(storage_.received_commits.size(), commit_count);
  EXPECT_EQ(storage_.sync_metadata[kTimestampKey.ToString()], "43");
  EXPECT_EQ(states_.back(), DOWNLOAD_IDLE);
}

TEST_F(PageDownloadTest, DownloadEmptyBacklog) { ASSERT_TRUE(StartDownloadAndWaitForIdle()); }

// Verifies that the cloud watcher is registered for the timestamp of the most
// recent commit downloaded from the backlog.
TEST_F(PageDownloadTest, RegisterWatcher) {
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id2", "content2"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));

  ASSERT_TRUE(StartDownloadAndWaitForIdle());

  ASSERT_EQ(page_cloud_.set_watcher_position_tokens.size(), 1u);
  EXPECT_EQ(convert::ToString(page_cloud_.set_watcher_position_tokens.front()->opaque_id), "43");
}

// Verifies that commit notifications about new commits in cloud provider are
// received and passed to storage.
TEST_F(PageDownloadTest, ReceiveNotifications) {
  ASSERT_TRUE(StartDownloadAndWaitForIdle());

  // Deliver a remote notification.
  EXPECT_EQ(storage_.received_commits.size(), 0u);
  EXPECT_EQ(storage_.sync_metadata.count(kTimestampKey.ToString()), 0u);
  auto commit_pack =
      MakeTestCommitPack(&encryption_service_, {{"id1", "content1"}, {"id2", "content2"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("43"), [] {});
  RunLoopUntilIdle();

  // Verify that the remote commits were added to storage.
  EXPECT_EQ(storage_.received_commits.size(), 2u);
  EXPECT_EQ(storage_.received_commits["id1"], "content1");
  EXPECT_EQ(storage_.received_commits["id2"], "content2");
  EXPECT_EQ(storage_.sync_metadata[kTimestampKey.ToString()], "43");
}

// Verify that we retry setting the remote watcher on connection errors
// and when the auth token expires.
TEST_F(PageDownloadTest, RetryRemoteWatcher) {
  page_download_->StartDownload();
  EXPECT_EQ(storage_.received_commits.size(), 0u);

  RunLoopUntilIdle();
  EXPECT_EQ(page_cloud_.set_watcher_position_tokens.size(), 1u);

  page_cloud_.set_watcher->OnError(cloud_provider::Status::NETWORK_ERROR);
  RunLoopFor(kTestBackoffInterval);
  EXPECT_EQ(page_cloud_.set_watcher_position_tokens.size(), 2u);

  page_cloud_.set_watcher->OnError(cloud_provider::Status::AUTH_ERROR);
  RunLoopFor(kTestBackoffInterval);
  EXPECT_EQ(page_cloud_.set_watcher_position_tokens.size(), 3u);
}

// Verifies that if multiple remote commits are received while one batch is
// already being downloaded, the new remote commits are added to storage in one
// request.
TEST_F(PageDownloadTest, CoalesceMultipleNotifications) {
  ASSERT_TRUE(StartDownloadAndWaitForIdle());

  // Make the storage delay requests to add remote commits.
  storage_.should_delay_add_commit_confirmation = true;

  // Deliver a remote notification.
  EXPECT_EQ(storage_.received_commits.size(), 0u);
  EXPECT_EQ(storage_.sync_metadata.count(kTimestampKey.ToString()), 0u);
  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id1", "content1"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("42"), [] {});
  RunLoopUntilIdle();
  EXPECT_EQ(storage_.delayed_add_commit_confirmations.size(), 1u);

  // Add two more remote commits, before storage confirms adding the first one.
  commit_pack =
      MakeTestCommitPack(&encryption_service_, {{"id2", "content2"}, {"id3", "content3"}});
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("44"), [] {});

  // Make storage confirm adding the first commit.
  storage_.should_delay_add_commit_confirmation = false;
  storage_.delayed_add_commit_confirmations.front()();
  RunLoopUntilIdle();
  EXPECT_EQ(storage_.received_commits.size(), 3u);

  // Verify that all three commits were delivered in total of two calls to
  // storage.
  EXPECT_EQ(storage_.received_commits.size(), 3u);
  EXPECT_EQ(storage_.received_commits["id1"], "content1");
  EXPECT_EQ(storage_.received_commits["id2"], "content2");
  EXPECT_EQ(storage_.received_commits["id3"], "content3");
  EXPECT_EQ(storage_.sync_metadata[kTimestampKey.ToString()], "44");
  EXPECT_EQ(storage_.add_commits_from_sync_calls, 2u);
}

// TODO(LE-497): The following should not pass. Investigate why.
// Verifies that failing attempts to download the backlog of unsynced commits
// are retried.
TEST_F(PageDownloadTest, RetryDownloadBacklog) {
  page_cloud_.status_to_return = cloud_provider::Status::NETWORK_ERROR;
  page_download_->StartDownload();

  // Loop through five attempts to download the backlog.
  SetOnNewStateCallback([this] {
    if (page_cloud_.get_commits_calls >= 5u) {
      QuitLoop();
    }
  });
  RunLoopUntilIdle();
  EXPECT_GE(5u, page_cloud_.get_commits_calls);
  EXPECT_EQ(storage_.received_commits.size(), 0u);

  SetOnNewStateCallback([] {});
  page_cloud_.status_to_return = cloud_provider::Status::OK;
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("42"));
  RunLoopFor(kTestBackoffInterval);
  EXPECT_TRUE(page_download_->IsIdle());

  EXPECT_EQ(storage_.received_commits.size(), 1u);
  EXPECT_EQ(storage_.received_commits["id1"], "content1");
  EXPECT_EQ(storage_.sync_metadata[kTimestampKey.ToString()], "42");
}

// Verifies that a failure to persist the remote commit stops syncing remote
// commits and the error status is returned.
TEST_F(PageDownloadTest, FailToStoreRemoteCommit) {
  ASSERT_TRUE(StartDownloadAndWaitForIdle());
  EXPECT_TRUE(page_cloud_.set_watcher.is_bound());

  storage_.should_fail_add_commit_from_sync = true;
  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id1", "content1"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("42"), [] {});

  RunLoopUntilIdle();
  ASSERT_FALSE(states_.empty());
  EXPECT_EQ(states_.back(), DOWNLOAD_PERMANENT_ERROR);
  EXPECT_FALSE(page_cloud_.set_watcher.is_bound());
}

// Verifies that the idle status is returned when there is no download in
// progress.
TEST_F(PageDownloadTest, DownloadIdleCallback) {
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id2", "content2"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));

  int on_idle_calls = 0;
  SetOnNewStateCallback([this, &on_idle_calls] {
    if (states_.back() == DOWNLOAD_IDLE) {
      on_idle_calls++;
    }
  });
  page_download_->StartDownload();
  EXPECT_EQ(on_idle_calls, 0);
  EXPECT_FALSE(page_download_->IsIdle());

  // Run the message loop and verify that the sync is idle after all remote
  // commits are added to storage.
  RunLoopUntilIdle();
  EXPECT_EQ(on_idle_calls, 1);
  EXPECT_TRUE(page_download_->IsIdle());

  // Notify about a new commit to download and verify that the idle callback was
  // called again on completion.
  auto commit_pack = MakeTestCommitPack(&encryption_service_, {{"id3", "content3"}});
  ASSERT_TRUE(commit_pack);
  page_cloud_.set_watcher->OnNewCommits(std::move(*commit_pack), MakeToken("44"), [] {});
  RunLoopUntilIdle();
  EXPECT_EQ(storage_.received_commits.size(), 3u);
  EXPECT_EQ(on_idle_calls, 2);
  EXPECT_TRUE(page_download_->IsIdle());
}

// Verifies that sync correctly fetches objects from the cloud provider.
TEST_F(PageDownloadTest, GetObject) {
  storage::ObjectIdentifier object_identifier = MakeObjectIdentifier();
  std::string object_name = encryption_service_.GetObjectNameSynchronous(object_identifier);
  page_cloud_.objects_to_return[object_name] =
      encryption_service_.EncryptObjectSynchronous("content");
  page_download_->StartDownload();

  bool called;
  ledger::Status status;
  storage::ChangeSource source;
  storage::IsObjectSynced is_object_synced;
  std::unique_ptr<storage::DataSource::DataChunk> data_chunk;
  RunLoopUntilIdle();
  states_.clear();
  storage_.page_sync_delegate_->GetObject(
      object_identifier, storage::ObjectType::BLOB,
      callback::Capture(callback::SetWhenCalled(&called), &status, &source, &is_object_synced,
                        &data_chunk));
  RunLoopUntilIdle();

  EXPECT_TRUE(called);
  EXPECT_EQ(status, ledger::Status::OK);
  EXPECT_EQ(source, storage::ChangeSource::CLOUD);
  EXPECT_EQ(is_object_synced, storage::IsObjectSynced::YES);
  EXPECT_EQ(data_chunk->Get().ToString(), "content");
  EXPECT_EQ(states_.size(), 2u);
  EXPECT_EQ(states_[0], DOWNLOAD_IN_PROGRESS);
  EXPECT_EQ(states_[1], DOWNLOAD_IDLE);
}

// Verifies that sync retries GetObject() attempts upon connection error.
TEST_F(PageDownloadTest, RetryGetObject) {
  storage::ObjectIdentifier object_identifier = MakeObjectIdentifier();
  std::string object_name = encryption_service_.GetObjectNameSynchronous(object_identifier);

  page_cloud_.status_to_return = cloud_provider::Status::NETWORK_ERROR;
  SetOnNewStateCallback([this] {
    if (states_.back() == DOWNLOAD_PERMANENT_ERROR) {
      QuitLoop();
    }
  });

  page_download_->StartDownload();

  bool called;
  ledger::Status status;
  storage::ChangeSource source;
  storage::IsObjectSynced is_object_synced;
  std::unique_ptr<storage::DataSource::DataChunk> data_chunk;
  storage_.page_sync_delegate_->GetObject(
      object_identifier, storage::ObjectType::BLOB,
      callback::Capture(callback::SetWhenCalled(&called), &status, &source, &is_object_synced,
                        &data_chunk));

  // Allow the operation to succeed after looping through five attempts.
  RunLoopFor(kTestBackoffInterval * 4);
  page_cloud_.status_to_return = cloud_provider::Status::OK;
  page_cloud_.objects_to_return[object_name] =
      encryption_service_.EncryptObjectSynchronous("content");
  RunLoopFor(kTestBackoffInterval);

  ASSERT_TRUE(called);
  EXPECT_EQ(page_cloud_.get_object_calls, 6u);
  EXPECT_EQ(status, ledger::Status::OK);
  EXPECT_EQ(source, storage::ChangeSource::CLOUD);
  EXPECT_EQ(data_chunk->Get().ToString(), "content");
  EXPECT_EQ(is_object_synced, storage::IsObjectSynced::YES);
}

class FailingDecryptCommitEncryptionService : public encryption::FakeEncryptionService {
 public:
  explicit FailingDecryptCommitEncryptionService(async_dispatcher_t* dispatcher)
      : encryption::FakeEncryptionService(dispatcher) {}

  void DecryptCommit(convert::ExtendedStringView /*storage_bytes*/,
                     fit::function<void(encryption::Status, std::string)> callback) override {
    callback(encryption::Status::INVALID_ARGUMENT, "");
  }
};

class FailingGetNameEncryptionService : public encryption::FakeEncryptionService {
 public:
  explicit FailingGetNameEncryptionService(async_dispatcher_t* dispatcher)
      : encryption::FakeEncryptionService(dispatcher) {}

  void GetObjectName(storage::ObjectIdentifier /*object_identifier*/,
                     fit::function<void(encryption::Status, std::string)> callback) override {
    callback(encryption::Status::INVALID_ARGUMENT, "");
  }
};

class FailingDecryptObjectEncryptionService : public encryption::FakeEncryptionService {
 public:
  explicit FailingDecryptObjectEncryptionService(async_dispatcher_t* dispatcher)
      : encryption::FakeEncryptionService(dispatcher) {}

  void DecryptObject(storage::ObjectIdentifier /*object_identifier*/,
                     std::string /*encrypted_data*/,
                     fit::function<void(encryption::Status, std::string)> callback) override {
    callback(encryption::Status::INVALID_ARGUMENT, "");
  }
};

using FailingDecryptCommitPageDownloadTest =
    BasePageDownloadTest<FailingDecryptCommitEncryptionService>;
TEST_F(FailingDecryptCommitPageDownloadTest, Fail) {
  EXPECT_EQ(storage_.received_commits.size(), 0u);
  EXPECT_EQ(storage_.sync_metadata.count(kTimestampKey.ToString()), 0u);

  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.commits_to_return.push_back(MakeTestCommit(&encryption_service_, "id2", "content2"));
  page_cloud_.position_token_to_return = fidl::MakeOptional(MakeToken("43"));

  EXPECT_FALSE(StartDownloadAndWaitForIdle());
  ASSERT_FALSE(states_.empty());
  EXPECT_EQ(states_.back(), DOWNLOAD_PERMANENT_ERROR);
}

template <typename E>
using FailingPageDownloadTest = BasePageDownloadTest<E>;

using FailingEncryptionServices =
    ::testing::Types<FailingGetNameEncryptionService, FailingDecryptObjectEncryptionService>;

TYPED_TEST_SUITE(FailingPageDownloadTest, FailingEncryptionServices);

TYPED_TEST(FailingPageDownloadTest, Fail) {
  storage::ObjectIdentifier object_identifier = MakeObjectIdentifier();
  std::string object_name = this->encryption_service_.GetObjectNameSynchronous(object_identifier);
  this->page_cloud_.objects_to_return[object_name] =
      this->encryption_service_.EncryptObjectSynchronous("content");
  this->page_download_->StartDownload();

  bool called;
  ledger::Status status;
  storage::ChangeSource source;
  storage::IsObjectSynced is_object_synced;
  std::unique_ptr<storage::DataSource::DataChunk> data_chunk;
  this->storage_.page_sync_delegate_->GetObject(
      object_identifier, storage::ObjectType::BLOB,
      callback::Capture(callback::SetWhenCalled(&called), &status, &source, &is_object_synced,
                        &data_chunk));
  this->RunLoopUntilIdle();

  ASSERT_TRUE(called);
  EXPECT_EQ(status, ledger::Status::IO_ERROR);
  EXPECT_EQ(source, storage::ChangeSource::CLOUD);
}

}  // namespace
}  // namespace cloud_sync
