// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/page_upload.h"

#include <lib/callback/scoped_callback.h>
#include <lib/fit/function.h>

namespace cloud_sync {

PageUpload::PageUpload(callback::ScopedTaskRunner* task_runner, storage::PageStorage* storage,
                       encryption::EncryptionService* encryption_service,
                       cloud_provider::PageCloudPtr* page_cloud, Delegate* delegate,
                       std::unique_ptr<backoff::Backoff> backoff)
    : task_runner_(task_runner),
      storage_(storage),
      encryption_service_(encryption_service),
      page_cloud_(page_cloud),
      delegate_(delegate),
      log_prefix_("Page " + convert::ToHex(storage->GetId()) + " upload sync: "),
      backoff_(std::move(backoff)),
      weak_ptr_factory_(this) {
  // Start to watch right away.
  // |this| ignores the notifications if it is not in the right state.
  storage_->AddCommitWatcher(this);
}

PageUpload::~PageUpload() { storage_->RemoveCommitWatcher(this); }

void PageUpload::StartOrRestartUpload() {
  if (external_state_ == UPLOAD_NOT_STARTED) {
    // When called for the first time, this method is responsible for handling
    // the initial setup.
    SetState(UPLOAD_SETUP);
  }
  // Whether called for the first time or to restart upload, prime the upload
  // process.
  NextState();
}

void PageUpload::OnNewCommits(
    const std::vector<std::unique_ptr<const storage::Commit>>& /*commits*/,
    storage::ChangeSource source) {
  // Only upload the locally created commits.
  // TODO(ppi): revisit this when we have p2p sync, too.
  if (source != storage::ChangeSource::LOCAL) {
    return;
  }

  switch (external_state_) {
    case UPLOAD_SETUP:
    case UPLOAD_IDLE:
    case UPLOAD_PENDING:
    case UPLOAD_WAIT_TOO_MANY_LOCAL_HEADS:
    case UPLOAD_WAIT_REMOTE_DOWNLOAD:
    case UPLOAD_IN_PROGRESS:
      break;
    case UPLOAD_NOT_STARTED:
      // Upload is not started. Ignore the new commits.
    case UPLOAD_TEMPORARY_ERROR:
      // Upload is already scheduled to retry uploading. No need to do anything
      // here.
    case UPLOAD_PERMANENT_ERROR:
      // Can't upload anything anymore. Ignore new commits.
      return;
  }
  NextState();
}

void PageUpload::UploadUnsyncedCommits() {
  FXL_DCHECK(internal_state_ == PageUploadState::PROCESSING);

  if (!delegate_->IsDownloadIdle()) {
    // If a commit batch is currently being downloaded, don't try to start the
    // upload.
    SetState(UPLOAD_WAIT_REMOTE_DOWNLOAD);
    PreviousState();
    return;
  }

  SetState(UPLOAD_PENDING);

  // Retrieve the  of the existing unsynced commits and enqueue them for
  // upload.
  // TODO(ppi): either switch to a paginating API or (better?) ensure that long
  // backlogs of local commits are squashed in storage, as otherwise the list of
  // commits can be possibly very big.
  storage_->GetUnsyncedCommits(callback::MakeScoped(
      weak_ptr_factory_.GetWeakPtr(),
      [this](ledger::Status status, std::vector<std::unique_ptr<const storage::Commit>> commits) {
        if (status != ledger::Status::OK) {
          SetState(UPLOAD_PERMANENT_ERROR);
          HandleError("Failed to retrieve the unsynced commits");
          return;
        }

        VerifyUnsyncedCommits(std::move(commits));
      }));
}

void PageUpload::VerifyUnsyncedCommits(
    std::vector<std::unique_ptr<const storage::Commit>> commits) {
  // If we have no commit to upload, skip.
  if (commits.empty()) {
    SetState(UPLOAD_IDLE);
    PreviousState();
    return;
  }

  std::vector<std::unique_ptr<const storage::Commit>> heads;
  ledger::Status status = storage_->GetHeadCommits(&heads);
  if (status != ledger::Status::OK) {
    HandleError("Failed to retrieve the current heads");
    return;
  }

  FXL_DCHECK(!heads.empty());

  if (!delegate_->IsDownloadIdle()) {
    // If a commit batch is currently being downloaded, don't try to start
    // the upload.
    SetState(UPLOAD_WAIT_REMOTE_DOWNLOAD);
    PreviousState();
    return;
  }

  if (heads.size() > 1u) {
    // Too many local heads.
    SetState(UPLOAD_WAIT_TOO_MANY_LOCAL_HEADS);
    PreviousState();
    return;
  }

  HandleUnsyncedCommits(std::move(commits));
}

void PageUpload::HandleUnsyncedCommits(
    std::vector<std::unique_ptr<const storage::Commit>> commits) {
  FXL_DCHECK(!batch_upload_);
  SetState(UPLOAD_IN_PROGRESS);
  batch_upload_ = std::make_unique<BatchUpload>(
      storage_, encryption_service_, page_cloud_, std::move(commits),
      [this] {
        // Upload succeeded, reset the backoff delay.
        backoff_->Reset();
        batch_upload_.reset();
        PreviousState();
      },
      [this](BatchUpload::ErrorType error_type) {
        switch (error_type) {
          case BatchUpload::ErrorType::TEMPORARY: {
            FXL_LOG(WARNING) << log_prefix_
                             << "commit upload failed due to a connection error, retrying.";
            SetState(UPLOAD_TEMPORARY_ERROR);
            batch_upload_.reset();
            PreviousState();
            RetryWithBackoff([this] { NextState(); });
          } break;
          case BatchUpload::ErrorType::PERMANENT: {
            FXL_LOG(WARNING) << log_prefix_ << "commit upload failed with a permanent error.";
            SetState(UPLOAD_PERMANENT_ERROR);
          } break;
        }
      });
  batch_upload_->Start();
}

void PageUpload::HandleError(const char error_description[]) {
  FXL_LOG(ERROR) << log_prefix_ << error_description << " Stopping sync.";
  SetState(UPLOAD_PERMANENT_ERROR);
}

void PageUpload::RetryWithBackoff(fit::closure callable) {
  task_runner_->PostDelayedTask(
      [this, callable = std::move(callable)]() {
        if (this->external_state_ != UPLOAD_PERMANENT_ERROR) {
          callable();
        }
      },
      backoff_->GetNext());
}

void PageUpload::SetState(UploadSyncState new_state) {
  if (new_state == external_state_) {
    return;
  }
  external_state_ = new_state;
  // Posting to the run loop to handle the case where the delegate will delete
  // this class in the SetUploadState method.
  // TODO(qsr): Aggregate changed state, so that a change from A -> B -> A do
  //            not send any signal.
  task_runner_->PostTask([this] { delegate_->SetUploadState(external_state_); });
}

bool PageUpload::IsIdle() {
  switch (external_state_) {
    case UPLOAD_NOT_STARTED:
    case UPLOAD_IDLE:
    // Note: this is considered idle because the reason for being blocked is
    // external to the class - there's nothing to do on our side.
    case UPLOAD_WAIT_TOO_MANY_LOCAL_HEADS:
    case UPLOAD_PERMANENT_ERROR:
      return true;
      break;
    case UPLOAD_SETUP:
    case UPLOAD_PENDING:
    case UPLOAD_WAIT_REMOTE_DOWNLOAD:
    case UPLOAD_TEMPORARY_ERROR:
    case UPLOAD_IN_PROGRESS:
      return false;
      break;
  }
}

void PageUpload::NextState() {
  switch (internal_state_) {
    case PageUploadState::NO_COMMIT:
      internal_state_ = PageUploadState::PROCESSING;
      UploadUnsyncedCommits();
      return;
    case PageUploadState::PROCESSING:
    case PageUploadState::PROCESSING_NEW_COMMIT:
      internal_state_ = PageUploadState::PROCESSING_NEW_COMMIT;
      return;
  }
}

void PageUpload::PreviousState() {
  switch (internal_state_) {
    case PageUploadState::NO_COMMIT:
      FXL_NOTREACHED() << "Bad state";
    case PageUploadState::PROCESSING:
      internal_state_ = PageUploadState::NO_COMMIT;
      if (external_state_ == UPLOAD_IN_PROGRESS) {
        SetState(UPLOAD_IDLE);
      }
      return;
    case PageUploadState::PROCESSING_NEW_COMMIT:
      internal_state_ = PageUploadState::PROCESSING;
      UploadUnsyncedCommits();
      return;
  }
}

}  // namespace cloud_sync
