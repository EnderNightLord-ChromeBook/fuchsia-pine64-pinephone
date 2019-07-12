// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_CLOUD_PROVIDER_IN_MEMORY_LIB_FAKE_PAGE_CLOUD_H_
#define SRC_LEDGER_CLOUD_PROVIDER_IN_MEMORY_LIB_FAKE_PAGE_CLOUD_H_

#include <fuchsia/ledger/cloud/cpp/fidl.h>
#include <lib/callback/auto_cleanable.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fit/function.h>

#include "src/ledger/bin/fidl/include/types.h"
#include "src/ledger/cloud_provider_in_memory/lib/types.h"
#include "src/ledger/lib/commit_pack/commit_pack.h"
#include "src/lib/fxl/macros.h"

namespace ledger {

class FakePageCloud : public cloud_provider::PageCloud {
 public:
  explicit FakePageCloud(InjectNetworkError inject_network_error);
  ~FakePageCloud() override;

  void set_on_empty(fit::closure on_empty) { on_empty_ = std::move(on_empty); }

  void Bind(fidl::InterfaceRequest<cloud_provider::PageCloud> request);

 private:
  void SendPendingCommits();
  bool MustReturnError(uint64_t request_signature);

  // cloud_provider::PageCloud:
  void AddCommits(cloud_provider::CommitPack commits, AddCommitsCallback callback) override;
  void GetCommits(std::unique_ptr<cloud_provider::PositionToken> min_position_token,
                  GetCommitsCallback callback) override;
  void AddObject(std::vector<uint8_t> id, fuchsia::mem::Buffer data,
                 cloud_provider::ReferencePack references, AddObjectCallback callback) override;
  void GetObject(std::vector<uint8_t> id, GetObjectCallback callback) override;
  void SetWatcher(std::unique_ptr<cloud_provider::PositionToken> min_position_token,
                  fidl::InterfaceHandle<cloud_provider::PageCloudWatcher> watcher,
                  SetWatcherCallback callback) override;

  InjectNetworkError inject_network_error_;
  std::map<uint64_t, size_t> remaining_errors_to_inject_;

  fidl::BindingSet<cloud_provider::PageCloud> bindings_;
  fit::closure on_empty_;

  std::vector<cloud_provider::CommitPackEntry> commits_;
  std::map<std::string, std::string> objects_;

  // Watchers set by the client.
  class WatcherContainer;
  callback::AutoCleanableSet<WatcherContainer> containers_;

  FXL_DISALLOW_COPY_AND_ASSIGN(FakePageCloud);
};

}  // namespace ledger

#endif  // SRC_LEDGER_CLOUD_PROVIDER_IN_MEMORY_LIB_FAKE_PAGE_CLOUD_H_
