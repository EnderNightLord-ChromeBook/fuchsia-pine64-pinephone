// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/app/active_page_manager_container.h"

#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/function.h>

#include <string>

#include "src/ledger/bin/app/active_page_manager.h"
#include "src/ledger/bin/app/page_usage_listener.h"
#include "src/ledger/bin/app/types.h"
#include "src/ledger/bin/storage/public/types.h"
#include "src/lib/fxl/logging.h"

namespace ledger {

ActivePageManagerContainer::ActivePageManagerContainer(std::string ledger_name,
                                                       storage::PageId page_id,
                                                       PageUsageListener* page_usage_listener)
    : page_id_(page_id),
      connection_notifier_(std::move(ledger_name), std::move(page_id), page_usage_listener) {}

ActivePageManagerContainer::~ActivePageManagerContainer() = default;

void ActivePageManagerContainer::set_on_empty(fit::closure on_empty_callback) {
  on_empty_callback_ = std::move(on_empty_callback);
  connection_notifier_.set_on_empty([this] { CheckEmpty(); });
  if (active_page_manager_) {
    active_page_manager_->set_on_empty(
        [this] { connection_notifier_.UnregisterExternalRequests(); });
  }
}

void ActivePageManagerContainer::BindPage(fidl::InterfaceRequest<Page> page_request,
                                          fit::function<void(Status)> callback) {
  connection_notifier_.RegisterExternalRequest();

  if (status_ != Status::OK) {
    callback(status_);
    return;
  }
  auto page_impl = std::make_unique<PageImpl>(page_id_, std::move(page_request));
  if (active_page_manager_) {
    active_page_manager_->AddPageImpl(std::move(page_impl), std::move(callback));
    return;
  }
  page_impls_.emplace_back(std::move(page_impl), std::move(callback));
}

void ActivePageManagerContainer::NewInternalRequest(
    fit::function<void(Status, ExpiringToken, ActivePageManager*)> callback) {
  if (status_ != Status::OK) {
    callback(status_, fit::defer<fit::closure>([] {}), nullptr);
    return;
  }

  if (active_page_manager_) {
    callback(status_, connection_notifier_.NewInternalRequestToken(), active_page_manager_.get());
    return;
  }

  internal_request_callbacks_.push_back(std::move(callback));
}

void ActivePageManagerContainer::SetActivePageManager(
    Status status, std::unique_ptr<ActivePageManager> active_page_manager) {
  auto token = connection_notifier_.NewInternalRequestToken();
  TRACE_DURATION("ledger", "page_manager_container_set_page_manager");

  FXL_DCHECK(!active_page_manager_is_set_);
  FXL_DCHECK((status != Status::OK) == !active_page_manager);
  status_ = status;
  active_page_manager_ = std::move(active_page_manager);
  active_page_manager_is_set_ = true;

  for (auto& [page_impl, callback] : page_impls_) {
    if (active_page_manager_) {
      active_page_manager_->AddPageImpl(std::move(page_impl), std::move(callback));
    } else {
      callback(status_);
    }
  }
  page_impls_.clear();

  for (auto& callback : internal_request_callbacks_) {
    if (!active_page_manager_) {
      callback(status_, fit::defer<fit::closure>([] {}), nullptr);
      continue;
    }
    callback(status_, connection_notifier_.NewInternalRequestToken(), active_page_manager_.get());
  }
  internal_request_callbacks_.clear();

  if (active_page_manager_) {
    active_page_manager_->set_on_empty(
        [this] { connection_notifier_.UnregisterExternalRequests(); });
  } else {
    connection_notifier_.UnregisterExternalRequests();
  }
  // |CheckEmpty| called when |token| goes out of scope.
}

bool ActivePageManagerContainer::PageConnectionIsOpen() {
  return (active_page_manager_ && !active_page_manager_->IsEmpty()) || !page_impls_.empty();
}

void ActivePageManagerContainer::CheckEmpty() {
  // The ActivePageManagerContainer is not considered empty until
  // |SetActivePageManager| has been called.
  if (on_empty_callback_ && connection_notifier_.IsEmpty() && active_page_manager_is_set_ &&
      (!active_page_manager_ || active_page_manager_->IsEmpty())) {
    on_empty_callback_();
  }
}

}  // namespace ledger
