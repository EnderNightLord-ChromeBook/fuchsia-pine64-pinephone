// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_APP_ACTIVE_PAGE_MANAGER_CONTAINER_H_
#define SRC_LEDGER_BIN_APP_ACTIVE_PAGE_MANAGER_CONTAINER_H_

#include <lib/fidl/cpp/interface_request.h>
#include <lib/fit/function.h>

#include <string>

#include "src/ledger/bin/app/active_page_manager.h"
#include "src/ledger/bin/app/page_connection_notifier.h"
#include "src/ledger/bin/app/page_impl.h"
#include "src/ledger/bin/app/page_usage_listener.h"
#include "src/ledger/bin/app/types.h"
#include "src/ledger/bin/storage/public/types.h"

namespace ledger {

// Container for a ActivePageManager that keeps track of in-flight page
// requests and callbacks and fires them when the ActivePageManager is
// available.
class ActivePageManagerContainer {
 public:
  ActivePageManagerContainer(std::string ledger_name, storage::PageId page_id,
                             PageUsageListener* page_usage_listener);
  ~ActivePageManagerContainer();

  void set_on_empty(fit::closure on_empty_callback);

  // Keeps track of |page| and |callback|. Binds |page| and fires |callback|
  // when a ActivePageManager is available or an error occurs.
  void BindPage(fidl::InterfaceRequest<Page> page_request, fit::function<void(Status)> callback);

  // Registers a new internal request for PageStorage.
  void NewInternalRequest(fit::function<void(Status, ExpiringToken, ActivePageManager*)> callback);

  // Sets the ActivePageManager or the error status for the container. This
  // notifies all awaiting callbacks and binds all pages in case of success.
  void SetActivePageManager(Status status, std::unique_ptr<ActivePageManager> active_page_manager);

  // Returns true if there is at least one active external page connection.
  bool PageConnectionIsOpen();

 private:
  // Checks whether this container is empty, and calls the |on_empty_callback_|
  // if it is.
  void CheckEmpty();

  const storage::PageId page_id_;

  std::unique_ptr<ActivePageManager> active_page_manager_;
  // |status_| holds the status given to |SetActivePageManager|. If
  // |active_page_manager_is_set_| is true, |status_| is |Status::OK| if and
  // only if |active_page_manager_| is not null.
  Status status_ = Status::OK;
  // |active_page_manager_is_set_| if |SetActivePageManager| has been called.
  // |active_page_manager_| may still be null.
  bool active_page_manager_is_set_ = false;

  PageConnectionNotifier connection_notifier_;
  // page_impls_ is only populated before active_page_manager_ is set. Once the
  // ActivePageManager is created and assigned to active_page_manager_, the
  // PageImpls stored in page_impls_ are handed off to that ActivePageManager
  // and page_impls_ is not used again.
  std::vector<std::pair<std::unique_ptr<PageImpl>, fit::function<void(Status)>>> page_impls_;
  std::vector<fit::function<void(Status, ExpiringToken, ActivePageManager*)>>
      internal_request_callbacks_;
  fit::closure on_empty_callback_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ActivePageManagerContainer);
};

}  // namespace ledger

#endif  // SRC_LEDGER_BIN_APP_ACTIVE_PAGE_MANAGER_CONTAINER_H_
