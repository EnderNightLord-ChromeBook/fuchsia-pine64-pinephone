// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/impl/leveldb.h"

#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>
#include <trace/event.h>

#include <utility>

#include "src/ledger/bin/cobalt/cobalt.h"
#include "src/ledger/bin/storage/impl/object_impl.h"
#include "src/ledger/lib/coroutine/coroutine.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/path.h"
#include "src/lib/fxl/logging.h"
#include "util/env_fuchsia.h"

namespace storage {

using coroutine::CoroutineHandler;

namespace {

Status MakeEmptySyncCallAndCheck(async_dispatcher_t* dispatcher,
                                 coroutine::CoroutineHandler* handler) {
  if (coroutine::SyncCall(handler, [&dispatcher](fit::closure on_done) {
        async::PostTask(dispatcher, std::move(on_done));
      }) == coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  return Status::OK;
}

Status ConvertStatus(leveldb::Status s) {
  if (s.IsNotFound()) {
    return Status::INTERNAL_NOT_FOUND;
  }
  if (!s.ok()) {
    FXL_LOG(ERROR) << "LevelDB error: " << s.ToString();
    return Status::INTERNAL_ERROR;
  }
  return Status::OK;
}

class BatchImpl : public Db::Batch {
 public:
  // Creates a new Batch based on a leveldb batch. Once |Execute| is called,
  // |callback| will be called with the same batch, ready to be written in
  // leveldb. If the destructor is called without a previous execution of the
  // batch, |callback| will be called with a |nullptr| and must return OK.
  BatchImpl(async_dispatcher_t* dispatcher, std::unique_ptr<leveldb::WriteBatch> batch,
            fit::function<Status(std::unique_ptr<leveldb::WriteBatch>)> callback)
      : dispatcher_(dispatcher), batch_(std::move(batch)), callback_(std::move(callback)) {}

  ~BatchImpl() override {
    if (batch_) {
      Status status = callback_(nullptr);
      FXL_DCHECK(status == Status::OK);
    }
  }

  Status Put(CoroutineHandler* handler, convert::ExtendedStringView key,
             fxl::StringView value) override {
    FXL_DCHECK(batch_);
    if (MakeEmptySyncCallAndCheck(dispatcher_, handler) == Status::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    batch_->Put(key, convert::ToSlice(value));
    return Status::OK;
  }

  Status Delete(CoroutineHandler* handler, convert::ExtendedStringView key) override {
    FXL_DCHECK(batch_);
    batch_->Delete(key);
    return MakeEmptySyncCallAndCheck(dispatcher_, handler);
  }

  Status Execute(CoroutineHandler* handler) override {
    FXL_DCHECK(batch_);
    if (MakeEmptySyncCallAndCheck(dispatcher_, handler) == Status::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    return callback_(std::move(batch_));
  }

 private:
  async_dispatcher_t* const dispatcher_;
  std::unique_ptr<leveldb::WriteBatch> batch_;

  fit::function<Status(std::unique_ptr<leveldb::WriteBatch>)> callback_;
};

class RowIterator
    : public Iterator<const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>> {
 public:
  RowIterator(std::unique_ptr<leveldb::Iterator> it, std::string prefix)
      : it_(std::move(it)), prefix_(std::move(prefix)) {
    PrepareEntry();
  }

  ~RowIterator() override {}

  Iterator<const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>>& Next()
      override {
    it_->Next();
    PrepareEntry();
    return *this;
  }

  bool Valid() const final { return it_->Valid() && it_->key().starts_with(prefix_); }

  Status GetStatus() const override {
    return it_->status().ok() ? Status::OK : Status::INTERNAL_ERROR;
  }

  const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>& operator*()
      const override {
    return *(row_.get());
  }

  const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>* operator->()
      const override {
    return row_.get();
  }

 private:
  void PrepareEntry() {
    if (!Valid()) {
      row_.reset(nullptr);
      return;
    }
    row_ = std::make_unique<std::pair<convert::ExtendedStringView, convert::ExtendedStringView>>(
        it_->key(), it_->value());
  }

  std::unique_ptr<leveldb::Iterator> it_;
  const std::string prefix_;

  std::unique_ptr<std::pair<convert::ExtendedStringView, convert::ExtendedStringView>> row_;
};

}  // namespace

LevelDb::LevelDb(async_dispatcher_t* dispatcher, ledger::DetachedPath db_path)
    : dispatcher_(dispatcher), db_path_(std::move(db_path)) {}

LevelDb::~LevelDb() {
  FXL_DCHECK(!active_batches_count_)
      << "Not all LevelDb batches have been executed or rolled back.";
}

Status LevelDb::Init() {
  TRACE_DURATION("ledger", "leveldb_init");
  if (!files::CreateDirectoryAt(db_path_.root_fd(), db_path_.path())) {
    FXL_LOG(ERROR) << "Failed to create directory under " << db_path_.path();
    return Status::INTERNAL_ERROR;
  }
  fxl::UniqueFD unique_fd;
  ledger::DetachedPath db_path = db_path_;
  if (db_path_.path() != ".") {
    // Open a UniqueFD at the db path.
    unique_fd = db_path_.OpenFD(&db_path);
    if (!unique_fd.is_valid()) {
      FXL_LOG(ERROR) << "Unable to open directory at " << db_path_.path() << ". errno: " << errno;
      return Status::INTERNAL_ERROR;
    }
  }
  env_ = leveldb::MakeFuchsiaEnv(db_path.root_fd());
  leveldb::Options options;
  options.env = env_.get();
  options.create_if_missing = true;
  leveldb::DB* db = nullptr;
  leveldb::Status status = leveldb::DB::Open(options, db_path.path(), &db);
  if (status.IsCorruption()) {
    FXL_LOG(ERROR) << "Ledger state corrupted at " << db_path_.path()
                   << " with leveldb status: " << status.ToString();
    FXL_LOG(WARNING) << "Trying to recover by erasing the local state.";
    FXL_LOG(WARNING) << "***** ALL LOCAL CHANGES IN THIS PAGE WILL BE LOST *****";
    ledger::ReportEvent(ledger::CobaltEvent::LEDGER_LEVELDB_STATE_CORRUPTED);

    if (!files::DeletePathAt(db_path_.root_fd(), db_path_.path(), true)) {
      FXL_LOG(ERROR) << "Failed to delete corrupted ledger at " << db_path_.path();
      return Status::INTERNAL_ERROR;
    }
    leveldb::Status status = leveldb::DB::Open(options, db_path.path(), &db);
    if (!status.ok()) {
      FXL_LOG(ERROR) << "Failed to create a new LevelDB at " << db_path_.path()
                     << " with leveldb status: " << status.ToString();
      return Status::INTERNAL_ERROR;
    }
  } else if (!status.ok()) {
    FXL_LOG(ERROR) << "Failed to open ledger at " << db_path_.path()
                   << " with leveldb status: " << status.ToString();
    return Status::INTERNAL_ERROR;
  }
  db_.reset(db);
  return Status::OK;
}

Status LevelDb::StartBatch(CoroutineHandler* handler, std::unique_ptr<Db::Batch>* batch) {
  auto db_batch = std::make_unique<leveldb::WriteBatch>();
  active_batches_count_++;
  *batch = std::make_unique<BatchImpl>(
      dispatcher_, std::move(db_batch), [this](std::unique_ptr<leveldb::WriteBatch> db_batch) {
        active_batches_count_--;
        if (db_batch) {
          leveldb::Status status = db_->Write(write_options_, db_batch.get());
          if (!status.ok()) {
            FXL_LOG(ERROR) << "Failed to execute batch with status: " << status.ToString();
            return Status::INTERNAL_ERROR;
          }
        }
        return Status::OK;
      });
  return MakeEmptySyncCallAndCheck(dispatcher_, handler);
}

Status LevelDb::Get(CoroutineHandler* handler, convert::ExtendedStringView key,
                    std::string* value) {
  if (MakeEmptySyncCallAndCheck(dispatcher_, handler) == Status::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  return ConvertStatus(db_->Get(read_options_, key, value));
}

Status LevelDb::HasKey(CoroutineHandler* handler, convert::ExtendedStringView key) {
  std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(read_options_));
  iterator->Seek(key);

  if (!iterator->Valid() || iterator->key() != key) {
    return Status::INTERNAL_NOT_FOUND;
  }
  return MakeEmptySyncCallAndCheck(dispatcher_, handler);
}

Status LevelDb::GetObject(CoroutineHandler* handler, convert::ExtendedStringView key,
                          ObjectIdentifier object_identifier, std::unique_ptr<const Piece>* piece) {
  FXL_DCHECK(piece);
  std::unique_ptr<leveldb::Iterator> iterator(db_->NewIterator(read_options_));
  iterator->Seek(key);

  if (!iterator->Valid() || iterator->key() != key) {
    return Status::INTERNAL_NOT_FOUND;
  }

  *piece = std::make_unique<LevelDBPiece>(std::move(object_identifier), std::move(iterator));
  return MakeEmptySyncCallAndCheck(dispatcher_, handler);
}

Status LevelDb::GetByPrefix(CoroutineHandler* handler, convert::ExtendedStringView prefix,
                            std::vector<std::string>* key_suffixes) {
  std::vector<std::string> result;
  std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options_));
  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    leveldb::Slice key = it->key();
    key.remove_prefix(prefix.size());
    result.push_back(key.ToString());
  }
  if (!it->status().ok()) {
    return ConvertStatus(it->status());
  }
  key_suffixes->swap(result);
  return MakeEmptySyncCallAndCheck(dispatcher_, handler);
}

Status LevelDb::GetEntriesByPrefix(CoroutineHandler* handler, convert::ExtendedStringView prefix,
                                   std::vector<std::pair<std::string, std::string>>* entries) {
  std::vector<std::pair<std::string, std::string>> result;
  std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options_));
  for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
    leveldb::Slice key = it->key();
    key.remove_prefix(prefix.size());
    result.emplace_back(key.ToString(), it->value().ToString());
  }
  if (!it->status().ok()) {
    return ConvertStatus(it->status());
  }
  entries->swap(result);
  return MakeEmptySyncCallAndCheck(dispatcher_, handler);
}

Status LevelDb::GetIteratorAtPrefix(
    CoroutineHandler* handler, convert::ExtendedStringView prefix,
    std::unique_ptr<
        Iterator<const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>>>*
        iterator) {
  std::unique_ptr<leveldb::Iterator> local_iterator(db_->NewIterator(read_options_));
  local_iterator->Seek(prefix);

  if (iterator) {
    std::unique_ptr<
        Iterator<const std::pair<convert::ExtendedStringView, convert::ExtendedStringView>>>
        row_iterator = std::make_unique<RowIterator>(std::move(local_iterator), prefix.ToString());
    iterator->swap(row_iterator);
  }
  return MakeEmptySyncCallAndCheck(dispatcher_, handler);
}

}  // namespace storage
