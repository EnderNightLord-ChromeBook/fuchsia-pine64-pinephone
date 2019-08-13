// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/sessionmgr/puppet_master/command_runners/remove_mod_command_runner.h"

#include "gtest/gtest.h"
#include "peridot/lib/testing/test_with_session_storage.h"

namespace modular {
namespace {

class RemoveModCommandRunnerTest : public testing::TestWithSessionStorage {
 protected:
  std::unique_ptr<RemoveModCommandRunner> MakeRunner() {
    return std::make_unique<RemoveModCommandRunner>();
  }

  std::vector<std::string> MakeModulePath(std::string path) {
    std::vector<std::string> module_path;
    module_path.push_back(path);
    return module_path;
  }

  void InitModuleData(StoryStorage* const story_storage, std::vector<std::string> path) {
    fuchsia::modular::ModuleData module_data;
    module_data.module_path = std::move(path);
    module_data.intent = fuchsia::modular::Intent::New();
    module_data.module_deleted = false;

    WriteModuleData(story_storage, std::move(module_data));
  }
};

TEST_F(RemoveModCommandRunnerTest, Execute) {
  auto storage = MakeSessionStorage("page");
  auto runner = MakeRunner();
  auto story_id = CreateStory(storage.get());
  auto story_storage = GetStoryStorage(storage.get(), story_id.value_or(""));

  auto mod_name = MakeModulePath("mod");
  InitModuleData(story_storage.get(), mod_name);

  fuchsia::modular::RemoveMod remove_mod;
  remove_mod.mod_name = mod_name;
  fuchsia::modular::StoryCommand command;
  command.set_remove_mod(std::move(remove_mod));

  bool done{};
  runner->Execute(story_id, story_storage.get(), std::move(command),
                  [&](fuchsia::modular::ExecuteResult result) {
                    EXPECT_EQ(fuchsia::modular::ExecuteStatus::OK, result.status);
                    done = true;
                  });
  RunLoopUntil([&] { return done; });

  done = false;
  story_storage->ReadModuleData(std::move(mod_name))
      ->Then([&](fuchsia::modular::ModuleDataPtr module_data) {
        EXPECT_TRUE(module_data->module_deleted);
        done = true;
      });
  RunLoopUntil([&] { return done; });
}

TEST_F(RemoveModCommandRunnerTest, ExecuteNoModuleData) {
  auto storage = MakeSessionStorage("page");
  auto runner = MakeRunner();
  auto story_id = CreateStory(storage.get());
  auto story_storage = GetStoryStorage(storage.get(), story_id.value_or(""));

  auto mod_name = MakeModulePath("mod");
  fuchsia::modular::RemoveMod remove_mod;
  remove_mod.mod_name = std::move(mod_name);
  fuchsia::modular::StoryCommand command;
  command.set_remove_mod(std::move(remove_mod));

  bool done{};
  runner->Execute(story_id, story_storage.get(), std::move(command),
                  [&](fuchsia::modular::ExecuteResult result) {
                    EXPECT_EQ(fuchsia::modular::ExecuteStatus::INVALID_MOD, result.status);
                    EXPECT_EQ(result.error_message, "No module data for given name.");
                    done = true;
                  });

  RunLoopUntil([&] { return done; });
}

TEST_F(RemoveModCommandRunnerTest, ExecuteModNameTransitional) {
  auto storage = MakeSessionStorage("page");
  auto runner = MakeRunner();
  auto story_id = CreateStory(storage.get());
  auto story_storage = GetStoryStorage(storage.get(), story_id.value_or(""));

  auto mod_name_transitional = "mod";
  auto mod_name = MakeModulePath(mod_name_transitional);
  InitModuleData(story_storage.get(), mod_name);

  fuchsia::modular::RemoveMod remove_mod;
  remove_mod.mod_name_transitional = mod_name_transitional;
  fuchsia::modular::StoryCommand command;
  command.set_remove_mod(std::move(remove_mod));

  bool done{};
  runner->Execute(story_id, story_storage.get(), std::move(command),
                  [&](fuchsia::modular::ExecuteResult result) {
                    EXPECT_EQ(fuchsia::modular::ExecuteStatus::OK, result.status);
                    done = true;
                  });
  RunLoopUntil([&] { return done; });

  done = false;
  story_storage->ReadModuleData(std::move(mod_name))
      ->Then([&](fuchsia::modular::ModuleDataPtr module_data) {
        EXPECT_TRUE(module_data->module_deleted);
        done = true;
      });
  RunLoopUntil([&] { return done; });
}

}  // namespace
}  // namespace modular
