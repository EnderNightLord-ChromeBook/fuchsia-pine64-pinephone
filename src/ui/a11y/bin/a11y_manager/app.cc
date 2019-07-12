// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/bin/a11y_manager/app.h"

namespace a11y_manager {

App::App()
    : startup_context_(sys::ComponentContext::Create()),
      settings_manager_impl_(std::make_unique<SettingsManagerImpl>()),
      semantics_manager_impl_(std::make_unique<SemanticsManagerImpl>()) {

  startup_context_->outgoing()->AddPublicService<fuchsia::accessibility::SettingsManager>(
      [this](fidl::InterfaceRequest<fuchsia::accessibility::SettingsManager> request) {
        settings_manager_impl_->AddBinding(std::move(request));
      });

  semantics_manager_impl_->SetDebugDirectory(startup_context_->outgoing()->debug_dir());
  startup_context_->outgoing()
      ->AddPublicService<fuchsia::accessibility::semantics::SemanticsManager>(
          [this](
              fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticsManager> request) {
            semantics_manager_impl_->AddBinding(std::move(request));
          });
}

}  // namespace a11y_manager
