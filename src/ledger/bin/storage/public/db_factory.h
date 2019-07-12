// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_STORAGE_PUBLIC_DB_FACTORY_H_
#define SRC_LEDGER_BIN_STORAGE_PUBLIC_DB_FACTORY_H_

#include <memory>

#include "src/ledger/bin/filesystem/detached_path.h"
#include "src/ledger/bin/storage/public/db.h"

namespace storage {

// A factory for Db instances.
class DbFactory {
 public:
  // Defines the action to be taken if |GetOrCreate| is called for a path that
  // doesn't already contain a Db.
  enum class OnDbNotFound {
    // |GetOrCreateDb| should return with a |PAGE_NOT_FOUND| status.
    RETURN,
    // |GetOrCreateDb| should create a new Db instance.
    CREATE
  };

  DbFactory() {}
  virtual ~DbFactory() {}

  // Opens and returns an initialized instance of Db in the given |db_path|.
  // Depending on the value of |on_db_not_found|, if the Db doesn't already
  // exist, it either returns with NOT_FOUND status, or creates a new one.
  virtual void GetOrCreateDb(ledger::DetachedPath db_path, OnDbNotFound on_db_not_found,
                             fit::function<void(Status, std::unique_ptr<Db>)> callback) = 0;
};

}  // namespace storage

#endif  // SRC_LEDGER_BIN_STORAGE_PUBLIC_DB_FACTORY_H_
