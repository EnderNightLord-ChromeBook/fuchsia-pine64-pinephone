// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_COMMAND_RUNNERS_SET_LINK_VALUE_COMMAND_RUNNER_H_
#define PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_COMMAND_RUNNERS_SET_LINK_VALUE_COMMAND_RUNNER_H_

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async/cpp/operation.h>

#include "peridot/bin/sessionmgr/puppet_master/command_runners/command_runner.h"

namespace modular {

class SetLinkValueCommandRunner : public CommandRunner {
 public:
  SetLinkValueCommandRunner();
  ~SetLinkValueCommandRunner() override;

  void Execute(fidl::StringPtr story_id, StoryStorage* story_storage,
               fuchsia::modular::StoryCommand command,
               fit::function<void(fuchsia::modular::ExecuteResult)> done) override;

 private:
  OperationQueue operation_queue_;
};

}  // namespace modular

#endif  // PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_COMMAND_RUNNERS_SET_LINK_VALUE_COMMAND_RUNNER_H_
