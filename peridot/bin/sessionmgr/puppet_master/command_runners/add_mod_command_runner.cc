// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/sessionmgr/puppet_master/command_runners/add_mod_command_runner.h"

#include <src/lib/fxl/logging.h>

#include "peridot/bin/sessionmgr/puppet_master/command_runners/operation_calls/add_mod_call.h"

namespace modular {

AddModCommandRunner::AddModCommandRunner(fuchsia::modular::ModuleResolver* const module_resolver,
                                         fuchsia::modular::EntityResolver* const entity_resolver)
    : module_resolver_(module_resolver), entity_resolver_(entity_resolver) {
  FXL_DCHECK(module_resolver_);
  FXL_DCHECK(entity_resolver_);
}

AddModCommandRunner::~AddModCommandRunner() = default;

void AddModCommandRunner::Execute(fidl::StringPtr story_id, StoryStorage* const story_storage,
                                  fuchsia::modular::StoryCommand command,
                                  fit::function<void(fuchsia::modular::ExecuteResult)> done) {
  FXL_CHECK(command.is_add_mod());

  auto& add_mod = command.add_mod();
  if (add_mod.mod_name.size() == 0 && !add_mod.mod_name_transitional.has_value()) {
    fuchsia::modular::ExecuteResult result;
    result.status = fuchsia::modular::ExecuteStatus::INVALID_COMMAND;
    result.error_message = "A Module name must be specified";
    done(result);
    return;
  }

  AddModParams params;
  params.parent_mod_path = std::move(add_mod.surface_parent_mod_name.value_or({}));
  if (add_mod.mod_name_transitional.has_value()) {
    params.mod_name = add_mod.mod_name_transitional.value();
  } else if (add_mod.mod_name.size() == 1) {
    params.mod_name = add_mod.mod_name[0];
  } else {
    params.mod_name = add_mod.mod_name.back();

    add_mod.mod_name.pop_back();
    params.parent_mod_path = add_mod.mod_name;
  }
  params.is_embedded = false;
  params.intent = std::move(add_mod.intent);
  params.surface_relation =
      std::make_unique<fuchsia::modular::SurfaceRelation>(std::move(add_mod.surface_relation));
  params.module_source = fuchsia::modular::ModuleSource::EXTERNAL;

  AddAddModOperation(&operation_queue_, story_storage, module_resolver_, entity_resolver_,
                     std::move(params),
                     [done = std::move(done)](fuchsia::modular::ExecuteResult result,
                                              fuchsia::modular::ModuleData module_data) {
                       done(std::move(result));
                     });
}

}  // namespace modular
