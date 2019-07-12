// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_APP_PAGE_USAGE_DB_H_
#define SRC_LEDGER_BIN_APP_PAGE_USAGE_DB_H_

#include <lib/callback/operation_serializer.h>
#include <lib/timekeeper/clock.h>

#include <functional>
#include <memory>

#include "src/ledger/bin/app/page_utils.h"
#include "src/ledger/bin/app/types.h"
#include "src/ledger/bin/storage/impl/leveldb.h"
#include "src/ledger/bin/storage/public/db.h"
#include "src/ledger/bin/storage/public/iterator.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/ledger/lib/coroutine/coroutine.h"
#include "src/lib/fxl/strings/concatenate.h"

namespace ledger {

// |PageUsageDb| persists all information on page usage.
//
// Calls to |MarkPageOpened| and |MarkPageClosed| will update the underlying
// database in the order in which they are called.
//
// Rows in the underlying database are serialized as follows:
// Last usage row:
// - Key: "opened/<ledger_name><page_id>"
// - Value: "<timestamp>" or timestamp 0 for open pages
class PageUsageDb {
 public:
  PageUsageDb(timekeeper::Clock* clock, std::unique_ptr<storage::Db> db);
  ~PageUsageDb();

  // Marks the page with the given id as opened. |INTERNAL_ERROR| is returned if
  // the operation is interrupted.
  Status MarkPageOpened(coroutine::CoroutineHandler* handler, fxl::StringView ledger_name,
                        storage::PageIdView page_id);

  // Marks the page with the given id as closed. |INTERNAL_ERROR| is returned if
  // the operation is interrupted.
  Status MarkPageClosed(coroutine::CoroutineHandler* handler, fxl::StringView ledger_name,
                        storage::PageIdView page_id);

  // Marks the page with the given id as evicted. |INTERNAL_ERROR| is returned
  // if the operation is interrupted.
  Status MarkPageEvicted(coroutine::CoroutineHandler* handler, fxl::StringView ledger_name,
                         storage::PageIdView page_id);

  // Marks all open pages as closed. |INTERNAL_ERROR| is returned if the
  // operation is interrupted.
  Status MarkAllPagesClosed(coroutine::CoroutineHandler* handler);

  // Updates |pages| to contain an iterator over all entries of page
  // information.
  Status GetPages(coroutine::CoroutineHandler* handler,
                  std::unique_ptr<storage::Iterator<const PageInfo>>* pages);

 private:
  // Inserts the given |key|-|value| pair in the underlying database.
  Status Put(coroutine::CoroutineHandler* handler, fxl::StringView key, fxl::StringView value);

  // Deletes the row with the given |key| in the underlying database.
  Status Delete(coroutine::CoroutineHandler* handler, fxl::StringView key);

  timekeeper::Clock* clock_;
  std::unique_ptr<storage::Db> db_;

  // A serializer used for Put and Delete. Both these operations need to be
  // serialized to guarantee that consecutive calls to update the contents of a
  // single page (e.g. a page is opened and then closed) are written in |db_| in
  // the right order, i.e. the order in which they were called.
  callback::OperationSerializer serializer_;

  FXL_DISALLOW_COPY_AND_ASSIGN(PageUsageDb);
};

}  // namespace ledger

#endif  // SRC_LEDGER_BIN_APP_PAGE_USAGE_DB_H_
