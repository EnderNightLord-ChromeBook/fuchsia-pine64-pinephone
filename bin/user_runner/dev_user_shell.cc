// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of a user shell for module development. It takes a
// root module URL and data for its Link as command line arguments,
// which can be set using the device_runner --user-shell-args flag.

#include <utility>

#include <memory>

#include <component/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <views_v1/cpp/fidl.h>
#include <views_v1_token/cpp/fidl.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app/cpp/connect.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/binding_set.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/fidl/single_service_app.h"
#include "peridot/lib/fidl/view_host.h"

namespace {

class Settings {
 public:
  explicit Settings(const fxl::CommandLine& command_line) {
    root_module =
        command_line.GetOptionValueWithDefault("root_module", "example_recipe");
    root_link = command_line.GetOptionValueWithDefault("root_link", "");
    story_id = command_line.GetOptionValueWithDefault("story_id", "");
  }

  std::string root_module;
  std::string root_link;
  std::string story_id;
};

class DevUserShellApp
    : fuchsia::modular::StoryWatcher,
      fuchsia::modular::InterruptionListener,
      fuchsia::modular::NextListener,
      public fuchsia::modular::SingleServiceApp<fuchsia::modular::UserShell> {
 public:
  explicit DevUserShellApp(
      component::ApplicationContext* const application_context,
      Settings settings)
      : SingleServiceApp(application_context),
        settings_(std::move(settings)),
        story_watcher_binding_(this) {}

  ~DevUserShellApp() override = default;

 private:
  // |SingleServiceApp|
  void CreateView(
      fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request,
      fidl::InterfaceRequest<component::ServiceProvider> /*services*/)
      override {
    view_owner_request_ = std::move(view_owner_request);
    Connect();
  }

  // |UserShell|
  void Initialize(fidl::InterfaceHandle<fuchsia::modular::UserShellContext>
                      user_shell_context) override {
    user_shell_context_.Bind(std::move(user_shell_context));
    user_shell_context_->GetStoryProvider(story_provider_.NewRequest());
    user_shell_context_->GetSuggestionProvider(
        suggestion_provider_.NewRequest());
    user_shell_context_->GetFocusController(focus_controller_.NewRequest());
    user_shell_context_->GetVisibleStoriesController(
        visible_stories_controller_.NewRequest());

    suggestion_provider_->SubscribeToInterruptions(
        interruption_listener_bindings_.AddBinding(this));
    suggestion_provider_->SubscribeToNext(
        next_listener_bindings_.AddBinding(this), 3);

    Connect();
  }

  void Connect() {
    if (!view_owner_request_ || !story_provider_) {
      // Not yet ready, wait for the other of CreateView() and
      // Initialize() to be called.
      return;
    }

    FXL_LOG(INFO) << "DevUserShell START " << settings_.root_module << " "
                  << settings_.root_link;

    view_ = std::make_unique<fuchsia::modular::ViewHost>(
        application_context()
            ->ConnectToEnvironmentService<views_v1::ViewManager>(),
        std::move(view_owner_request_));

    if (settings_.story_id.empty()) {
      story_provider_->CreateStory(settings_.root_module,
                                   [this](const fidl::StringPtr& story_id) {
                                     StartStoryById(story_id);
                                   });
    } else {
      StartStoryById(settings_.story_id);
    }
  }

  void StartStoryById(const fidl::StringPtr& story_id) {
    story_provider_->GetController(story_id, story_controller_.NewRequest());
    story_controller_.set_error_handler([this, story_id] {
      FXL_LOG(ERROR) << "Story controller for story " << story_id
                     << " died. Does this story exist?";
    });

    story_controller_->Watch(story_watcher_binding_.NewBinding());

    FXL_LOG(INFO) << "DevUserShell Starting story with id: " << story_id;
    fidl::InterfaceHandle<views_v1_token::ViewOwner> root_module_view;
    story_controller_->Start(root_module_view.NewRequest());
    view_->ConnectView(std::move(root_module_view));
    focus_controller_->Set(story_id);
    auto visible_stories = fidl::VectorPtr<fidl::StringPtr>::New(0);
    visible_stories.push_back(story_id);
    visible_stories_controller_->Set(std::move(visible_stories));

    if (!settings_.root_link.empty()) {
      fuchsia::modular::LinkPtr root;
      story_controller_->GetLink(nullptr, "root", root.NewRequest());
      root->UpdateObject(nullptr, settings_.root_link);
    }
  }

  // |StoryWatcher|
  void OnStateChange(fuchsia::modular::StoryState state) override {
    FXL_LOG(INFO) << "DevUserShell State " << state;
  }

  // |StoryWatcher|
  void OnModuleAdded(fuchsia::modular::ModuleData /*module_data*/) override {}

  // |NextListener|
  void OnNextResults(
      fidl::VectorPtr<fuchsia::modular::Suggestion> suggestions) override {
    FXL_VLOG(4) << "DevUserShell/NextListener::OnNextResults()";
    for (auto& suggestion : *suggestions) {
      FXL_LOG(INFO) << "  " << suggestion.uuid << " "
                    << suggestion.display.headline;
    }
  }

  // |InterruptionListener|
  void OnInterrupt(fuchsia::modular::Suggestion suggestion) override {
    FXL_VLOG(4) << "DevUserShell/InterruptionListener::OnInterrupt() "
                << suggestion.uuid;
  }

  // |NextListener|
  void OnProcessingChange(bool processing) override {
    FXL_VLOG(4) << "DevUserShell/NextListener::OnProcessingChange("
                << processing << ")";
  }

  const Settings settings_;

  fidl::InterfaceRequest<views_v1_token::ViewOwner> view_owner_request_;
  std::unique_ptr<fuchsia::modular::ViewHost> view_;

  fuchsia::modular::UserShellContextPtr user_shell_context_;
  fuchsia::modular::StoryProviderPtr story_provider_;
  fuchsia::modular::StoryControllerPtr story_controller_;
  fuchsia::modular::FocusControllerPtr focus_controller_;
  fuchsia::modular::VisibleStoriesControllerPtr visible_stories_controller_;

  fidl::Binding<fuchsia::modular::StoryWatcher> story_watcher_binding_;

  fuchsia::modular::SuggestionProviderPtr suggestion_provider_;
  fidl::BindingSet<fuchsia::modular::InterruptionListener>
      interruption_listener_bindings_;
  fidl::BindingSet<fuchsia::modular::NextListener> next_listener_bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DevUserShellApp);
};

}  // namespace

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  Settings settings(command_line);

  fsl::MessageLoop loop;

  auto app_context = component::ApplicationContext::CreateFromStartupInfo();
  fuchsia::modular::AppDriver<DevUserShellApp> driver(
      app_context->outgoing().deprecated_services(),
      std::make_unique<DevUserShellApp>(app_context.get(), std::move(settings)),
      [&loop] { loop.QuitNow(); });

  loop.Run();
  return 0;
}
