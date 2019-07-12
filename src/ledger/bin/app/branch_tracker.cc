// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/app/branch_tracker.h"

#include <lib/callback/scoped_callback.h>
#include <lib/callback/waiter.h>
#include <lib/fit/defer.h>
#include <lib/fit/function.h>

#include <vector>

#include "src/ledger/bin/app/active_page_manager.h"
#include "src/ledger/bin/app/diff_utils.h"
#include "src/ledger/bin/app/fidl/serialization_size.h"
#include "src/ledger/bin/app/page_utils.h"
#include "src/lib/fxl/memory/ref_ptr.h"

namespace ledger {
class BranchTracker::PageWatcherContainer {
 public:
  PageWatcherContainer(coroutine::CoroutineService* coroutine_service, PageWatcherPtr watcher,
                       ActivePageManager* active_page_manager, storage::PageStorage* storage,
                       std::unique_ptr<const storage::Commit> base_commit, std::string key_prefix)
      : change_in_flight_(false),
        last_commit_(std::move(base_commit)),
        coroutine_service_(coroutine_service),
        key_prefix_(std::move(key_prefix)),
        active_page_manager_(active_page_manager),
        storage_(storage),
        interface_(std::move(watcher)),
        weak_factory_(this) {
    interface_.set_error_handler([this](zx_status_t status) {
      if (handler_) {
        handler_->Resume(coroutine::ContinuationStatus::INTERRUPTED);
      }
      FXL_DCHECK(!handler_);
      if (on_empty_callback_) {
        on_empty_callback_();
      }
    });
  }

  ~PageWatcherContainer() {
    if (on_drained_) {
      on_drained_();
    }
    if (handler_) {
      handler_->Resume(coroutine::ContinuationStatus::INTERRUPTED);
    }
    FXL_DCHECK(!handler_);
  }

  void set_on_empty(fit::closure on_empty_callback) {
    on_empty_callback_ = std::move(on_empty_callback);
  }

  void UpdateCommit(std::unique_ptr<const storage::Commit> commit) {
    current_commit_ = std::move(commit);
    FXL_DCHECK(current_commit_);
    SendCommit();
  }

  // Sets a callback to be called when all pending updates are sent. If all
  // updates are already sent, the callback will be called immediately. This
  // callback will only be called once; |SetOnDrainedCallback| should be called
  // again to set a new callback after the first one is called. Setting a
  // callback while a previous one is still active will execute the previous
  // callback.
  void SetOnDrainedCallback(fit::closure on_drained) {
    // If a transaction is committed or rolled back before all watchers have
    // been drained, we do not want to continue blocking until they drain. Thus,
    // we declare them drained right away and proceed.
    if (on_drained_) {
      on_drained_();
      on_drained_ = nullptr;
    }
    on_drained_ = std::move(on_drained);
    if (Drained() && on_drained_) {
      on_drained_();
      on_drained_ = nullptr;
    }
  }

 private:
  // Returns true if all changes have been sent to the watcher client, false
  // otherwise.
  bool Drained() { return !current_commit_ || last_commit_->GetId() == current_commit_->GetId(); }

  std::vector<PageChange> PaginateChanges(PageChangePtr change) {
    std::vector<PageChange> changes;

    // These are initialized to valid values in the first run of the loop.
    size_t fidl_size = -1;
    size_t handle_count = -1;
    size_t timestamp = change->timestamp;
    auto entries = std::move(change->changed_entries);
    auto deletions = std::move(change->deleted_keys);
    for (size_t i = 0, j = 0; i < entries.size() || j < deletions.size();) {
      bool add_entry = i < entries.size() &&
                       (j == deletions.size() || convert::ExtendedStringView(entries.at(i).key) <
                                                     convert::ExtendedStringView(deletions.at(j)));
      size_t entry_size = add_entry ? fidl_serialization::GetEntrySize(entries.at(i).key.size())
                                    : fidl_serialization::GetByteVectorSize(deletions.at(j).size());
      size_t entry_handle_count = add_entry ? 1 : 0;

      if (changes.empty() || fidl_size + entry_size > fidl_serialization::kMaxInlineDataSize ||
          handle_count + entry_handle_count > fidl_serialization::kMaxMessageHandles) {
        PageChange change;
        change.timestamp = timestamp;
        change.changed_entries.resize(0);
        change.deleted_keys.resize(0);
        changes.push_back(std::move(change));
        fidl_size = fidl_serialization::kPageChangeHeaderSize;
        handle_count = 0u;
      }
      fidl_size += entry_size;
      handle_count += entry_handle_count;
      if (add_entry) {
        changes.back().changed_entries.push_back(std::move(entries.at(i)));
        ++i;
      } else {
        changes.back().deleted_keys.push_back(std::move(deletions.at(j)));
        ++j;
      }
    }
    return changes;
  }

  void SendChange(PageChange page_change, ResultState state,
                  std::unique_ptr<const storage::Commit> new_commit, fit::closure on_done) {
    interface_->OnChange(
        std::move(page_change), state,
        [this, state, new_commit = std::move(new_commit), on_done = std::move(on_done)](
            fidl::InterfaceRequest<PageSnapshot> snapshot_request) mutable {
          if (snapshot_request) {
            active_page_manager_->BindPageSnapshot(new_commit->Clone(), std::move(snapshot_request),
                                                   key_prefix_);
          }
          if (state != ResultState::COMPLETED && state != ResultState::PARTIAL_COMPLETED) {
            on_done();
            return;
          }
          change_in_flight_ = false;
          last_commit_.swap(new_commit);
          // SendCommit will start handling the following commit, so we need
          // to make sure on_done() is called before that.
          on_done();
          SendCommit();
        });
  }

  // Sends a commit to the watcher if needed.
  void SendCommit() {
    if (change_in_flight_) {
      return;
    }

    if (Drained()) {
      if (on_drained_) {
        on_drained_();
        on_drained_ = nullptr;
      }
      return;
    }

    change_in_flight_ = true;

    // TODO(etiennej): See LE-74: clean object ownership
    diff_utils::ComputePageChange(
        storage_, *last_commit_, *current_commit_, key_prefix_, key_prefix_,
        diff_utils::PaginationBehavior::NO_PAGINATION,
        callback::MakeScoped(
            weak_factory_.GetWeakPtr(),
            [this, new_commit = std::move(current_commit_)](
                Status status, std::pair<PageChangePtr, std::string> page_change_ptr) mutable {
              if (status != Status::OK) {
                // This change notification is abandonned. At the next commit,
                // we will try again (but not before). The next notification
                // will cover both this change and the next.
                FXL_LOG(ERROR) << "Unable to compute PageChange for Watch update.";
                change_in_flight_ = false;
                return;
              }

              if (!page_change_ptr.first) {
                change_in_flight_ = false;
                last_commit_.swap(new_commit);
                SendCommit();
                return;
              }
              std::vector<PageChange> paginated_changes =
                  PaginateChanges(std::move(page_change_ptr.first));
              if (paginated_changes.size() == 1) {
                SendChange(std::move(paginated_changes[0]), ResultState::COMPLETED,
                           std::move(new_commit), [] {});
                return;
              }
              coroutine_service_->StartCoroutine([this, new_commit = std::move(new_commit),
                                                  paginated_changes = std::move(paginated_changes)](
                                                     coroutine::CoroutineHandler* handler) mutable {
                auto guard = fit::defer([this] { handler_ = nullptr; });
                FXL_DCHECK(!handler_);
                handler_ = handler;
                for (size_t i = 0; i < paginated_changes.size(); ++i) {
                  ResultState state;
                  if (i == 0) {
                    state = ResultState::PARTIAL_STARTED;
                  } else if (i == paginated_changes.size() - 1) {
                    state = ResultState::PARTIAL_COMPLETED;
                  } else {
                    state = ResultState::PARTIAL_CONTINUED;
                  }
                  if (coroutine::SyncCall(handler, [this, change = std::move(paginated_changes[i]),
                                                    state, new_commit = new_commit->Clone()](
                                                       fit::closure on_done) mutable {
                        SendChange(std::move(change), state, std::move(new_commit),
                                   std::move(on_done));
                      }) == coroutine::ContinuationStatus::INTERRUPTED) {
                    return;
                  }
                }
              });
            }));
  }

  fit::closure on_drained_ = nullptr;
  fit::closure on_empty_callback_ = nullptr;
  bool change_in_flight_;
  std::unique_ptr<const storage::Commit> last_commit_;
  std::unique_ptr<const storage::Commit> current_commit_;
  coroutine::CoroutineService* coroutine_service_;
  coroutine::CoroutineHandler* handler_ = nullptr;
  const std::string key_prefix_;
  ActivePageManager* active_page_manager_;
  storage::PageStorage* storage_;
  PageWatcherPtr interface_;

  // This must be the last member of the class.
  fxl::WeakPtrFactory<PageWatcherContainer> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(PageWatcherContainer);
};

BranchTracker::BranchTracker(coroutine::CoroutineService* coroutine_service,
                             ActivePageManager* manager, storage::PageStorage* storage)
    : coroutine_service_(coroutine_service),
      manager_(manager),
      storage_(storage),
      transaction_in_progress_(false),
      current_commit_(nullptr),
      weak_factory_(this) {
  watchers_.set_on_empty([this] { CheckEmpty(); });
}

BranchTracker::~BranchTracker() { storage_->RemoveCommitWatcher(this); }

Status BranchTracker::Init() {
  std::vector<std::unique_ptr<const storage::Commit>> commits;
  Status status = storage_->GetHeadCommits(&commits);
  if (status != Status::OK) {
    return status;
  }

  FXL_DCHECK(!commits.empty());
  FXL_DCHECK(commits[0]);
  FXL_DCHECK(!current_commit_);

  current_commit_ = std::move(commits[0]);
  storage_->AddCommitWatcher(this);
  return Status::OK;
}

void BranchTracker::set_on_empty(fit::closure on_empty_callback) {
  on_empty_callback_ = std::move(on_empty_callback);
}

std::unique_ptr<const storage::Commit> BranchTracker::GetBranchHead() {
  return current_commit_->Clone();
}

void BranchTracker::OnNewCommits(const std::vector<std::unique_ptr<const storage::Commit>>& commits,
                                 storage::ChangeSource /*source*/) {
  FXL_DCHECK(current_commit_);
  bool changed = false;
  const std::unique_ptr<const storage::Commit>* new_current_commit = &current_commit_;
  for (const auto& commit : commits) {
    if (commit->GetId() == (*new_current_commit)->GetId()) {
      continue;
    }
    // This assumes commits are received in (partial) order. If the commit
    // doesn't have current_commit_ as a parent it is not part of this branch
    // and should be ignored.
    std::vector<storage::CommitIdView> parent_ids = commit->GetParentIds();
    if (std::find(parent_ids.begin(), parent_ids.end(), (*new_current_commit)->GetId()) ==
        parent_ids.end()) {
      continue;
    }
    changed = true;
    new_current_commit = &commit;
  }
  if (changed) {
    current_commit_ = (*new_current_commit)->Clone();
    FXL_DCHECK(current_commit_);
  }

  if (!changed || transaction_in_progress_) {
    return;
  }
  for (auto& watcher : watchers_) {
    watcher.UpdateCommit(current_commit_->Clone());
  }
}

void BranchTracker::StartTransaction(fit::closure watchers_drained_callback) {
  FXL_DCHECK(!transaction_in_progress_);
  transaction_in_progress_ = true;
  auto waiter = fxl::MakeRefCounted<callback::CompletionWaiter>();
  for (auto& watcher : watchers_) {
    watcher.SetOnDrainedCallback(waiter->NewCallback());
  }
  waiter->Finalize(std::move(watchers_drained_callback));
}

void BranchTracker::StopTransaction(std::unique_ptr<const storage::Commit> commit) {
  FXL_DCHECK(transaction_in_progress_ || !commit);

  if (!transaction_in_progress_) {
    return;
  }
  transaction_in_progress_ = false;

  if (commit) {
    current_commit_ = std::move(commit);
  }

  for (auto& watcher : watchers_) {
    watcher.SetOnDrainedCallback(nullptr);
    watcher.UpdateCommit(current_commit_->Clone());
  }
}

void BranchTracker::RegisterPageWatcher(PageWatcherPtr page_watcher_ptr,
                                        std::unique_ptr<const storage::Commit> base_commit,
                                        std::string key_prefix) {
  watchers_.emplace(coroutine_service_, std::move(page_watcher_ptr), manager_, storage_,
                    std::move(base_commit), std::move(key_prefix));
}

bool BranchTracker::IsEmpty() { return watchers_.empty(); }

void BranchTracker::CheckEmpty() {
  if (on_empty_callback_ && IsEmpty()) {
    on_empty_callback_();
  }
}

}  // namespace ledger
