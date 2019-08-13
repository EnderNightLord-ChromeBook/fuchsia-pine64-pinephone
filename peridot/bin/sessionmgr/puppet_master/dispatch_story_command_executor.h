// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_DISPATCH_STORY_COMMAND_EXECUTOR_H_
#define PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_DISPATCH_STORY_COMMAND_EXECUTOR_H_

#include <lib/fidl/cpp/string.h>

#include "peridot/bin/sessionmgr/puppet_master/command_runners/command_runner.h"
#include "peridot/bin/sessionmgr/puppet_master/story_command_executor.h"

namespace modular {

class OperationContainer;

// An implementation of StoryCommandExecutor which dispatches execution of
// individual StoryCommands to CommandRunners for each union tag of
// fuchsia::modular::StoryCommand.
class DispatchStoryCommandExecutor : public StoryCommandExecutor {
 public:
  DispatchStoryCommandExecutor(
      SessionStorage* session_storage,
      std::map<fuchsia::modular::StoryCommand::Tag, std::unique_ptr<CommandRunner>>
          command_runners);
  ~DispatchStoryCommandExecutor() override;

 private:
  // |StoryCommandExecutor|
  void ExecuteCommandsInternal(fidl::StringPtr story_id,
                               std::vector<fuchsia::modular::StoryCommand> commands,
                               fit::function<void(fuchsia::modular::ExecuteResult)> done) override;

  class ExecuteStoryCommandsCall;

  SessionStorage* const session_storage_;  // Not owned.
  const std::map<fuchsia::modular::StoryCommand::Tag, std::unique_ptr<CommandRunner>>
      command_runners_;

  // Lookup table from fuchsia::modular::StoryCommand union tag to a
  // human-readable string.
  const std::map<fuchsia::modular::StoryCommand::Tag, const char*> story_command_tag_strings_;

  std::map<fidl::StringPtr, OperationQueue> operation_queues_;
};

}  // namespace modular

#endif  // PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_DISPATCH_STORY_COMMAND_EXECUTOR_H_
