// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/impl/page_storage_impl.h"

#include <errno.h>
#include <fcntl.h>
#include <lib/callback/scoped_callback.h>
#include <lib/callback/trace_callback.h>
#include <lib/callback/waiter.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <utility>

#include <trace/event.h>

#include "src/ledger/bin/cobalt/cobalt.h"
#include "src/ledger/bin/storage/impl/btree/diff.h"
#include "src/ledger/bin/storage/impl/btree/iterator.h"
#include "src/ledger/bin/storage/impl/commit_factory.h"
#include "src/ledger/bin/storage/impl/constants.h"
#include "src/ledger/bin/storage/impl/file_index.h"
#include "src/ledger/bin/storage/impl/file_index_generated.h"
#include "src/ledger/bin/storage/impl/journal_impl.h"
#include "src/ledger/bin/storage/impl/object_digest.h"
#include "src/ledger/bin/storage/impl/object_identifier_encoding.h"
#include "src/ledger/bin/storage/impl/object_identifier_factory_impl.h"
#include "src/ledger/bin/storage/impl/object_impl.h"
#include "src/ledger/bin/storage/impl/split.h"
#include "src/ledger/bin/storage/public/constants.h"
#include "src/ledger/bin/storage/public/object.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/ledger/bin/synchronization/lock.h"
#include "src/ledger/lib/coroutine/coroutine_waiter.h"
#include "src/lib/files/directory.h"
#include "src/lib/files/file.h"
#include "src/lib/files/file_descriptor.h"
#include "src/lib/files/path.h"
#include "src/lib/files/unique_fd.h"
#include "src/lib/fxl/arraysize.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/lib/fxl/strings/concatenate.h"
#include "src/lib/fxl/strings/string_view.h"

namespace storage {
namespace {

using ::coroutine::CoroutineHandler;

struct StringPointerComparator {
  using is_transparent = std::true_type;

  bool operator()(const std::string* str1, const std::string* str2) const { return *str1 < *str2; }

  bool operator()(const std::string* str1, const CommitIdView* str2) const { return *str1 < *str2; }

  bool operator()(const CommitIdView* str1, const std::string* str2) const { return *str1 < *str2; }
};

// Converts the user-provided offset for an object part (defined in comments for
// |FetchPartial| in ledger.fidl) to the actual offset used for reading. If the
// offset is off-limits, returns the |object_size|.
int64_t GetObjectPartStart(int64_t offset, int64_t object_size) {
  // Valid indices are between -N and N-1.
  if (offset < -object_size || offset >= object_size) {
    return object_size;
  }
  return offset < 0 ? object_size + offset : offset;
}

int64_t GetObjectPartLength(int64_t max_size, int64_t object_size, int64_t start) {
  int64_t adjusted_max_size = max_size < 0 ? object_size : max_size;
  return start > object_size ? 0 : std::min(adjusted_max_size, object_size - start);
}

}  // namespace

PageStorageImpl::PageStorageImpl(ledger::Environment* environment,
                                 encryption::EncryptionService* encryption_service,
                                 std::unique_ptr<Db> db, PageId page_id, CommitPruningPolicy policy)
    : PageStorageImpl(
          environment, encryption_service,
          std::make_unique<PageDbImpl>(environment, &object_identifier_factory_, std::move(db)),
          std::move(page_id), policy) {}

PageStorageImpl::PageStorageImpl(ledger::Environment* environment,
                                 encryption::EncryptionService* encryption_service,
                                 std::unique_ptr<PageDb> page_db, PageId page_id,
                                 CommitPruningPolicy policy)
    : environment_(environment),
      encryption_service_(encryption_service),
      page_id_(std::move(page_id)),
      commit_factory_(&object_identifier_factory_),
      commit_pruner_(environment_, this, &commit_factory_, policy),
      db_(std::move(page_db)),
      page_sync_(nullptr),
      coroutine_manager_(environment->coroutine_service()),
      weak_factory_(this) {}

PageStorageImpl::~PageStorageImpl() {}

void PageStorageImpl::Init(fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback), [this](CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(SynchronousInit(handler));
      });
}

PageId PageStorageImpl::GetId() { return page_id_; }

void PageStorageImpl::SetSyncDelegate(PageSyncDelegate* page_sync) { page_sync_ = page_sync; }

Status PageStorageImpl::GetHeadCommits(std::vector<std::unique_ptr<const Commit>>* head_commits) {
  FXL_DCHECK(head_commits);
  *head_commits = commit_factory_.GetHeads();
  return Status::OK;
}

void PageStorageImpl::GetMergeCommitIds(
    CommitIdView parent1_id, CommitIdView parent2_id,
    fit::function<void(Status, std::vector<CommitId>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, parent1_id = parent1_id.ToString(), parent2_id = parent2_id.ToString()](
          CoroutineHandler* handler, fit::function<void(Status, std::vector<CommitId>)> callback) {
        std::vector<CommitId> commit_ids;
        Status status = db_->GetMerges(handler, parent1_id, parent2_id, &commit_ids);
        callback(status, std::move(commit_ids));
      });
}

void PageStorageImpl::GetCommit(
    CommitIdView commit_id, fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
  FXL_DCHECK(commit_id.size());
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, commit_id = commit_id.ToString()](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
        std::unique_ptr<const Commit> commit;
        Status status = SynchronousGetCommit(handler, commit_id, &commit);
        callback(status, std::move(commit));
      });
}

void PageStorageImpl::AddCommitsFromSync(
    std::vector<CommitIdAndBytes> ids_and_bytes, ChangeSource source,
    fit::function<void(Status, std::vector<CommitId>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, ids_and_bytes = std::move(ids_and_bytes), source](
          CoroutineHandler* handler,
          fit::function<void(Status, std::vector<CommitId>)> callback) mutable {
        std::vector<CommitId> missing_ids;
        Status status =
            SynchronousAddCommitsFromSync(handler, std::move(ids_and_bytes), source, &missing_ids);
        callback(status, std::move(missing_ids));
      });
}

std::unique_ptr<Journal> PageStorageImpl::StartCommit(std::unique_ptr<const Commit> commit) {
  return JournalImpl::Simple(environment_, this, std::move(commit));
}

std::unique_ptr<Journal> PageStorageImpl::StartMergeCommit(std::unique_ptr<const Commit> left,
                                                           std::unique_ptr<const Commit> right) {
  return JournalImpl::Merge(environment_, this, std::move(left), std::move(right));
}

void PageStorageImpl::CommitJournal(
    std::unique_ptr<Journal> journal,
    fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
  FXL_DCHECK(journal);

  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, journal = std::move(journal)](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<const Commit>)> callback) mutable {
        JournalImpl* journal_ptr = static_cast<JournalImpl*>(journal.get());

        std::unique_ptr<const Commit> commit;
        std::vector<ObjectIdentifier> objects_to_sync;
        Status status = journal_ptr->Commit(handler, &commit, &objects_to_sync);
        if (status != Status::OK || !commit) {
          // There is an error, or the commit is empty (no change).
          callback(status, nullptr);
          return;
        }

        status =
            SynchronousAddCommitFromLocal(handler, commit->Clone(), std::move(objects_to_sync));

        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        callback(status, std::move(commit));
      });
}

void PageStorageImpl::DeleteCommits(std::vector<std::unique_ptr<const Commit>> commits,
                                    fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback), [this, commits = std::move(commits)](
                               CoroutineHandler* handler, fit::function<void(Status)> callback) {
        std::unique_ptr<PageDb::Batch> batch;
        Status status = db_->StartBatch(handler, &batch);
        if (status != Status::OK) {
          callback(status);
          return;
        }
        for (const std::unique_ptr<const Commit>& commit : commits) {
          auto parents = commit->GetParentIds();
          if (parents.size() > 1) {
            status = batch->DeleteMerge(handler, parents[0], parents[1], commit->GetId());
            if (status != Status::OK) {
              callback(status);
              return;
            }
          }
          status = batch->DeleteCommit(handler, commit->GetId(), commit->GetRootIdentifier());
          if (status != Status::OK) {
            callback(status);
            return;
          }
        }
        callback(batch->Execute(handler));
      });
}

void PageStorageImpl::AddCommitWatcher(CommitWatcher* watcher) { watchers_.AddObserver(watcher); }

void PageStorageImpl::RemoveCommitWatcher(CommitWatcher* watcher) {
  watchers_.RemoveObserver(watcher);
}

void PageStorageImpl::IsSynced(fit::function<void(Status, bool)> callback) {
  auto waiter = fxl::MakeRefCounted<callback::Waiter<Status, bool>>(Status::OK);
  // Check for unsynced commits.
  coroutine_manager_.StartCoroutine(
      waiter->NewCallback(),
      [this](CoroutineHandler* handler, fit::function<void(Status, bool)> callback) {
        std::vector<CommitId> commit_ids;
        Status status = db_->GetUnsyncedCommitIds(handler, &commit_ids);
        if (status != Status::OK) {
          callback(status, false);
        } else {
          callback(Status::OK, commit_ids.empty());
        }
      });

  // Check for unsynced pieces.
  GetUnsyncedPieces([pieces_callback = waiter->NewCallback()](
                        Status status, std::vector<ObjectIdentifier> pieces) {
    if (status != Status::OK) {
      pieces_callback(status, false);
    } else {
      pieces_callback(Status::OK, pieces.empty());
    }
  });

  waiter->Finalize([callback = std::move(callback)](Status status, std::vector<bool> is_synced) {
    if (status != Status::OK) {
      callback(status, false);
      return;
    }
    FXL_DCHECK(is_synced.size() == 2);
    callback(Status::OK, is_synced[0] && is_synced[1]);
  });
}

bool PageStorageImpl::IsOnline() { return page_is_online_; }

void PageStorageImpl::IsEmpty(fit::function<void(Status, bool)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](CoroutineHandler* handler, fit::function<void(Status, bool)> callback) {
        // Check there is a single head.
        std::vector<std::pair<zx::time_utc, CommitId>> commit_ids;
        Status status = db_->GetHeads(handler, &commit_ids);
        if (status != Status::OK) {
          callback(status, false);
          return;
        }
        FXL_DCHECK(!commit_ids.empty());
        if (commit_ids.size() > 1) {
          // A page is not empty if there is more than one head commit.
          callback(Status::OK, false);
          return;
        }
        // Compare the root node of the head commit to that of the empty node.
        std::unique_ptr<const Commit> commit;
        status = SynchronousGetCommit(handler, commit_ids[0].second, &commit);
        if (status != Status::OK) {
          callback(status, false);
          return;
        }
        ObjectIdentifier* empty_node_id;
        status = SynchronousGetEmptyNodeIdentifier(handler, &empty_node_id);
        if (status != Status::OK) {
          callback(status, false);
          return;
        }
        callback(Status::OK, commit->GetRootIdentifier() == *empty_node_id);
      });
}

void PageStorageImpl::GetUnsyncedCommits(
    fit::function<void(Status, std::vector<std::unique_ptr<const Commit>>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](CoroutineHandler* handler,
             fit::function<void(Status, std::vector<std::unique_ptr<const Commit>>)> callback) {
        std::vector<std::unique_ptr<const Commit>> unsynced_commits;
        Status s = SynchronousGetUnsyncedCommits(handler, &unsynced_commits);
        callback(s, std::move(unsynced_commits));
      });
}

void PageStorageImpl::MarkCommitSynced(const CommitId& commit_id,
                                       fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, commit_id](CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(SynchronousMarkCommitSynced(handler, commit_id));
      });
}

void PageStorageImpl::GetUnsyncedPieces(
    fit::function<void(Status, std::vector<ObjectIdentifier>)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this](CoroutineHandler* handler,
             fit::function<void(Status, std::vector<ObjectIdentifier>)> callback) {
        std::vector<ObjectIdentifier> unsynced_object_identifiers;
        Status s = db_->GetUnsyncedPieces(handler, &unsynced_object_identifiers);
        callback(s, unsynced_object_identifiers);
      });
}

void PageStorageImpl::MarkPieceSynced(ObjectIdentifier object_identifier,
                                      fit::function<void(Status)> callback) {
  FXL_DCHECK(IsTokenValid(object_identifier));
  coroutine_manager_.StartCoroutine(
      std::move(callback), [this, object_identifier = std::move(object_identifier)](
                               CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(db_->SetObjectStatus(handler, object_identifier, PageDbObjectStatus::SYNCED));
      });
}

void PageStorageImpl::IsPieceSynced(ObjectIdentifier object_identifier,
                                    fit::function<void(Status, bool)> callback) {
  FXL_DCHECK(IsTokenValid(object_identifier));
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler, fit::function<void(Status, bool)> callback) {
        PageDbObjectStatus object_status;
        Status status = db_->GetObjectStatus(handler, object_identifier, &object_status);
        callback(status, object_status == PageDbObjectStatus::SYNCED);
      });
}

void PageStorageImpl::MarkSyncedToPeer(fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      [this, callback = std::move(callback)](CoroutineHandler* handler) {
        std::unique_ptr<PageDb::Batch> batch;
        Status status = db_->StartBatch(handler, &batch);
        if (status != Status::OK) {
          callback(status);
          return;
        }
        status = SynchronousMarkPageOnline(handler, batch.get());
        if (status != Status::OK) {
          callback(status);
          return;
        }
        callback(batch->Execute(handler));
      });
}

void PageStorageImpl::AddObjectFromLocal(ObjectType object_type,
                                         std::unique_ptr<DataSource> data_source,
                                         ObjectReferencesAndPriority tree_references,
                                         fit::function<void(Status, ObjectIdentifier)> callback) {
  // |data_source| is not splitted yet: |tree_references| must contain only
  // BTree-level references, not piece-level references, and only in the case
  // where |data_source| actually represents a tree node.
  FXL_DCHECK(object_type == ObjectType::TREE_NODE || tree_references.empty());
  auto traced_callback = TRACE_CALLBACK(std::move(callback), "ledger", "page_storage_add_object");

  auto managed_data_source = managed_container_.Manage(std::move(data_source));
  auto managed_data_source_ptr = managed_data_source->get();
  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  encryption_service_->GetChunkingPermutation(
      [this, waiter, managed_data_source = std::move(managed_data_source),
       tree_references = std::move(tree_references), managed_data_source_ptr, object_type,
       callback = std::move(traced_callback)](
          encryption::Status status,
          fit::function<uint64_t(uint64_t)> chunking_permutation) mutable {
        if (status != encryption::Status::OK) {
          callback(Status::INTERNAL_ERROR, ObjectIdentifier());
          return;
        }
        SplitDataSource(
            managed_data_source_ptr, object_type,
            [this](ObjectDigest object_digest) {
              FXL_DCHECK(IsDigestValid(object_digest));
              return encryption_service_->MakeObjectIdentifier(&object_identifier_factory_,
                                                               std::move(object_digest));
            },
            std::move(chunking_permutation),
            [this, waiter, managed_data_source = std::move(managed_data_source),
             tree_references = std::move(tree_references), callback = std::move(callback)](
                IterationStatus status, std::unique_ptr<Piece> piece) mutable {
              if (status == IterationStatus::ERROR) {
                callback(Status::IO_ERROR, ObjectIdentifier());
                return;
              }

              FXL_DCHECK(piece != nullptr);
              ObjectIdentifier identifier = piece->GetIdentifier();
              auto object_info = GetObjectDigestInfo(identifier.object_digest());
              if (!object_info.is_inlined()) {
                ObjectReferencesAndPriority piece_references;
                if (piece->AppendReferences(&piece_references) != Status::OK) {
                  // The piece is generated internally by splitting, not coming from
                  // untrusted source, so decoding should never fail.
                  callback(Status::INTERNAL_ERROR, ObjectIdentifier());
                  return;
                }
                if (object_info.object_type == ObjectType::TREE_NODE) {
                  // There is at most one TREE_NODE, and it must be the last piece, so
                  // it is safe to add tree_references to piece_references there.
                  FXL_DCHECK(status == IterationStatus::DONE);
                  piece_references.insert(std::make_move_iterator(tree_references.begin()),
                                          std::make_move_iterator(tree_references.end()));
                }
                AddPiece(std::move(piece), ChangeSource::LOCAL, IsObjectSynced::NO,
                         std::move(piece_references), waiter->NewCallback());
              }
              if (status == IterationStatus::IN_PROGRESS)
                return;

              FXL_DCHECK(status == IterationStatus::DONE);
              waiter->Finalize([identifier = std::move(identifier),
                                callback = std::move(callback)](Status status) mutable {
                callback(status, std::move(identifier));
              });
            });
      });
}

void PageStorageImpl::GetObjectPart(ObjectIdentifier object_identifier, int64_t offset,
                                    int64_t max_size, Location location,
                                    fit::function<void(Status, fsl::SizedVmo)> callback) {
  FXL_DCHECK(IsDigestValid(object_identifier.object_digest()));
  FXL_DCHECK(GetObjectDigestInfo(object_identifier.object_digest()).object_type ==
             ObjectType::BLOB);
  FXL_DCHECK(IsTokenValid(object_identifier));
  GetOrDownloadPiece(
      object_identifier, location,
      [this, location, object_identifier = std::move(object_identifier), offset, max_size,
       callback = std::move(callback)](Status status, std::unique_ptr<const Piece> piece,
                                       WritePieceCallback write_callback) mutable {
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }
        FXL_DCHECK(piece);
        // |piece| is necessarily a blob, so it must have been retrieved from
        // disk or written to disk already.
        FXL_DCHECK(!write_callback);

        // If we are reading zero bytes, bail out now.
        if (max_size == 0) {
          fsl::SizedVmo buffer;
          if (!fsl::VmoFromString("", &buffer)) {
            callback(Status::INTERNAL_ERROR, nullptr);
            return;
          }
          callback(Status::OK, std::move(buffer));
          return;
        }

        ObjectDigestInfo digest_info = GetObjectDigestInfo(piece->GetIdentifier().object_digest());

        // If the piece is a chunk, then the piece represents the whole object.
        if (digest_info.is_chunk()) {
          const fxl::StringView data = piece->GetData();
          fsl::SizedVmo buffer;
          int64_t start = GetObjectPartStart(offset, data.size());
          int64_t length = GetObjectPartLength(max_size, data.size(), start);
          if (!fsl::VmoFromString(data.substr(start, length), &buffer)) {
            callback(Status::INTERNAL_ERROR, nullptr);
            return;
          }
          callback(Status::OK, std::move(buffer));
          return;
        }

        FXL_DCHECK(digest_info.piece_type == PieceType::INDEX);
        GetIndexObject(*piece, offset, max_size, location, std::move(callback));
      });
}

void PageStorageImpl::GetObject(
    ObjectIdentifier object_identifier, Location location,
    fit::function<void(Status, std::unique_ptr<const Object>)> callback) {
  auto traced_callback = TRACE_CALLBACK(std::move(callback), "ledger", "page_storage_get_object");
  FXL_DCHECK(IsDigestValid(object_identifier.object_digest()));
  FXL_DCHECK(IsTokenValid(object_identifier));
  GetOrDownloadPiece(
      object_identifier, location,
      [this, location, object_identifier = std::move(object_identifier),
       callback = std::move(traced_callback)](Status status, std::unique_ptr<const Piece> piece,
                                              WritePieceCallback write_callback) mutable {
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }
        FXL_DCHECK(piece);
        ObjectDigestInfo digest_info = GetObjectDigestInfo(piece->GetIdentifier().object_digest());

        // If the piece is a chunk, then the piece represents the whole object.
        if (digest_info.is_chunk()) {
          FXL_DCHECK(!write_callback);
          callback(Status::OK, std::make_unique<ChunkObject>(std::move(piece)));
          return;
        }

        FXL_DCHECK(digest_info.piece_type == PieceType::INDEX);
        // This reference remains valid as long as |piece| is valid. The latter
        // is owned by the final callback passed to GetIndexObject, so it
        // outlives the former.
        const Piece& piece_ref = *piece;
        GetIndexObject(piece_ref, 0, -1, location,
                       [piece = std::move(piece), object_identifier,
                        write_callback = std::move(write_callback),
                        callback = std::move(callback)](Status status, fsl::SizedVmo vmo) mutable {
                         if (status != Status::OK) {
                           callback(status, nullptr);
                           return;
                         }
                         auto object = std::make_unique<VmoObject>(std::move(object_identifier),
                                                                   std::move(vmo));
                         if (write_callback) {
                           write_callback(std::move(piece), std::move(object), std::move(callback));
                         } else {
                           callback(status, std::move(object));
                         }
                       });
      });
}

void PageStorageImpl::GetPiece(ObjectIdentifier object_identifier,
                               fit::function<void(Status, std::unique_ptr<const Piece>)> callback) {
  FXL_DCHECK(IsTokenValid(object_identifier));
  ObjectDigestInfo digest_info = GetObjectDigestInfo(object_identifier.object_digest());
  if (digest_info.is_inlined()) {
    callback(Status::OK, std::make_unique<InlinePiece>(std::move(object_identifier)));
    return;
  }

  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler,
          fit::function<void(Status, std::unique_ptr<const Piece>)> callback) mutable {
        std::unique_ptr<const Piece> piece;
        Status status = db_->ReadObject(handler, std::move(object_identifier), &piece);
        callback(status, std::move(piece));
      });
}

void PageStorageImpl::SetSyncMetadata(fxl::StringView key, fxl::StringView value,
                                      fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback), [this, key = key.ToString(), value = value.ToString()](
                               CoroutineHandler* handler, fit::function<void(Status)> callback) {
        callback(db_->SetSyncMetadata(handler, key, value));
      });
}

void PageStorageImpl::GetSyncMetadata(fxl::StringView key,
                                      fit::function<void(Status, std::string)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, key = key.ToString()](CoroutineHandler* handler,
                                   fit::function<void(Status, std::string)> callback) {
        std::string value;
        Status status = db_->GetSyncMetadata(handler, key, &value);
        callback(status, std::move(value));
      });
}

void PageStorageImpl::GetCommitContents(const Commit& commit, std::string min_key,
                                        fit::function<bool(Entry)> on_next,
                                        fit::function<void(Status)> on_done) {
  btree::ForEachEntry(
      environment_->coroutine_service(), this,
      {commit.GetRootIdentifier(), PageStorage::Location::TreeNodeFromNetwork(commit.GetId())},
      min_key,
      [on_next = std::move(on_next)](btree::EntryAndNodeIdentifier next) {
        return on_next(next.entry);
      },
      std::move(on_done));
}

void PageStorageImpl::GetEntryFromCommit(const Commit& commit, std::string key,
                                         fit::function<void(Status, Entry)> callback) {
  std::unique_ptr<bool> key_found = std::make_unique<bool>(false);
  auto on_next = [key, key_found = key_found.get(),
                  callback = callback.share()](btree::EntryAndNodeIdentifier next) {
    if (next.entry.key == key) {
      *key_found = true;
      callback(Status::OK, next.entry);
    }
    return false;
  };

  auto on_done = [key_found = std::move(key_found), callback = std::move(callback)](Status s) {
    if (*key_found) {
      return;
    }
    if (s == Status::OK) {
      callback(Status::KEY_NOT_FOUND, Entry());
      return;
    }
    callback(s, Entry());
  };
  btree::ForEachEntry(
      environment_->coroutine_service(), this,
      {commit.GetRootIdentifier(), PageStorage::Location::TreeNodeFromNetwork(commit.GetId())},
      std::move(key), std::move(on_next), std::move(on_done));
}

void PageStorageImpl::GetDiffForCloud(
    const Commit& target_commit,
    fit::function<void(Status, CommitIdView, std::vector<EntryChange>)> callback) {
  // Use the first parent as the base commit.
  const CommitId base_id = target_commit.GetParentIds()[0].ToString();
  GetCommit(base_id,
            callback::MakeScoped(
                weak_factory_.GetWeakPtr(),
                [this, target_commit = target_commit.Clone(), callback = std::move(callback)](
                    Status status, std::unique_ptr<const Commit> base_commit) mutable {
                  // TODO(nellyv): Here we assume that the parent commit is available: when we start
                  // prunning synced commits it might not be the case and another commit should be
                  // used instead.
                  FXL_DCHECK(status != Status::INTERNAL_NOT_FOUND);
                  if (status != Status::OK) {
                    callback(status, "", {});
                    return;
                  }
                  auto changes = std::make_unique<std::vector<EntryChange>>();
                  auto on_next_diff = [weak_this = weak_factory_.GetWeakPtr(),
                                       changes = changes.get()](TwoWayChange change) {
                    if (!weak_this) {
                      return false;
                    }
                    if (change.base) {
                      FXL_DCHECK(!change.base->entry_id.empty());
                      // This change is either an update or a deletion. In either case we send to
                      // the cloud a deletion of the previous entry.
                      changes->push_back({std::move(*change.base), /*deleted*/ true});
                    }
                    if (change.target) {
                      FXL_DCHECK(!change.target->entry_id.empty());
                      // This change is either an update or an insertion. In either case we send to
                      // the cloud an insertion of the updated entry.
                      changes->push_back({std::move(*change.target), /*deleted*/ false});
                    }
                    return true;
                  };
                  auto on_done = [base_id = base_commit->GetId(), changes = std::move(changes),
                                  callback = std::move(callback)](Status status) {
                    if (status != Status::OK) {
                      callback(status, "", {});
                    }
                    callback(status, base_id, std::move(*changes));
                  };

                  // We expect both commits to be present locally.
                  btree::ForEachTwoWayDiff(
                      environment_->coroutine_service(), this,
                      {base_commit->GetRootIdentifier(), PageStorage::Location::Local()},
                      {target_commit->GetRootIdentifier(), PageStorage::Location::Local()}, "",
                      std::move(on_next_diff), std::move(on_done));
                }));
}

void PageStorageImpl::GetCommitContentsDiff(const Commit& base_commit, const Commit& other_commit,
                                            std::string min_key,
                                            fit::function<bool(EntryChange)> on_next_diff,
                                            fit::function<void(Status)> on_done) {
  btree::ForEachDiff(environment_->coroutine_service(), this,
                     {base_commit.GetRootIdentifier(),
                      PageStorage::Location::TreeNodeFromNetwork(base_commit.GetId())},
                     {other_commit.GetRootIdentifier(),
                      PageStorage::Location::TreeNodeFromNetwork(other_commit.GetId())},
                     std::move(min_key), std::move(on_next_diff), std::move(on_done));
}

void PageStorageImpl::GetThreeWayContentsDiff(const Commit& base_commit, const Commit& left_commit,
                                              const Commit& right_commit, std::string min_key,
                                              fit::function<bool(ThreeWayChange)> on_next_diff,
                                              fit::function<void(Status)> on_done) {
  btree::ForEachThreeWayDiff(environment_->coroutine_service(), this,
                             {base_commit.GetRootIdentifier(),
                              PageStorage::Location::TreeNodeFromNetwork(base_commit.GetId())},
                             {left_commit.GetRootIdentifier(),
                              PageStorage::Location::TreeNodeFromNetwork(left_commit.GetId())},
                             {right_commit.GetRootIdentifier(),
                              PageStorage::Location::TreeNodeFromNetwork(right_commit.GetId())},
                             std::move(min_key), std::move(on_next_diff), std::move(on_done));
}

void PageStorageImpl::NotifyWatchersOfNewCommits(
    const std::vector<std::unique_ptr<const Commit>>& new_commits, ChangeSource source) {
  for (auto& watcher : watchers_) {
    watcher.OnNewCommits(new_commits, source);
  }
}

Status PageStorageImpl::MarkAllPiecesLocal(CoroutineHandler* handler, PageDb::Batch* batch,
                                           std::vector<ObjectIdentifier> object_identifiers) {
  std::set<ObjectIdentifier> seen_identifiers;
  while (!object_identifiers.empty()) {
    auto it = seen_identifiers.insert(std::move(object_identifiers.back()));
    object_identifiers.pop_back();
    const ObjectIdentifier& object_identifier = *(it.first);
    FXL_DCHECK(!GetObjectDigestInfo(object_identifier.object_digest()).is_inlined());
    FXL_DCHECK(IsTokenValid(object_identifier));
    Status status = batch->SetObjectStatus(handler, object_identifier, PageDbObjectStatus::LOCAL);
    if (status != Status::OK) {
      return status;
    }
    if (GetObjectDigestInfo(object_identifier.object_digest()).piece_type == PieceType::INDEX) {
      std::unique_ptr<const Piece> piece;
      status = db_->ReadObject(handler, object_identifier, &piece);
      if (status != Status::OK) {
        return status;
      }
      fxl::StringView content = piece->GetData();

      const FileIndex* file_index;
      status = FileIndexSerialization::ParseFileIndex(content, &file_index);
      if (status != Status::OK) {
        return status;
      }

      object_identifiers.reserve(object_identifiers.size() + file_index->children()->size());
      for (const auto* child : *file_index->children()) {
        ObjectIdentifier new_object_identifier =
            ToObjectIdentifier(child->object_identifier(), &object_identifier_factory_);
        if (!GetObjectDigestInfo(new_object_identifier.object_digest()).is_inlined()) {
          if (!seen_identifiers.count(new_object_identifier)) {
            object_identifiers.push_back(std::move(new_object_identifier));
          }
        }
      }
    }
  }
  return Status::OK;
}

Status PageStorageImpl::ContainsCommit(CoroutineHandler* handler, CommitIdView id) {
  if (IsFirstCommit(id)) {
    return Status::OK;
  }
  std::string bytes;
  return db_->GetCommitStorageBytes(handler, id, &bytes);
}

bool PageStorageImpl::IsFirstCommit(CommitIdView id) { return id == kFirstPageCommitId; }

void PageStorageImpl::AddPiece(std::unique_ptr<const Piece> piece, ChangeSource source,
                               IsObjectSynced is_object_synced,
                               ObjectReferencesAndPriority references,
                               fit::function<void(Status)> callback) {
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, piece = std::move(piece), source, references = std::move(references),
       is_object_synced](CoroutineHandler* handler, fit::function<void(Status)> callback) mutable {
        callback(
            SynchronousAddPiece(handler, *piece, source, is_object_synced, std::move(references)));
      });
}

void PageStorageImpl::ObjectIsUntracked(ObjectIdentifier object_identifier,
                                        fit::function<void(Status, bool)> callback) {
  FXL_DCHECK(IsTokenValid(object_identifier));
  coroutine_manager_.StartCoroutine(
      std::move(callback),
      [this, object_identifier = std::move(object_identifier)](
          CoroutineHandler* handler, fit::function<void(Status, bool)> callback) mutable {
        if (GetObjectDigestInfo(object_identifier.object_digest()).is_inlined()) {
          callback(Status::OK, false);
          return;
        }

        PageDbObjectStatus object_status;
        Status status = db_->GetObjectStatus(handler, object_identifier, &object_status);
        callback(status, object_status == PageDbObjectStatus::TRANSIENT);
      });
}

void PageStorageImpl::GetIndexObject(const Piece& piece, int64_t offset, int64_t max_size,
                                     Location location,
                                     fit::function<void(Status, fsl::SizedVmo)> callback) {
  ObjectDigestInfo digest_info = GetObjectDigestInfo(piece.GetIdentifier().object_digest());

  FXL_DCHECK(digest_info.piece_type == PieceType::INDEX);
  fxl::StringView content = piece.GetData();
  const FileIndex* file_index;
  Status status = FileIndexSerialization::ParseFileIndex(content, &file_index);
  if (status != Status::OK) {
    callback(Status::DATA_INTEGRITY_ERROR, nullptr);
    return;
  }

  int64_t start = GetObjectPartStart(offset, file_index->size());
  int64_t length = GetObjectPartLength(max_size, file_index->size(), start);
  zx::vmo raw_vmo;
  zx_status_t zx_status = zx::vmo::create(length, 0, &raw_vmo);
  if (zx_status != ZX_OK) {
    FXL_PLOG(WARNING, zx_status) << "Unable to create VMO of size: " << length;
    callback(Status::INTERNAL_ERROR, nullptr);
    return;
  }
  fsl::SizedVmo vmo(std::move(raw_vmo), length);

  fsl::SizedVmo vmo_copy;
  zx_status = vmo.Duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_WRITE, &vmo_copy);
  if (zx_status != ZX_OK) {
    FXL_PLOG(ERROR, zx_status) << "Unable to duplicate vmo";
    callback(Status::INTERNAL_ERROR, nullptr);
    return;
  }

  FillBufferWithObjectContent(
      piece, std::move(vmo_copy), start, length, 0, file_index->size(), location,
      [vmo = std::move(vmo), callback = std::move(callback)](Status status) mutable {
        callback(status, std::move(vmo));
      });
}

void PageStorageImpl::FillBufferWithObjectContent(const Piece& piece, fsl::SizedVmo vmo,
                                                  int64_t global_offset, int64_t global_size,
                                                  int64_t current_position, int64_t object_size,
                                                  Location location,
                                                  fit::function<void(Status)> callback) {
  fxl::StringView content = piece.GetData();
  ObjectDigestInfo digest_info = GetObjectDigestInfo(piece.GetIdentifier().object_digest());
  if (digest_info.is_inlined() || digest_info.is_chunk()) {
    if (object_size != static_cast<int64_t>(content.size())) {
      FXL_LOG(ERROR) << "Error in serialization format. Expecting object: " << piece.GetIdentifier()
                     << " to have size: " << object_size
                     << ", but found an object of size: " << content.size();
      callback(Status::DATA_INTEGRITY_ERROR);
      return;
    }
    // Distance is negative if the offset is ahead and positive if behind.
    int64_t distance_from_global_offset = current_position - global_offset;
    // Read offset can be non-zero on first read; in that case, we need to skip
    // bytes coming before global offset.
    int64_t read_offset = std::max(-distance_from_global_offset, 0L);
    // Write offset is zero on the first write; otherwise we need to skip number
    // of bytes corresponding to what we have already written.
    int64_t write_offset = std::max(distance_from_global_offset, 0L);
    // Read and write until reaching either size of the object, or global size.
    int64_t read_write_size =
        std::min(static_cast<int64_t>(content.size()) - read_offset, global_size - write_offset);
    FXL_DCHECK(read_write_size > 0);
    fxl::StringView read_substr = content.substr(read_offset, read_write_size);
    zx_status_t zx_status = vmo.vmo().write(read_substr.data(), write_offset, read_write_size);
    if (zx_status != ZX_OK) {
      FXL_PLOG(ERROR, zx_status) << "Unable to write to vmo";
      callback(Status::INTERNAL_ERROR);
      return;
    }
    callback(Status::OK);
    return;
  }

  const FileIndex* file_index;
  Status status = FileIndexSerialization::ParseFileIndex(content, &file_index);
  if (status != Status::OK) {
    callback(Status::DATA_INTEGRITY_ERROR);
    return;
  }
  if (static_cast<int64_t>(file_index->size()) != object_size) {
    FXL_LOG(ERROR) << "Error in serialization format. Expecting object: " << piece.GetIdentifier()
                   << " to have size " << object_size
                   << ", but found an index object of size: " << file_index->size();
    callback(Status::DATA_INTEGRITY_ERROR);
    return;
  }

  // Iterate over the children pieces, recursing into the ones corresponding to
  // the part of the object to be copied to the VMO.
  // TODO(LE-702): ensure that all intermediate pieces are kept alive until the finalization
  // callback has run, which will write the current piece to disk if necessary.
  int64_t sub_offset = 0;
  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  for (const auto* child : *file_index->children()) {
    if (sub_offset + child->size() > file_index->size()) {
      callback(Status::DATA_INTEGRITY_ERROR);
      return;
    }
    int64_t child_position = current_position + sub_offset;
    ObjectIdentifier child_identifier =
        ToObjectIdentifier(child->object_identifier(), &object_identifier_factory_);
    // Skip children before the part to copy.
    if (child_position + static_cast<int64_t>(child->size()) <= global_offset) {
      sub_offset += child->size();
      continue;
    }
    // Stop iterating as soon as the part has been fully copied.
    if (global_offset + global_size <= child_position) {
      break;
    }
    // Create a copy of the VMO to be owned by the recursive call.
    fsl::SizedVmo vmo_copy;
    zx_status_t zx_status = vmo.Duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_WRITE, &vmo_copy);
    if (zx_status != ZX_OK) {
      FXL_PLOG(ERROR, zx_status) << "Unable to duplicate vmo";
      callback(Status::INTERNAL_ERROR);
      return;
    }
    // This is a child, so it cannot be a tree node, only top pieces may be tree
    // nodes.
    FXL_DCHECK(GetObjectDigestInfo(child_identifier.object_digest()).object_type ==
               ObjectType::BLOB);
    GetOrDownloadPiece(child_identifier, location,
                       [this, child_identifier, vmo = std::move(vmo_copy), global_offset,
                        global_size, child_position, child_size = child->size(), location,
                        child_callback = waiter->NewCallback()](
                           Status status, std::unique_ptr<const Piece> child_piece,
                           WritePieceCallback write_callback) mutable {
                         if (status != Status::OK) {
                           child_callback(status);
                           return;
                         }
                         FXL_DCHECK(child_piece);
                         // The |child_piece| is necessarily a blob, so it must have been read
                         // from or written to disk already.
                         FXL_DCHECK(!write_callback);
                         FillBufferWithObjectContent(
                             *child_piece, std::move(vmo), global_offset, global_size,
                             child_position, child_size, location,
                             [child_callback = std::move(child_callback)](Status status) mutable {
                               child_callback(status);
                             });
                       });
    sub_offset += child->size();
  }
  waiter->Finalize(std::move(callback));
}

void PageStorageImpl::GetOrDownloadPiece(
    ObjectIdentifier object_identifier, Location location,
    fit::function<void(Status, std::unique_ptr<const Piece>, WritePieceCallback)> callback) {
  GetPiece(object_identifier, [this, callback = std::move(callback), location,
                               object_identifier = std::move(object_identifier)](
                                  Status status, std::unique_ptr<const Piece> piece) mutable {
    // Object was found.
    if (status == Status::OK) {
      callback(status, std::move(piece), {});
      return;
    }
    FXL_DCHECK(piece == nullptr);
    // An unexpected error occured.
    if (status != Status::INTERNAL_NOT_FOUND || location.is_local()) {
      callback(status, nullptr, {});
      return;
    }
    // Object not found locally, attempt to download it.
    FXL_DCHECK(location.is_network());
    if (!page_sync_) {
      callback(Status::NETWORK_ERROR, nullptr, {});
      return;
    }
    coroutine_manager_.StartCoroutine(
        std::move(callback),
        [this, object_identifier = std::move(object_identifier)](
            CoroutineHandler* handler,
            fit::function<void(Status, std::unique_ptr<const Piece>, WritePieceCallback)>
                callback) mutable {
          Status status;
          ChangeSource source;
          IsObjectSynced is_object_synced;
          std::unique_ptr<DataSource::DataChunk> chunk;

          // Retrieve an object from the network.
          if (coroutine::SyncCall(
                  handler,
                  [this,
                   object_identifier](fit::function<void(Status, ChangeSource, IsObjectSynced,
                                                         std::unique_ptr<DataSource::DataChunk>)>
                                          callback) mutable {
                    page_sync_->GetObject(std::move(object_identifier), ObjectType::BLOB,
                                          std::move(callback));
                  },
                  &status, &source, &is_object_synced,
                  &chunk) == coroutine::ContinuationStatus::INTERRUPTED) {
            callback(Status::INTERRUPTED, nullptr, {});
            return;
          }
          if (status != Status::OK) {
            callback(status, nullptr, {});
            return;
          }
          // Sanity-check of retrieved object.
          auto digest_info = GetObjectDigestInfo(object_identifier.object_digest());
          FXL_DCHECK(!digest_info.is_inlined());

          if (object_identifier.object_digest() !=
              ComputeObjectDigest(digest_info.piece_type, digest_info.object_type, chunk->Get())) {
            callback(Status::DATA_INTEGRITY_ERROR, nullptr, {});
            return;
          }
          std::unique_ptr<const Piece> piece =
              std::make_unique<DataChunkPiece>(std::move(object_identifier), std::move(chunk));

          // Write the piece to disk if possible. Index tree nodes cannot be
          // written at this stage as we need the full object.
          if (digest_info.object_type == ObjectType::TREE_NODE &&
              digest_info.piece_type == PieceType::INDEX) {
            // Return a WritePiece callback since the piece has not been written to disk.
            callback(
                Status::OK, std::move(piece),
                [this, source, is_object_synced](
                    std::unique_ptr<const Piece> piece, std::unique_ptr<const Object> object,
                    fit::function<void(Status, std::unique_ptr<const Object>)> final_callback) {
                  ObjectReferencesAndPriority references;
                  Status status = piece->AppendReferences(&references);
                  if (status != Status::OK) {
                    final_callback(status, nullptr);
                    return;
                  }
                  status = object->AppendReferences(&references);
                  if (status != Status::OK) {
                    final_callback(status, nullptr);
                    return;
                  }
                  AddPiece(std::move(piece), source, is_object_synced, references,
                           [final_callback = std::move(final_callback),
                            object = std::move(object)](Status status) mutable {
                             if (status != Status::OK) {
                               final_callback(status, nullptr);
                               return;
                             }
                             final_callback(Status::OK, std::move(object));
                           });
                });
            return;
          }
          ObjectReferencesAndPriority references;
          status = piece->AppendReferences(&references);
          if (status != Status::OK) {
            callback(status, nullptr, {});
            return;
          }
          if (digest_info.object_type == ObjectType::TREE_NODE) {
            FXL_DCHECK(digest_info.is_chunk());
            // Convert the piece to a chunk Object to extract its
            // references.
            auto object = std::make_unique<ChunkObject>(std::move(piece));
            status = object->AppendReferences(&references);
            if (status != Status::OK) {
              callback(status, nullptr, {});
              return;
            }
            piece = object->ReleasePiece();
          }
          status = SynchronousAddPiece(handler, *piece, source, is_object_synced, references);
          if (status != Status::OK) {
            callback(status, nullptr, {});
            return;
          }
          callback(Status::OK, std::move(piece), {});
        });
  });
}

ObjectIdentifierFactory* PageStorageImpl::GetObjectIdentifierFactory() {
  return &object_identifier_factory_;
}

CommitFactory* PageStorageImpl::GetCommitFactory() { return &commit_factory_; }

Status PageStorageImpl::SynchronousInit(CoroutineHandler* handler) {
  // Add the default page head if this page is empty.
  std::vector<std::pair<zx::time_utc, CommitId>> heads;
  Status s = db_->GetHeads(handler, &heads);
  if (s != Status::OK) {
    return s;
  }
  // Cache the heads and update the live commit tracker.
  std::vector<std::unique_ptr<const Commit>> commits;
  if (heads.empty()) {
    s = db_->AddHead(handler, kFirstPageCommitId, zx::time_utc());
    if (s != Status::OK) {
      return s;
    }
    std::unique_ptr<const Commit> head_commit;
    s = SynchronousGetCommit(handler, kFirstPageCommitId.ToString(), &head_commit);
    if (s != Status::OK) {
      return s;
    }
    commits.push_back(std::move(head_commit));
  } else {
    auto waiter =
        fxl::MakeRefCounted<callback::Waiter<Status, std::unique_ptr<const Commit>>>(Status::OK);

    for (const auto& head : heads) {
      GetCommit(head.second, waiter->NewCallback());
    }
    if (coroutine::Wait(handler, std::move(waiter), &s, &commits) ==
        coroutine::ContinuationStatus::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    if (s != Status::OK) {
      return s;
    }
  }
  commit_factory_.AddHeads(std::move(commits));

  // Cache whether this page is online or not.
  return db_->IsPageOnline(handler, &page_is_online_);
}

Status PageStorageImpl::SynchronousGetCommit(CoroutineHandler* handler, CommitId commit_id,
                                             std::unique_ptr<const Commit>* commit) {
  if (IsFirstCommit(commit_id)) {
    Status s;
    if (coroutine::SyncCall(
            handler,
            [this](fit::function<void(Status, std::unique_ptr<const Commit>)> callback) {
              commit_factory_.Empty(this, std::move(callback));
            },
            &s, commit) == coroutine::ContinuationStatus::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    return s;
  }
  std::string bytes;
  Status s = db_->GetCommitStorageBytes(handler, commit_id, &bytes);
  if (s != Status::OK) {
    return s;
  }
  return commit_factory_.FromStorageBytes(commit_id, std::move(bytes), commit);
}

Status PageStorageImpl::SynchronousAddCommitFromLocal(CoroutineHandler* handler,
                                                      std::unique_ptr<const Commit> commit,
                                                      std::vector<ObjectIdentifier> new_objects) {
  FXL_DCHECK(IsDigestValid(commit->GetRootIdentifier().object_digest()));
  FXL_DCHECK(IsTokenValid(commit->GetRootIdentifier()));
  std::vector<std::unique_ptr<const Commit>> commits;
  commits.reserve(1);
  commits.push_back(std::move(commit));

  return SynchronousAddCommits(handler, std::move(commits), ChangeSource::LOCAL,
                               std::move(new_objects), nullptr);
}

Status PageStorageImpl::SynchronousAddCommitsFromSync(CoroutineHandler* handler,
                                                      std::vector<CommitIdAndBytes> ids_and_bytes,
                                                      ChangeSource source,
                                                      std::vector<CommitId>* missing_ids) {
  std::vector<std::unique_ptr<const Commit>> commits;

  std::map<const CommitId*, const Commit*, StringPointerComparator> leaves;
  commits.reserve(ids_and_bytes.size());

  // The locked section below contains asynchronous operations reading the
  // database, and branches depending on those reads. This section is thus a
  // critical section and we need to ensure it is not executed concurrently by
  // several coroutines. The locked sections (and only those) are thus executed
  // serially.
  std::unique_ptr<lock::Lock> lock;
  if (lock::AcquireLock(handler, &commit_serializer_, &lock) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }

  for (auto& id_and_bytes : ids_and_bytes) {
    CommitId id = std::move(id_and_bytes.id);
    std::string storage_bytes = std::move(id_and_bytes.bytes);
    Status status = ContainsCommit(handler, id);
    if (status == Status::OK) {
      // We only mark cloud-sourced commits as synced.
      if (source == ChangeSource::CLOUD) {
        Status status = SynchronousMarkCommitSynced(handler, id);
        if (status != Status::OK) {
          return status;
        }
      }
      continue;
    }

    if (status != Status::INTERNAL_NOT_FOUND) {
      return status;
    }

    std::unique_ptr<const Commit> commit;
    status = commit_factory_.FromStorageBytes(id, std::move(storage_bytes), &commit);
    if (status != Status::OK) {
      FXL_LOG(ERROR) << "Unable to add commit. Id: " << convert::ToHex(id);
      return status;
    }

    // Remove parents from leaves.
    for (const auto& parent_id : commit->GetParentIds()) {
      auto it = leaves.find(&parent_id);
      if (it != leaves.end()) {
        leaves.erase(it);
      }
    }
    leaves[&commit->GetId()] = commit.get();
    commits.push_back(std::move(commit));
  }

  if (commits.empty()) {
    return Status::OK;
  }

  lock.reset();

  auto waiter = fxl::MakeRefCounted<callback::StatusWaiter<Status>>(Status::OK);
  // Get all objects from sync and then add the commit objects.
  for (const auto& leaf : leaves) {
    btree::GetObjectsFromSync(environment_->coroutine_service(), this,
                              {leaf.second->GetRootIdentifier(),
                               PageStorage::Location::TreeNodeFromNetwork(leaf.second->GetId())},
                              waiter->NewCallback());
  }

  Status waiter_status;
  if (coroutine::Wait(handler, std::move(waiter), &waiter_status) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  if (waiter_status != Status::OK) {
    return waiter_status;
  }

  return SynchronousAddCommits(handler, std::move(commits), source, std::vector<ObjectIdentifier>(),
                               missing_ids);
}

Status PageStorageImpl::SynchronousGetUnsyncedCommits(
    CoroutineHandler* handler, std::vector<std::unique_ptr<const Commit>>* unsynced_commits) {
  std::vector<CommitId> commit_ids;
  Status s = db_->GetUnsyncedCommitIds(handler, &commit_ids);
  if (s != Status::OK) {
    return s;
  }

  auto waiter =
      fxl::MakeRefCounted<callback::Waiter<Status, std::unique_ptr<const Commit>>>(Status::OK);
  for (const auto& commit_id : commit_ids) {
    GetCommit(commit_id, waiter->NewCallback());
  }

  std::vector<std::unique_ptr<const Commit>> result;
  if (coroutine::Wait(handler, std::move(waiter), &s, &result) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }
  if (s != Status::OK) {
    return s;
  }
  unsynced_commits->swap(result);
  return Status::OK;
}

Status PageStorageImpl::SynchronousMarkCommitSynced(CoroutineHandler* handler,
                                                    const CommitId& commit_id) {
  std::unique_ptr<PageDb::Batch> batch;
  Status status = db_->StartBatch(handler, &batch);
  if (status != Status::OK) {
    return status;
  }
  status = SynchronousMarkCommitSyncedInBatch(handler, batch.get(), commit_id);
  if (status != Status::OK) {
    return status;
  }
  return batch->Execute(handler);
}

Status PageStorageImpl::SynchronousMarkCommitSyncedInBatch(CoroutineHandler* handler,
                                                           PageDb::Batch* batch,
                                                           const CommitId& commit_id) {
  Status status = SynchronousMarkPageOnline(handler, batch);
  if (status != Status::OK) {
    return status;
  }
  return batch->MarkCommitIdSynced(handler, commit_id);
}

Status PageStorageImpl::SynchronousAddCommits(CoroutineHandler* handler,
                                              std::vector<std::unique_ptr<const Commit>> commits,
                                              ChangeSource source,
                                              std::vector<ObjectIdentifier> new_objects,
                                              std::vector<CommitId>* missing_ids) {
  // Make sure that only one AddCommits operation is executed at a time.
  // Otherwise, if db_ operations are asynchronous, ContainsCommit (below) may
  // return NOT_FOUND while another commit is added, and batch->Execute() will
  // break the invariants of this system (in particular, that synced commits
  // cannot become unsynced).
  std::unique_ptr<lock::Lock> lock;
  if (lock::AcquireLock(handler, &commit_serializer_, &lock) ==
      coroutine::ContinuationStatus::INTERRUPTED) {
    return Status::INTERRUPTED;
  }

  // Apply all changes atomically.
  std::unique_ptr<PageDb::Batch> batch;
  Status status = db_->StartBatch(handler, &batch);
  if (status != Status::OK) {
    return status;
  }
  std::set<const CommitId*, StringPointerComparator> added_commits;
  std::vector<std::unique_ptr<const Commit>> commits_to_send;

  std::map<CommitId, std::unique_ptr<const Commit>> heads_to_add;
  std::vector<CommitId> removed_heads;

  int orphaned_commits = 0;
  for (auto& commit : commits) {
    // We need to check if we are adding an already present remote commit here
    // because we might both download and locally commit the same commit at
    // roughly the same time. As commit writing is asynchronous, the previous
    // check in AddCommitsFromSync may have not matched any commit, while a
    // commit got added in between.
    Status s = ContainsCommit(handler, commit->GetId());
    if (s == Status::OK) {
      if (source == ChangeSource::CLOUD) {
        s = SynchronousMarkCommitSyncedInBatch(handler, batch.get(), commit->GetId());
        if (s != Status::OK) {
          return s;
        }
      }
      // The commit is already here. We can safely skip it.
      continue;
    }
    if (s != Status::INTERNAL_NOT_FOUND) {
      return s;
    }
    // Now, we know we are adding a new commit.

    // If the commit is a merge, register it in the merge index.
    std::vector<CommitIdView> parent_ids = commit->GetParentIds();
    if (parent_ids.size() == 2) {
      s = batch->AddMerge(handler, parent_ids[0], parent_ids[1], commit->GetId());
      if (s != Status::OK) {
        return s;
      }
    }

    // Commits should arrive in order. Check that the parents are either
    // present in PageDb or in the list of already processed commits.
    // If the commit arrive out of order, print an error, but skip it
    // temporarily so that the Ledger can recover if all the needed commits
    // are received in a single batch.
    bool orphaned_commit = false;
    for (const CommitIdView& parent_id : parent_ids) {
      if (added_commits.find(&parent_id) == added_commits.end()) {
        s = ContainsCommit(handler, parent_id);
        if (s == Status::INTERRUPTED) {
          return s;
        }
        if (s != Status::OK) {
          FXL_LOG(ERROR) << "Failed to find parent commit \"" << ToHex(parent_id)
                         << "\" of commit \"" << convert::ToHex(commit->GetId()) << "\".";
          if (s == Status::INTERNAL_NOT_FOUND) {
            if (missing_ids) {
              missing_ids->push_back(parent_id.ToString());
            }
            orphaned_commit = true;
            continue;
          }
          return Status::INTERNAL_ERROR;
        }
      }
      // Remove the parent from the list of heads.
      if (!heads_to_add.erase(parent_id.ToString())) {
        // parent_id was not added in the batch: remove it from heads in Db.
        s = batch->RemoveHead(handler, parent_id);
        if (s != Status::OK) {
          return s;
        }
        removed_heads.push_back(parent_id.ToString());
      }
    }

    // The commit could not be added. Skip it.
    if (orphaned_commit) {
      orphaned_commits++;
      continue;
    }

    s = batch->AddCommitStorageBytes(handler, commit->GetId(), commit->GetRootIdentifier(),
                                     commit->GetStorageBytes());
    if (s != Status::OK) {
      return s;
    }

    if (source != ChangeSource::CLOUD) {
      s = batch->MarkCommitIdUnsynced(handler, commit->GetId(), commit->GetGeneration());
      if (s != Status::OK) {
        return s;
      }
    }

    // Update heads_to_add.
    heads_to_add[commit->GetId()] = commit->Clone();

    added_commits.insert(&commit->GetId());
    commits_to_send.push_back(std::move(commit));
  }

  if (orphaned_commits > 0) {
    if (source != ChangeSource::P2P) {
      ledger::ReportEvent(ledger::CobaltEvent::COMMITS_RECEIVED_OUT_OF_ORDER_NOT_RECOVERED);
      FXL_LOG(ERROR) << "Failed adding commits. Found " << orphaned_commits
                     << " orphaned commits (one of their parent was not found).";
    }
    return Status::INTERNAL_NOT_FOUND;
  }

  // Update heads in Db.
  for (const auto& head : heads_to_add) {
    Status s = batch->AddHead(handler, head.second->GetId(), head.second->GetTimestamp());
    if (s != Status::OK) {
      return s;
    }
  }

  // If adding local commits, mark all new pieces as local.
  Status s = MarkAllPiecesLocal(handler, batch.get(), std::move(new_objects));
  if (s != Status::OK) {
    return s;
  }

  s = batch->Execute(handler);
  if (s != Status::OK) {
    return s;
  }

  // Only update the cache of heads after a successful update of the PageDb.
  commit_factory_.RemoveHeads(std::move(removed_heads));
  std::vector<std::unique_ptr<const Commit>> new_heads;
  std::transform(std::make_move_iterator(heads_to_add.begin()),
                 std::make_move_iterator(heads_to_add.end()), std::back_inserter(new_heads),
                 [](std::pair<CommitId, std::unique_ptr<const Commit>>&& head) {
                   return std::move(std::get<std::unique_ptr<const Commit>>(head));
                 });
  commit_factory_.AddHeads(std::move(new_heads));
  NotifyWatchersOfNewCommits(commits_to_send, source);
  commit_pruner_.Prune([](Status status) {
    if (status != Status::OK) {
      FXL_LOG(ERROR) << "Error when pruning: " << status;
    }
  });
  return s;
}

Status PageStorageImpl::SynchronousAddPiece(CoroutineHandler* handler, const Piece& piece,
                                            ChangeSource source, IsObjectSynced is_object_synced,
                                            ObjectReferencesAndPriority references) {
  FXL_DCHECK(!GetObjectDigestInfo(piece.GetIdentifier().object_digest()).is_inlined());
  FXL_DCHECK(
      piece.GetIdentifier().object_digest() ==
      ComputeObjectDigest(GetObjectDigestInfo(piece.GetIdentifier().object_digest()).piece_type,
                          GetObjectDigestInfo(piece.GetIdentifier().object_digest()).object_type,
                          piece.GetData()));

  Status status = db_->HasObject(handler, piece.GetIdentifier());
  if (status == Status::INTERNAL_NOT_FOUND) {
    PageDbObjectStatus object_status;
    switch (is_object_synced) {
      case IsObjectSynced::NO:
        object_status = (source == ChangeSource::LOCAL ? PageDbObjectStatus::TRANSIENT
                                                       : PageDbObjectStatus::LOCAL);
        break;
      case IsObjectSynced::YES:
        object_status = PageDbObjectStatus::SYNCED;
        break;
    }
    return db_->WriteObject(handler, piece, object_status, references);
  }
  return status;
}

Status PageStorageImpl::SynchronousMarkPageOnline(coroutine::CoroutineHandler* handler,
                                                  PageDb::Batch* batch) {
  if (page_is_online_) {
    return Status::OK;
  }
  Status status = batch->MarkPageOnline(handler);
  if (status == Status::OK) {
    page_is_online_ = true;
  }
  return status;
}

FXL_WARN_UNUSED_RESULT Status PageStorageImpl::SynchronousGetEmptyNodeIdentifier(
    coroutine::CoroutineHandler* handler, ObjectIdentifier** empty_node_id) {
  if (!empty_node_id_) {
    // Get the empty node identifier and cache it.
    Status status;
    ObjectIdentifier object_identifier;
    if (coroutine::SyncCall(
            handler,
            [this](fit::function<void(Status, ObjectIdentifier)> callback) {
              btree::TreeNode::Empty(this, std::move(callback));
            },
            &status, &object_identifier) == coroutine::ContinuationStatus::INTERRUPTED) {
      return Status::INTERRUPTED;
    }
    if (status != Status::OK) {
      return status;
    }
    empty_node_id_ = std::make_unique<ObjectIdentifier>(std::move(object_identifier));
  }
  *empty_node_id = empty_node_id_.get();
  return Status::OK;
}

bool PageStorageImpl::IsTokenValid(const ObjectIdentifier& object_identifier) {
  return object_identifier.factory() == &object_identifier_factory_;
}
}  // namespace storage
