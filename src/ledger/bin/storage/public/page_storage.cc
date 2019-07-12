// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/public/page_storage.h"

namespace storage {

PageStorage::CommitIdAndBytes::CommitIdAndBytes() {}

PageStorage::CommitIdAndBytes::CommitIdAndBytes(CommitId id, std::string bytes)
    : id(std::move(id)), bytes(std::move(bytes)) {}

PageStorage::CommitIdAndBytes::CommitIdAndBytes(CommitIdAndBytes&& other) noexcept = default;

PageStorage::CommitIdAndBytes& PageStorage::CommitIdAndBytes::operator=(
    CommitIdAndBytes&& other) noexcept = default;

}  // namespace storage
