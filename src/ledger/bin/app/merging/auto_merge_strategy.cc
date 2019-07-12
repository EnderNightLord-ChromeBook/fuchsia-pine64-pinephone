// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/app/merging/auto_merge_strategy.h"

#include <lib/callback/scoped_callback.h>
#include <lib/fit/function.h>

#include <memory>
#include <string>
#include <vector>

#include "src/ledger/bin/app/active_page_manager.h"
#include "src/ledger/bin/app/merging/conflict_resolver_client.h"
#include "src/ledger/bin/app/page_utils.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace ledger {
class AutoMergeStrategy::AutoMerger {
 public:
  AutoMerger(storage::PageStorage* storage, ActivePageManager* active_page_manager,
             ConflictResolver* conflict_resolver, std::unique_ptr<const storage::Commit> left,
             std::unique_ptr<const storage::Commit> right,
             std::unique_ptr<const storage::Commit> ancestor, fit::function<void(Status)> callback);
  ~AutoMerger();

  void Start();
  void Cancel();
  void Done(Status status);

 private:
  void OnRightChangeReady(Status status,
                          std::unique_ptr<std::vector<storage::EntryChange>> right_change);
  void OnComparisonDone(Status status,
                        std::unique_ptr<std::vector<storage::EntryChange>> right_changes,
                        bool distinct);
  void ApplyDiffOnJournal(std::unique_ptr<storage::Journal> journal,
                          std::unique_ptr<std::vector<storage::EntryChange>> diff);

  storage::PageStorage* const storage_;
  ActivePageManager* const manager_;
  ConflictResolver* const conflict_resolver_;

  std::unique_ptr<const storage::Commit> left_;
  std::unique_ptr<const storage::Commit> right_;
  std::unique_ptr<const storage::Commit> ancestor_;

  std::unique_ptr<ConflictResolverClient> delegated_merge_;

  fit::function<void(Status)> callback_;

  bool cancelled_ = false;

  // This must be the last member of the class.
  fxl::WeakPtrFactory<AutoMergeStrategy::AutoMerger> weak_factory_;
};

AutoMergeStrategy::AutoMerger::AutoMerger(storage::PageStorage* storage,
                                          ActivePageManager* active_page_manager,
                                          ConflictResolver* conflict_resolver,
                                          std::unique_ptr<const storage::Commit> left,
                                          std::unique_ptr<const storage::Commit> right,
                                          std::unique_ptr<const storage::Commit> ancestor,
                                          fit::function<void(Status)> callback)
    : storage_(storage),
      manager_(active_page_manager),
      conflict_resolver_(conflict_resolver),
      left_(std::move(left)),
      right_(std::move(right)),
      ancestor_(std::move(ancestor)),
      callback_(std::move(callback)),
      weak_factory_(this) {
  FXL_DCHECK(callback_);
}

AutoMergeStrategy::AutoMerger::~AutoMerger() {}

void AutoMergeStrategy::AutoMerger::Start() {
  auto changes = std::make_unique<std::vector<storage::EntryChange>>();
  auto on_next = [weak_this = weak_factory_.GetWeakPtr(),
                  changes = changes.get()](storage::EntryChange change) {
    if (!weak_this) {
      return false;
    }

    if (weak_this->cancelled_) {
      return false;
    }

    changes->push_back(change);
    return true;
  };

  auto callback = callback::MakeScoped(weak_factory_.GetWeakPtr(),
                                       [this, changes = std::move(changes)](Status status) mutable {
                                         if (cancelled_) {
                                           Done(Status::INTERNAL_ERROR);
                                           return;
                                         }
                                         OnRightChangeReady(status, std::move(changes));
                                       });

  storage_->GetCommitContentsDiff(*ancestor_, *right_, "", std::move(on_next), std::move(callback));
}

void AutoMergeStrategy::AutoMerger::OnRightChangeReady(
    Status status, std::unique_ptr<std::vector<storage::EntryChange>> right_change) {
  if (cancelled_) {
    Done(Status::INTERNAL_ERROR);
    return;
  }

  if (status != Status::OK) {
    FXL_LOG(ERROR) << "Unable to compute right diff due to error " << status << ", aborting.";
    Done(status);
    return;
  }

  if (right_change->empty()) {
    OnComparisonDone(Status::OK, std::move(right_change), true);
    return;
  }

  struct PageChangeIndex {
    size_t entry_index = 0;
    bool distinct = true;
  };

  auto index = std::make_unique<PageChangeIndex>();

  auto on_next = [weak_this = weak_factory_.GetWeakPtr(), index = index.get(),
                  right_change = right_change.get()](storage::EntryChange change) {
    if (!weak_this || weak_this->cancelled_) {
      return false;
    }

    while (change.entry.key > (*right_change)[index->entry_index].entry.key) {
      index->entry_index++;
      if (index->entry_index >= right_change->size()) {
        return false;
      }
    }
    if (change.entry.key == (*right_change)[index->entry_index].entry.key) {
      if (change == (*right_change)[index->entry_index]) {
        return true;
      }
      index->distinct = false;
      return false;
    }
    return true;
  };

  // |callback| is called when the full diff is computed.
  auto callback = callback::MakeScoped(
      weak_factory_.GetWeakPtr(), [this, right_change = std::move(right_change),
                                   index = std::move(index)](Status status) mutable {
        if (cancelled_) {
          Done(Status::INTERNAL_ERROR);
          return;
        }
        OnComparisonDone(status, std::move(right_change), index->distinct);
      });

  storage_->GetCommitContentsDiff(*ancestor_, *left_, "", std::move(on_next), std::move(callback));
}

void AutoMergeStrategy::AutoMerger::OnComparisonDone(
    Status status, std::unique_ptr<std::vector<storage::EntryChange>> right_changes,
    bool distinct) {
  if (cancelled_) {
    Done(Status::INTERNAL_ERROR);
    return;
  }

  if (status != Status::OK) {
    FXL_LOG(ERROR) << "Unable to compute left diff due to error " << status << ", aborting.";
    Done(status);
    return;
  }

  if (!distinct) {
    // Some keys are overlapping, so we need to proceed like the CUSTOM
    // strategy. We could be more efficient if we reused |right_changes| instead
    // of re-computing the diff inside |ConflictResolverClient|.
    delegated_merge_ = std::make_unique<ConflictResolverClient>(
        storage_, manager_, conflict_resolver_, std::move(left_), std::move(right_),
        std::move(ancestor_),
        callback::MakeScoped(weak_factory_.GetWeakPtr(), [this](Status status) {
          if (cancelled_) {
            Done(Status::INTERNAL_ERROR);
            return;
          }
          Done(status);
        }));

    delegated_merge_->Start();
    return;
  }

  // Here, we reuse the diff we computed before to create the merge commit. As
  // StartMergeCommit uses the left commit (first parameter) as its base, we
  // only have to apply the right diff to it and we are done.
  std::unique_ptr<storage::Journal> journal =
      storage_->StartMergeCommit(std::move(left_), std::move(right_));
  ApplyDiffOnJournal(std::move(journal), std::move(right_changes));
}

void AutoMergeStrategy::AutoMerger::ApplyDiffOnJournal(
    std::unique_ptr<storage::Journal> journal,
    std::unique_ptr<std::vector<storage::EntryChange>> diff) {
  for (const storage::EntryChange& change : *diff) {
    if (change.deleted) {
      journal->Delete(change.entry.key);
    } else {
      journal->Put(change.entry.key, change.entry.object_identifier, change.entry.priority);
    }
  }

  storage_->CommitJournal(std::move(journal),
                          [weak_this = weak_factory_.GetWeakPtr()](
                              Status s, std::unique_ptr<const storage::Commit> /*commit*/) {
                            if (s != Status::OK) {
                              FXL_LOG(ERROR) << "Unable to commit merge journal: " << s;
                            }
                            if (weak_this) {
                              weak_this->Done(s);
                            }
                          });
}

void AutoMergeStrategy::AutoMerger::Cancel() {
  cancelled_ = true;
  if (delegated_merge_) {
    delegated_merge_->Cancel();
  }
}

void AutoMergeStrategy::AutoMerger::Done(Status status) {
  delegated_merge_.reset();
  auto callback = std::move(callback_);
  callback_ = nullptr;
  callback(status);
}

AutoMergeStrategy::AutoMergeStrategy(ConflictResolverPtr conflict_resolver)
    : conflict_resolver_(std::move(conflict_resolver)) {
  conflict_resolver_.set_error_handler([this](zx_status_t status) {
    // If a merge is in progress, it must be terminated.
    if (in_progress_merge_) {
      // The actual cleanup of in_progress_merge_ will happen in its callback
      // callback.
      in_progress_merge_->Cancel();
    }
    if (on_error_) {
      // It is safe to call |on_error_| because the error handler waits for the
      // merges to finish before deleting this object.
      on_error_();
    }
  });
}

AutoMergeStrategy::~AutoMergeStrategy() {}

void AutoMergeStrategy::SetOnError(fit::closure on_error) { on_error_ = std::move(on_error); }

void AutoMergeStrategy::Merge(storage::PageStorage* storage, ActivePageManager* active_page_manager,
                              std::unique_ptr<const storage::Commit> head_1,
                              std::unique_ptr<const storage::Commit> head_2,
                              std::unique_ptr<const storage::Commit> ancestor,
                              fit::function<void(Status)> callback) {
  FXL_DCHECK(storage::Commit::TimestampOrdered(head_1, head_2));
  FXL_DCHECK(!in_progress_merge_);

  in_progress_merge_ = std::make_unique<AutoMergeStrategy::AutoMerger>(
      storage, active_page_manager, conflict_resolver_.get(), std::move(head_2), std::move(head_1),
      std::move(ancestor), [this, callback = std::move(callback)](Status status) {
        in_progress_merge_.reset();
        callback(status);
      });

  in_progress_merge_->Start();
}

void AutoMergeStrategy::Cancel() {
  if (in_progress_merge_) {
    in_progress_merge_->Cancel();
  }
}

}  // namespace ledger
