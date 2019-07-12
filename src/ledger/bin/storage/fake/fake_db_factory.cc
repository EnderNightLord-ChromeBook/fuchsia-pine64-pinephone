// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/fake/fake_db_factory.h"

#include "src/ledger/bin/storage/fake/fake_db.h"
#include "src/lib/files/directory.h"

namespace storage {
namespace fake {

void FakeDbFactory::GetOrCreateDb(ledger::DetachedPath db_path,
                                  DbFactory::OnDbNotFound on_db_not_found,
                                  fit::function<void(Status, std::unique_ptr<Db>)> callback) {
  if (!files::IsDirectoryAt(db_path.root_fd(), db_path.path())) {
    if (on_db_not_found == DbFactory::OnDbNotFound::RETURN) {
      callback(Status::PAGE_NOT_FOUND, nullptr);
      return;
    }
    // Create the path to fake the creation of the Db at the expected
    // destination.
    if (!files::CreateDirectoryAt(db_path.root_fd(), db_path.path())) {
      FXL_LOG(ERROR) << "Failed to create the storage directory in " << db_path.path();
      callback(Status::INTERNAL_ERROR, nullptr);
      return;
    }
  }
  callback(Status::OK, std::make_unique<FakeDb>(dispatcher_));
}

}  // namespace fake
}  // namespace storage
