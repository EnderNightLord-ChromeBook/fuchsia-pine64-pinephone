// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/test/fakes/fake_session.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>

#include <iomanip>
#include <iostream>

#include "lib/fidl/cpp/optional.h"
#include "src/lib/fxl/logging.h"
#include "src/media/playback/mediaplayer/graph/formatting.h"

namespace media_player {
namespace test {
namespace {

constexpr uint64_t kPresentationRatePerSecond = 60;
constexpr zx::duration kPresentationInterval = zx::duration(ZX_SEC(1) / kPresentationRatePerSecond);
constexpr uint32_t kRootNodeId = 1;

}  // namespace

FakeSession::FakeSession()
    : dispatcher_(async_get_default_dispatcher()), binding_(this), weak_factory_(this) {
  fuchsia::ui::gfx::ResourceArgs root_resource;
  fuchsia::ui::gfx::ViewArgs view_args;
  root_resource.set_view(std::move(view_args));
  resources_by_id_.emplace(kRootNodeId, std::move(root_resource));
}

FakeSession::~FakeSession() {}

void FakeSession::Bind(fidl::InterfaceRequest<fuchsia::ui::scenic::Session> request,
                       fuchsia::ui::scenic::SessionListenerPtr listener) {
  binding_.Bind(std::move(request));
  listener_ = std::move(listener);

  PresentScene();
}

void FakeSession::DumpExpectations(uint32_t display_height) {
  if (image_pipe_) {
    image_pipe_->DumpExpectations(display_height);
  } else {
    dump_expectations_ = true;
    expected_display_height_ = display_height;
  }
}

void FakeSession::SetExpectations(uint32_t black_image_id,
                                  const fuchsia::images::ImageInfo& black_image_info,
                                  const fuchsia::images::ImageInfo& info, uint32_t display_height,
                                  const std::vector<PacketInfo>&& expected_packets_info) {
  if (image_pipe_) {
    image_pipe_->SetExpectations(black_image_id, black_image_info, info, display_height,
                                 std::move(expected_packets_info));
  } else {
    expected_black_image_id_ = black_image_id;
    expected_black_image_info_ = fidl::MakeOptional(black_image_info);
    expected_image_info_ = fidl::MakeOptional(info);
    expected_packets_info_ = std::move(expected_packets_info);
    expected_display_height_ = display_height;
  }
}

void FakeSession::Enqueue(std::vector<fuchsia::ui::scenic::Command> cmds) {
  for (auto& command : cmds) {
    switch (command.Which()) {
      case fuchsia::ui::scenic::Command::Tag::kGfx:
        switch (command.gfx().Which()) {
          case fuchsia::ui::gfx::Command::Tag::kSetEventMask:
            HandleSetEventMask(command.gfx().set_event_mask().id,
                               command.gfx().set_event_mask().event_mask);
            break;
          case fuchsia::ui::gfx::Command::Tag::kCreateResource:
            HandleCreateResource(command.gfx().create_resource().id,
                                 std::move(command.gfx().create_resource().resource));
            break;
          case fuchsia::ui::gfx::Command::Tag::kReleaseResource:
            HandleReleaseResource(command.gfx().release_resource().id);
            break;
          case fuchsia::ui::gfx::Command::Tag::kAddChild:
            HandleAddChild(command.gfx().add_child().node_id, command.gfx().add_child().child_id);
            break;
          case fuchsia::ui::gfx::Command::Tag::kAddPart:
            HandleAddPart(command.gfx().add_part().node_id, command.gfx().add_part().part_id);
            break;
          case fuchsia::ui::gfx::Command::Tag::kSetMaterial:
            HandleSetMaterial(command.gfx().set_material().node_id,
                              command.gfx().set_material().material_id);
            break;
          case fuchsia::ui::gfx::Command::Tag::kSetTexture:
            HandleSetTexture(command.gfx().set_texture().material_id,
                             command.gfx().set_texture().texture_id);
            break;
          case fuchsia::ui::gfx::Command::Tag::kSetShape:
            HandleSetShape(command.gfx().set_shape().node_id, command.gfx().set_shape().shape_id);
            break;
          case fuchsia::ui::gfx::Command::Tag::kSetTranslation:
            HandleSetTranslation(command.gfx().set_translation().id,
                                 command.gfx().set_translation().value);
            break;
          case fuchsia::ui::gfx::Command::Tag::kSetScale:
            HandleSetScale(command.gfx().set_scale().id, command.gfx().set_scale().value);
            break;
          case fuchsia::ui::gfx::Command::Tag::kSetClipPlanes:
            HandleSetClipPlanes(command.gfx().set_clip_planes().node_id,
                                std::move(command.gfx().set_clip_planes().clip_planes));
            break;
          default:
            break;
        }
        break;

      case fuchsia::ui::scenic::Command::Tag::kViews:
        FXL_LOG(INFO) << "Enqueue: views (not implemented), tag "
                      << static_cast<fidl_union_tag_t>(command.views().Which());
        break;

      default:
        FXL_LOG(INFO) << "Enqueue: (not implemented), tag "
                      << static_cast<fidl_union_tag_t>(command.Which());
        break;
    }
  }
}

void FakeSession::Present(uint64_t presentation_time, std::vector<zx::event> acquire_fences,
                          std::vector<zx::event> release_fences, PresentCallback callback) {
  // The video renderer doesn't use these fences, so we don't support them in
  // the fake.
  FXL_CHECK(acquire_fences.empty()) << "Present: acquire_fences not supported.";
  FXL_CHECK(release_fences.empty()) << "Present: release_fences not supported.";

  async::PostTask(dispatcher_, [this, callback = std::move(callback), weak_this = GetWeakThis()]() {
    if (!weak_this) {
      return;
    }

    fuchsia::images::PresentationInfo presentation_info;
    presentation_info.presentation_time = next_presentation_time_.get();
    presentation_info.presentation_interval = kPresentationInterval.get();
    callback(presentation_info);
  });
}

FakeSession::Resource* FakeSession::FindResource(uint32_t id) {
  auto iter = resources_by_id_.find(id);
  return (iter == resources_by_id_.end()) ? nullptr : &iter->second;
}

void FakeSession::HandleSetEventMask(uint32_t resource_id, uint32_t event_mask) {
  if (event_mask & fuchsia::ui::gfx::kMetricsEventMask) {
    fuchsia::ui::gfx::Event gfx_event;
    gfx_event.set_metrics({.node_id = resource_id, .metrics = {1.77344f, 1.77344f, 1.0f}});
    SendGfxEvent(std::move(gfx_event));
  }
}

void FakeSession::HandleCreateResource(uint32_t resource_id, fuchsia::ui::gfx::ResourceArgs args) {
  if (args.is_image_pipe()) {
    FXL_CHECK(!image_pipe_) << "fake supports only one image pipe.";
    FXL_CHECK(args.image_pipe().image_pipe_request);
    image_pipe_ = std::make_unique<FakeImagePipe>();
    image_pipe_->Bind(std::move(args.image_pipe().image_pipe_request));
    image_pipe_->OnPresentScene(zx::time(), next_presentation_time_, kPresentationInterval);

    if (dump_expectations_) {
      image_pipe_->DumpExpectations(expected_display_height_);
    }

    if (!expected_packets_info_.empty()) {
      FXL_CHECK(expected_image_info_);
      image_pipe_->SetExpectations(expected_black_image_id_, *expected_black_image_info_,
                                   *expected_image_info_, expected_display_height_,
                                   std::move(expected_packets_info_));
    }
  } else if (args.is_view()) {
    fuchsia::ui::gfx::ViewProperties view_properties;
    view_properties.bounding_box.min = {0.0f, 0.0f, -1000.0f};
    view_properties.bounding_box.max = {1353.3f, 902.203f, 0.0f};
    fuchsia::ui::gfx::Event gfx_event;
    gfx_event.set_view_properties_changed({resource_id, std::move(view_properties)});
    SendGfxEvent(std::move(gfx_event));
  }

  resources_by_id_.emplace(resource_id, std::move(args));
}

void FakeSession::HandleReleaseResource(uint32_t resource_id) {
  if (resources_by_id_.erase(resource_id) != 1) {
    FXL_LOG(ERROR) << "Asked to release unrecognized resource " << resource_id
                   << ", closing connection.";
    expected_ = false;
    binding_.Unbind();
  }
}

void FakeSession::HandleAddChild(uint32_t parent_id, uint32_t child_id) {
  Resource* parent = FindResource(parent_id);
  Resource* child = FindResource(child_id);

  if (!parent) {
    FXL_LOG(ERROR) << "Asked to add child, parent_id (" << parent_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!parent->can_have_children()) {
    FXL_LOG(ERROR) << "Asked to add child, parent_id (" << parent_id
                   << ") can't have children, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!child) {
    FXL_LOG(ERROR) << "Asked to add child, child_id (" << child_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!child->can_have_parent()) {
    FXL_LOG(ERROR) << "Asked to add child, child_id (" << child_id
                   << ") can't have parent, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (child->parent_ != kNullResourceId) {
    Resource* prev_parent = FindResource(child->parent_);
    if (prev_parent != nullptr) {
      prev_parent->children_.erase(child_id);
      prev_parent->parts_.erase(child_id);
    }
  }

  parent->children_.insert(child_id);
  child->parent_ = parent_id;
}

void FakeSession::HandleAddPart(uint32_t node_id, uint32_t part_id) {
  Resource* node = FindResource(node_id);
  Resource* part = FindResource(part_id);

  if (!node) {
    FXL_LOG(ERROR) << "Asked to add part, node_id (" << node_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!node->can_have_children()) {
    FXL_LOG(ERROR) << "Asked to add part, node_id (" << node_id
                   << ") can't have children, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!part) {
    FXL_LOG(ERROR) << "Asked to add part, part_id (" << part_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!part->can_be_part()) {
    FXL_LOG(ERROR) << "Asked to add part, part_id (" << part_id
                   << ") can't be a part, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (part->parent_ != kNullResourceId) {
    Resource* prev_parent = FindResource(part->parent_);
    if (prev_parent != nullptr) {
      prev_parent->children_.erase(part_id);
      prev_parent->parts_.erase(part_id);
    }
  }

  node->parts_.insert(part_id);
}

void FakeSession::HandleSetMaterial(uint32_t node_id, uint32_t material_id) {
  Resource* node = FindResource(node_id);
  Resource* material = FindResource(material_id);

  if (!node) {
    FXL_LOG(ERROR) << "Asked to set material, node_id (" << node_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!node->can_have_material()) {
    FXL_LOG(ERROR) << "Asked to set material, node_id (" << node_id
                   << ") can't have material, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!material) {
    FXL_LOG(ERROR) << "Asked to set material, material_id (" << material_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!material->is_material()) {
    FXL_LOG(ERROR) << "Asked to set material, material_id (" << material_id
                   << ") is not a material, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  // TODO(dalesat): Copy material info from |material| to |node|.
}

void FakeSession::HandleSetTexture(uint32_t material_id, uint32_t texture_id) {
  Resource* material = FindResource(material_id);
  Resource* texture = FindResource(texture_id);

  if (!material) {
    FXL_LOG(ERROR) << "Asked to set texture, material_id (" << material_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!material->is_material()) {
    FXL_LOG(ERROR) << "Asked to set texture, material_id (" << material_id
                   << ") is not a material, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!texture) {
    FXL_LOG(ERROR) << "Asked to set texture, texture_id (" << texture_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!texture->is_texture()) {
    FXL_LOG(ERROR) << "Asked to set texture, texture_id (" << texture_id
                   << ") is not a texture, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  // TODO(dalesat): Copy texture info from |texture| to |material|.
}

void FakeSession::HandleSetShape(uint32_t node_id, uint32_t shape_id) {
  Resource* node = FindResource(node_id);
  Resource* shape = FindResource(shape_id);

  if (!node) {
    FXL_LOG(ERROR) << "Asked to set shape, node_id (" << node_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!node->can_have_shape()) {
    FXL_LOG(ERROR) << "Asked to set shape, node_id (" << node_id
                   << ") can't have a shape, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!shape) {
    FXL_LOG(ERROR) << "Asked to set shape, shape_id (" << shape_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!shape->is_shape()) {
    FXL_LOG(ERROR) << "Asked to set shape, shape_id (" << shape_id
                   << ") is not a shape, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  node->shape_args_ = fidl::MakeOptional(fidl::Clone(shape->args_));
}

void FakeSession::HandleSetTranslation(uint32_t node_id,
                                       const fuchsia::ui::gfx::Vector3Value& value) {
  Resource* node = FindResource(node_id);

  if (!node) {
    FXL_LOG(ERROR) << "Asked to set translation, node_id (" << node_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!node->can_have_transform()) {
    FXL_LOG(ERROR) << "Asked to set translation, node_id (" << node_id
                   << ") can't have a transform, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  node->translation_ = fidl::MakeOptional(value);
}

void FakeSession::HandleSetScale(uint32_t node_id, const fuchsia::ui::gfx::Vector3Value& value) {
  Resource* node = FindResource(node_id);

  if (!node) {
    FXL_LOG(ERROR) << "Asked to set scale, node_id (" << node_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!node->can_have_transform()) {
    FXL_LOG(ERROR) << "Asked to set scale, node_id (" << node_id
                   << ") can't have a transform, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  node->scale_ = fidl::MakeOptional(value);
}

void FakeSession::HandleSetClipPlanes(uint32_t node_id,
                                      std::vector<fuchsia::ui::gfx::Plane3> value) {
  Resource* node = FindResource(node_id);

  if (!node) {
    FXL_LOG(ERROR) << "Asked to set clip planes, node_id (" << node_id
                   << ") not recognized, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  if (!node->can_have_clip_planes()) {
    FXL_LOG(ERROR) << "Asked to set clip planes, node_id (" << node_id
                   << ") can't have clip planes, closing connection.";
    expected_ = false;
    binding_.Unbind();
    return;
  }

  std::swap(node->clip_planes_, value);
}

void FakeSession::PresentScene() {
  auto now = zx::time(zx_clock_get_monotonic());

  next_presentation_time_ = now + kPresentationInterval;

  if (image_pipe_) {
    image_pipe_->OnPresentScene(now, next_presentation_time_, kPresentationInterval);
  }

  async::PostTaskForTime(
      dispatcher_,
      [this, weak_this = GetWeakThis()]() {
        if (!weak_this) {
          return;
        }

        PresentScene();
      },
      next_presentation_time_);
}

void FakeSession::SendGfxEvent(fuchsia::ui::gfx::Event gfx_event) {
  fuchsia::ui::scenic::Event event;
  event.set_gfx(std::move(gfx_event));
  std::vector<fuchsia::ui::scenic::Event> events;
  events.push_back(std::move(event));
  listener_->OnScenicEvent(std::move(events));
}

void FakeSession::DetectZFighting() {
  std::vector<ShapeNode> shape_nodes;
  FindShapeNodes(kRootNodeId, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, &shape_nodes);

  if (shape_nodes.size() == 0) {
    return;
  }

  for (size_t i = 0; i < shape_nodes.size() - 1; ++i) {
    for (size_t j = i + 1; j < shape_nodes.size(); ++j) {
      if (shape_nodes[i].Intersects(shape_nodes[j])) {
        FXL_LOG(ERROR) << "Node " << shape_nodes[i].id_ << " z-fights with node "
                       << shape_nodes[j].id_ << ".";
        expected_ = false;
      }
    }
  }
}

void FakeSession::FindShapeNodes(uint32_t node_id, fuchsia::ui::gfx::vec3 translation,
                                 fuchsia::ui::gfx::vec3 scale,
                                 std::vector<ShapeNode>* shape_nodes) {
  FXL_CHECK(shape_nodes);
  Resource* node = FindResource(node_id);
  FXL_CHECK(node);

  if (node->translation_) {
    FXL_CHECK(node->translation_->variable_id == 0) << "Variables not supported.";
    translation.x += node->translation_->value.x * scale.x;
    translation.y += node->translation_->value.y * scale.y;
    translation.z += node->translation_->value.z * scale.z;
  }

  if (node->scale_) {
    FXL_CHECK(node->scale_->variable_id == 0) << "Variables not supported.";
    scale.x *= node->scale_->value.x;
    scale.y *= node->scale_->value.y;
    scale.z *= node->scale_->value.z;
  }

  if (node->shape_args_) {
    FXL_CHECK(node->shape_args_->is_rectangle()) << "Only rectangle shapes are supported";
    fuchsia::ui::gfx::vec3 extent;
    FXL_CHECK(node->shape_args_->rectangle().width.is_vector1())
        << "Only vector1 values are supported.";
    FXL_CHECK(node->shape_args_->rectangle().height.is_vector1())
        << "Only vector1 values are supported.";
    extent.x = scale.x * node->shape_args_->rectangle().width.vector1();
    extent.y = scale.y * node->shape_args_->rectangle().height.vector1();
    extent.z = scale.z * 0;
    shape_nodes->emplace_back(
        node_id,
        fuchsia::ui::gfx::vec3{translation.x - extent.x / 2.0f, translation.y - extent.y / 2.0f,
                               translation.z - extent.z / 2.0f},
        extent);
  }

  for (uint32_t child_id : node->children_) {
    FindShapeNodes(child_id, translation, scale, shape_nodes);
  }

  for (uint32_t part_id : node->parts_) {
    FindShapeNodes(part_id, translation, scale, shape_nodes);
  }
}

}  // namespace test
}  // namespace media_player
