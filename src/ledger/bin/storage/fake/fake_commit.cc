// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/fake/fake_commit.h"

#include <memory>
#include <string>

#include "src/ledger/bin/encryption/fake/fake_encryption_service.h"
#include "src/ledger/bin/storage/fake/fake_journal_delegate.h"
#include "src/ledger/bin/storage/public/commit.h"
#include "src/ledger/bin/storage/public/constants.h"
#include "src/ledger/bin/storage/public/iterator.h"

namespace storage {
namespace fake {

FakeCommit::FakeCommit(FakeJournalDelegate* journal) : journal_(journal) {}
FakeCommit::~FakeCommit() {}

std::unique_ptr<const Commit> FakeCommit::Clone() const {
  return std::make_unique<FakeCommit>(journal_);
}

const CommitId& FakeCommit::GetId() const { return journal_->GetId(); }

std::vector<CommitIdView> FakeCommit::GetParentIds() const { return journal_->GetParentIds(); }

zx::time_utc FakeCommit::GetTimestamp() const { return zx::time_utc(); }

uint64_t FakeCommit::GetGeneration() const { return journal_->GetGeneration(); }

ObjectIdentifier FakeCommit::GetRootIdentifier() const {
  // The object digest is fake here: using journal id is arbitrary.
  return encryption::MakeDefaultObjectIdentifier(ObjectDigest(journal_->GetId()));
}

fxl::StringView FakeCommit::GetStorageBytes() const { return fxl::StringView(); }

}  // namespace fake
}  // namespace storage
