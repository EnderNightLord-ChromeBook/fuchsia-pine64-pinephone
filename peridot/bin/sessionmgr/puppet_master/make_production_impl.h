// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_MAKE_PRODUCTION_IMPL_H_
#define PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_MAKE_PRODUCTION_IMPL_H_

#include <memory>

#include "peridot/bin/sessionmgr/puppet_master/dispatch_story_command_executor.h"

namespace modular {

class StoryCommandExecutor;

// Returns a StoryCommandExecutor suitable for use in production.
std::unique_ptr<StoryCommandExecutor> MakeProductionStoryCommandExecutor(
    SessionStorage* session_storage, fuchsia::modular::FocusProviderPtr focus_provider,
    fuchsia::modular::ModuleResolver* module_resolver,
    fuchsia::modular::EntityResolver* entity_resolver,
    fit::function<void(std::string, std::vector<std::string>)> module_focuser);

}  // namespace modular

#endif  // PERIDOT_BIN_SESSIONMGR_PUPPET_MASTER_MAKE_PRODUCTION_IMPL_H_
