// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/story_runner/module_context_impl.h"

#include <lib/fidl/cpp/interface_request.h>

#include <string>

#include "src/lib/fxl/strings/join_strings.h"
#include "src/modular/bin/sessionmgr/storage/constants_and_utils.h"
#include "src/modular/bin/sessionmgr/story_runner/story_controller_impl.h"
#include "src/modular/lib/fidl/clone.h"

namespace modular {

ModuleContextImpl::ModuleContextImpl(
    const ModuleContextInfo& info, const fuchsia::modular::ModuleData* const module_data,
    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> service_provider_request)
    : module_data_(module_data),
      story_controller_impl_(info.story_controller_impl),
      component_context_impl_(
          info.component_context_info,
          EncodeModuleComponentNamespace(info.story_controller_impl->GetStoryId().value_or("")),
          EncodeModulePath(module_data_->module_path()), module_data_->module_url()),
      discover_registry_(info.discover_registry) {
  service_provider_impl_.AddService<fuchsia::modular::ComponentContext>(
      [this](fidl::InterfaceRequest<fuchsia::modular::ComponentContext> request) {
        component_context_impl_.Connect(std::move(request));
      });
  service_provider_impl_.AddService<fuchsia::modular::ModuleContext>(
      [this](fidl::InterfaceRequest<fuchsia::modular::ModuleContext> request) {
        bindings_.AddBinding(this, std::move(request));
      });
  service_provider_impl_.AddService<fuchsia::app::discover::ModuleOutputWriter>(
      [this](auto request) {
        fuchsia::app::discover::ModuleIdentifier module_scope;
        module_scope.set_story_id(story_controller_impl_->GetStoryId().value_or(""));
        module_scope.set_module_path(module_data_->module_path());
        discover_registry_->RegisterModuleOutputWriter(std::move(module_scope), std::move(request));
      });
  service_provider_impl_.AddService<fuchsia::app::discover::StoryModule>([this](auto request) {
    fuchsia::app::discover::ModuleIdentifier module_scope;
    module_scope.set_story_id(story_controller_impl_->GetStoryId().value_or(""));
    module_scope.set_module_path(module_data_->module_path());
    discover_registry_->RegisterStoryModule(std::move(module_scope), std::move(request));
  });
  service_provider_impl_.AddBinding(std::move(service_provider_request));
}

ModuleContextImpl::~ModuleContextImpl() {}

void ModuleContextImpl::GetLink(fidl::StringPtr name,
                                fidl::InterfaceRequest<fuchsia::modular::Link> request) {
  fuchsia::modular::LinkPathPtr link_path;
  // See if there's a parameter mapping for this link.
  link_path = story_controller_impl_->GetLinkPathForParameterName(module_data_->module_path(),
                                                                  name.value_or(""));
  story_controller_impl_->ConnectLinkPath(std::move(link_path), std::move(request));
}

void ModuleContextImpl::EmbedModule(
    std::string name, fuchsia::modular::Intent intent,
    fidl::InterfaceRequest<fuchsia::modular::ModuleController> module_controller,
    fuchsia::ui::views::ViewToken view_token, EmbedModuleCallback callback) {
  AddModParams params;
  params.parent_mod_path = module_data_->module_path();
  params.mod_name = name;
  params.intent = std::move(intent);
  params.module_source = fuchsia::modular::ModuleSource::INTERNAL;
  params.surface_relation = nullptr;
  params.is_embedded = true;
  story_controller_impl_->EmbedModule(std::move(params), std::move(module_controller),
                                      std::move(view_token), std::move(callback));
}

void ModuleContextImpl::EmbedModule2(
    std::string name, fuchsia::modular::Intent intent,
    fidl::InterfaceRequest<fuchsia::modular::ModuleController> module_controller,
    fuchsia::ui::views::ViewToken view_token, EmbedModule2Callback callback) {
  EmbedModule(std::move(name), std::move(intent), std::move(module_controller),
              std::move(view_token), std::move(callback));
}

void ModuleContextImpl::AddModuleToStory(
    std::string name, fuchsia::modular::Intent intent,
    fidl::InterfaceRequest<fuchsia::modular::ModuleController> module_controller,
    fuchsia::modular::SurfaceRelationPtr surface_relation, AddModuleToStoryCallback callback) {
  AddModParams params;
  params.parent_mod_path = module_data_->module_path();
  params.mod_name = name;
  params.intent = std::move(intent);
  params.module_source = fuchsia::modular::ModuleSource::INTERNAL;
  params.surface_relation = std::move(surface_relation);
  params.is_embedded = false;
  story_controller_impl_->AddModuleToStory(std::move(params), std::move(module_controller),
                                           std::move(callback));
}

void ModuleContextImpl::RequestFocus() {
  story_controller_impl_->FocusModule(module_data_->module_path());
  story_controller_impl_->RequestStoryFocus();
}

void ModuleContextImpl::RemoveSelfFromStory() {
  story_controller_impl_->RemoveModuleFromStory(module_data_->module_path());
}

void ModuleContextImpl::StartOngoingActivity(
    fuchsia::modular::OngoingActivityType ongoing_activity_type,
    fidl::InterfaceRequest<fuchsia::modular::OngoingActivity> request) {
  story_controller_impl_->StartOngoingActivity(ongoing_activity_type, std::move(request));
}

void ModuleContextImpl::CreateEntity(
    std::string type, fuchsia::mem::Buffer data,
    fidl::InterfaceRequest<fuchsia::modular::Entity> entity_request,
    CreateEntityCallback callback) {
  story_controller_impl_->CreateEntity(type, std::move(data), std::move(entity_request),
                                       std::move(callback));
}

}  // namespace modular
