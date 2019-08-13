// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/impl/btree/synchronous_storage.h"

#include <lib/fit/function.h>

#include "src/ledger/lib/coroutine/coroutine_waiter.h"
#include "src/lib/fxl/memory/ref_ptr.h"

namespace storage {
namespace btree {

SynchronousStorage::SynchronousStorage(PageStorage* page_storage,
                                       coroutine::CoroutineHandler* handler)
    : page_storage_(page_storage), handler_(handler) {}

Status SynchronousStorage::TreeNodeFromIdentifier(LocatedObjectIdentifier object_identifier,
                                                  std::unique_ptr<const TreeNode>* result) {
  Status status;
  if (coroutine::SyncCall(
          handler_,
          [this, &object_identifier](
              fit::function<void(Status, std::unique_ptr<const TreeNode>)> callback) {
            TreeNode::FromIdentifier(page_storage_, object_identifier, std::move(callback));
          },
          &status, result) == coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  return status;
}

Status SynchronousStorage::TreeNodeFromEntries(uint8_t level, const std::vector<Entry>& entries,
                                               const std::map<size_t, ObjectIdentifier>& children,
                                               ObjectIdentifier* result) {
  Status status;
  if (coroutine::SyncCall(
          handler_,
          [this, level, &entries,
           &children](fit::function<void(Status, ObjectIdentifier)> callback) {
            TreeNode::FromEntries(page_storage_, level, entries, children, std::move(callback));
          },
          &status, result) == coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  return status;
}

}  // namespace btree
}  // namespace storage
