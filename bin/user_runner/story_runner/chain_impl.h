// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_CHAIN_IMPL_H_
#define PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_CHAIN_IMPL_H_

#include <vector>

#include <fuchsia/modular/cpp/fidl.h>

#include "lib/fidl/cpp/binding_set.h"
#include "lib/fxl/macros.h"

namespace fuchsia {
namespace modular {

class ChainImpl {
 public:
  ChainImpl(const fidl::VectorPtr<fidl::StringPtr>& path,
            const ChainData& chain_data);
  ~ChainImpl();

  const fidl::VectorPtr<fidl::StringPtr>& chain_path() const { return path_; }

  LinkPathPtr GetLinkPathForKey(const fidl::StringPtr& key);

 private:
  fidl::VectorPtr<fidl::StringPtr> path_;
  ChainData chain_data_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ChainImpl);
};

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_BIN_USER_RUNNER_STORY_RUNNER_CHAIN_IMPL_H_
