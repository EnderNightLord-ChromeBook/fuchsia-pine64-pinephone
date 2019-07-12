// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_IMPL_COMMIT_IMPL_H_
#define SRC_LEDGER_BIN_STORAGE_IMPL_COMMIT_IMPL_H_

#include <lib/fit/function.h>
#include <lib/timekeeper/clock.h>

#include <memory>
#include <string>
#include <vector>

#include "src/ledger/bin/storage/impl/live_commit_tracker.h"
#include "src/ledger/bin/storage/public/commit.h"
#include "src/ledger/bin/storage/public/page_storage.h"
#include "src/lib/fxl/memory/ref_ptr.h"

namespace storage {

class CommitImpl : public Commit {
 private:
  // Passkey idiom to restrict access to the constructor to static factories.
  class Token;
  class SharedStorageBytes;

 public:
  // Creates a new |CommitImpl| object with the given contents.
  CommitImpl(Token token, CommitId id, zx::time_utc timestamp, uint64_t generation,
             ObjectIdentifier root_node_identifier, std::vector<CommitIdView> parent_ids,
             fxl::RefPtr<SharedStorageBytes> storage_bytes, LiveCommitTracker* tracker);

  ~CommitImpl() override;

  // Factory method for creating a |CommitImpl| object given its storage
  // representation. If the format is incorrect, |nullptr| will be returned.
  static Status FromStorageBytes(LiveCommitTracker* tracker, CommitId id, std::string storage_bytes,
                                 std::unique_ptr<const Commit>* commit);

  static std::unique_ptr<const Commit> FromContentAndParents(
      LiveCommitTracker* tracker, timekeeper::Clock* clock, ObjectIdentifier root_node_identifier,
      std::vector<std::unique_ptr<const Commit>> parent_commits);

  // Factory method for creating an empty |CommitImpl| object, i.e. without
  // parents and with empty contents.
  static void Empty(PageStorage* page_storage, LiveCommitTracker* tracker,
                    fit::function<void(Status, std::unique_ptr<const Commit>)> callback);

  // Commit:
  std::unique_ptr<const Commit> Clone() const override;
  const CommitId& GetId() const override;
  std::vector<CommitIdView> GetParentIds() const override;
  zx::time_utc GetTimestamp() const override;
  uint64_t GetGeneration() const override;
  ObjectIdentifier GetRootIdentifier() const override;
  fxl::StringView GetStorageBytes() const override;

 private:
  class Token {
   private:
    Token() {}
    friend CommitImpl;
  };

  const CommitId id_;
  const zx::time_utc timestamp_;
  const uint64_t generation_;
  const ObjectIdentifier root_node_identifier_;
  const std::vector<CommitIdView> parent_ids_;
  const fxl::RefPtr<SharedStorageBytes> storage_bytes_;
  LiveCommitTracker* const tracker_;
};

}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_IMPL_COMMIT_IMPL_H_
