// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/cloud_sync/impl/page_sync_impl.h"

#include <lib/fit/function.h>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "src/ledger/bin/cloud_sync/impl/constants.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/logging.h"

namespace cloud_sync {

PageSyncImpl::PageSyncImpl(async_dispatcher_t* dispatcher, storage::PageStorage* storage,
                           storage::PageSyncClient* sync_client,
                           encryption::EncryptionService* encryption_service,
                           cloud_provider::PageCloudPtr page_cloud,
                           std::unique_ptr<backoff::Backoff> download_backoff,
                           std::unique_ptr<backoff::Backoff> upload_backoff,
                           std::unique_ptr<SyncStateWatcher> ledger_watcher)
    : storage_(storage),
      sync_client_(sync_client),
      encryption_service_(encryption_service),
      page_cloud_(std::move(page_cloud)),
      log_prefix_("Page " + convert::ToHex(storage->GetId()) + " sync: "),
      ledger_watcher_(std::move(ledger_watcher)),
      task_runner_(dispatcher) {
  FXL_DCHECK(storage_);
  FXL_DCHECK(page_cloud_);
  // We need to initialize page_download_ after task_runner_, but task_runner_
  // must be the last field.
  page_download_ =
      std::make_unique<PageDownload>(&task_runner_, storage_, sync_client_, encryption_service_,
                                     &page_cloud_, this, std::move(download_backoff));
  page_upload_ = std::make_unique<PageUpload>(&task_runner_, storage_, encryption_service_,
                                              &page_cloud_, this, std::move(upload_backoff));
  page_cloud_.set_error_handler([this](zx_status_t status) {
    if (on_unrecoverable_error_ && !error_callback_already_called_) {
      error_callback_already_called_ = true;
      on_unrecoverable_error_();
    }
  });
}

PageSyncImpl::~PageSyncImpl() {
  if (on_delete_) {
    on_delete_();
  }
}

void PageSyncImpl::EnableUpload() {
  enable_upload_ = true;

  if (!started_) {
    // We will start upload when this object is started.
    return;
  }

  if (upload_state_ == UPLOAD_NOT_STARTED) {
    page_upload_->StartOrRestartUpload();
  }
}

void PageSyncImpl::Start() {
  FXL_DCHECK(!started_);
  started_ = true;

  page_download_->StartDownload();
  if (enable_upload_) {
    page_upload_->StartOrRestartUpload();
  }
}

void PageSyncImpl::SetOnIdle(fit::closure on_idle) {
  FXL_DCHECK(!on_idle_);
  FXL_DCHECK(!started_);
  on_idle_ = std::move(on_idle);
}

bool PageSyncImpl::IsIdle() { return page_upload_->IsIdle() && page_download_->IsIdle(); }

void PageSyncImpl::SetOnBacklogDownloaded(fit::closure on_backlog_downloaded) {
  FXL_DCHECK(!on_backlog_downloaded_);
  FXL_DCHECK(!started_);
  on_backlog_downloaded_ = std::move(on_backlog_downloaded);
}

void PageSyncImpl::SetSyncWatcher(SyncStateWatcher* watcher) {
  page_watcher_ = watcher;
  if (page_watcher_) {
    page_watcher_->Notify(download_state_, upload_state_);
  }
}

void PageSyncImpl::SetOnUnrecoverableError(fit::closure on_unrecoverable_error) {
  on_unrecoverable_error_ = std::move(on_unrecoverable_error);
}

void PageSyncImpl::HandleError() {
  if (error_callback_already_called_) {
    return;
  }

  if (on_unrecoverable_error_) {
    error_callback_already_called_ = true;
    // This may destruct the object.
    on_unrecoverable_error_();
  }
}

void PageSyncImpl::CheckIdle() {
  if (IsIdle()) {
    if (on_idle_) {
      on_idle_();
    }
  }
}

void PageSyncImpl::NotifyStateWatcher() {
  if (ledger_watcher_) {
    ledger_watcher_->Notify(download_state_, upload_state_);
  }
  if (page_watcher_) {
    page_watcher_->Notify(download_state_, upload_state_);
  }
  CheckIdle();
}

void PageSyncImpl::SetDownloadState(DownloadSyncState next_download_state) {
  if (download_state_ == DOWNLOAD_BACKLOG && next_download_state != DOWNLOAD_PERMANENT_ERROR &&
      on_backlog_downloaded_) {
    on_backlog_downloaded_();
  }

  if (download_state_ != DOWNLOAD_IDLE && next_download_state == DOWNLOAD_IDLE && enable_upload_) {
    page_upload_->StartOrRestartUpload();
  }

  download_state_ = next_download_state;
  NotifyStateWatcher();

  if (next_download_state == DOWNLOAD_PERMANENT_ERROR) {
    // This may destruct the object.
    HandleError();
    return;
  }
}

void PageSyncImpl::SetUploadState(UploadSyncState next_upload_state) {
  upload_state_ = next_upload_state;
  NotifyStateWatcher();

  if (next_upload_state == UPLOAD_PERMANENT_ERROR) {
    // This may destruct the object.
    HandleError();
    return;
  }
}

bool PageSyncImpl::IsDownloadIdle() { return page_download_->IsIdle(); }

}  // namespace cloud_sync
