// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/batch_download.h"

#include <lib/async/cpp/task.h>
#include <lib/callback/capture.h>
#include <lib/fit/function.h>
#include <lib/gtest/test_loop_fixture.h>

#include <map>

#include "gtest/gtest.h"
#include "src/ledger/bin/cloud_sync/impl/constants.h"
#include "src/ledger/bin/cloud_sync/impl/testing/test_page_cloud.h"
#include "src/ledger/bin/encryption/fake/fake_encryption_service.h"
#include "src/ledger/bin/storage/testing/page_storage_empty_impl.h"
#include "src/lib/fxl/macros.h"

namespace cloud_sync {

namespace {

// Creates a dummy continuation token.
std::unique_ptr<cloud_provider::PositionToken> MakeToken(convert::ExtendedStringView token_id) {
  auto token = std::make_unique<cloud_provider::PositionToken>();
  token->opaque_id = convert::ToArray(token_id);
  return token;
}

// Fake implementation of storage::PageStorage. Injects the data that
// CommitUpload asks about: page id and unsynced objects to be uploaded.
// Registers the reported results of the upload: commits and objects marked as
// synced.
class TestPageStorage : public storage::PageStorageEmptyImpl {
 public:
  explicit TestPageStorage(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

  void AddCommitsFromSync(
      std::vector<storage::PageStorage::CommitIdAndBytes> ids_and_bytes,
      storage::ChangeSource source,
      fit::function<void(ledger::Status, std::vector<storage::CommitId>)> callback) override {
    ASSERT_EQ(storage::ChangeSource::CLOUD, source);
    if (should_fail_add_commit_from_sync) {
      async::PostTask(dispatcher_, [callback = std::move(callback)]() {
        callback(ledger::Status::IO_ERROR, {});
      });
      return;
    }
    async::PostTask(dispatcher_, [this, ids_and_bytes = std::move(ids_and_bytes),
                                  callback = std::move(callback)]() mutable {
      for (auto& commit : ids_and_bytes) {
        received_commits[std::move(commit.id)] = std::move(commit.bytes);
      }
      callback(ledger::Status::OK, {});
    });
  }

  void SetSyncMetadata(fxl::StringView key, fxl::StringView value,
                       fit::function<void(ledger::Status)> callback) override {
    sync_metadata[key.ToString()] = value.ToString();
    async::PostTask(dispatcher_,
                    [callback = std::move(callback)]() { callback(ledger::Status::OK); });
  }

  bool should_fail_add_commit_from_sync = false;
  std::map<storage::CommitId, std::string> received_commits;
  std::map<std::string, std::string> sync_metadata;

 private:
  async_dispatcher_t* const dispatcher_;
};

class BatchDownloadTest : public gtest::TestLoopFixture {
 public:
  BatchDownloadTest() : storage_(dispatcher()), encryption_service_(dispatcher()) {}
  ~BatchDownloadTest() override {}

 protected:
  TestPageStorage storage_;
  encryption::FakeEncryptionService encryption_service_;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(BatchDownloadTest);
};

TEST_F(BatchDownloadTest, AddCommit) {
  int done_calls = 0;
  int error_calls = 0;
  std::vector<cloud_provider::CommitPackEntry> entries;
  entries.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  BatchDownload batch_download(
      &storage_, &encryption_service_, std::move(entries), MakeToken("42"),
      [&done_calls] { done_calls++; }, [&error_calls] { error_calls++; });
  batch_download.Start();

  RunLoopUntilIdle();
  EXPECT_EQ(1, done_calls);
  EXPECT_EQ(0, error_calls);
  EXPECT_EQ(1u, storage_.received_commits.size());
  EXPECT_EQ("content1", storage_.received_commits["id1"]);
  EXPECT_EQ("42", storage_.sync_metadata[kTimestampKey.ToString()]);
}

TEST_F(BatchDownloadTest, AddMultipleCommits) {
  int done_calls = 0;
  int error_calls = 0;
  std::vector<cloud_provider::CommitPackEntry> entries;
  entries.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  entries.push_back(MakeTestCommit(&encryption_service_, "id2", "content2"));
  BatchDownload batch_download(
      &storage_, &encryption_service_, std::move(entries), MakeToken("43"),
      [&done_calls] { done_calls++; }, [&error_calls] { error_calls++; });
  batch_download.Start();

  RunLoopUntilIdle();
  EXPECT_EQ(1, done_calls);
  EXPECT_EQ(0, error_calls);
  EXPECT_EQ(2u, storage_.received_commits.size());
  EXPECT_EQ("content1", storage_.received_commits["id1"]);
  EXPECT_EQ("content2", storage_.received_commits["id2"]);
  EXPECT_EQ("43", storage_.sync_metadata[kTimestampKey.ToString()]);
}

TEST_F(BatchDownloadTest, FailToAddCommit) {
  int done_calls = 0;
  int error_calls = 0;
  std::vector<cloud_provider::CommitPackEntry> entries;
  entries.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  BatchDownload batch_download(
      &storage_, &encryption_service_, std::move(entries), MakeToken("42"),
      [&done_calls] { done_calls++; }, [&error_calls] { error_calls++; });
  storage_.should_fail_add_commit_from_sync = true;
  batch_download.Start();

  RunLoopUntilIdle();
  EXPECT_EQ(0, done_calls);
  EXPECT_EQ(1, error_calls);
  EXPECT_TRUE(storage_.received_commits.empty());
  EXPECT_EQ(0u, storage_.sync_metadata.count(kTimestampKey.ToString()));
}

}  // namespace

}  // namespace cloud_sync
