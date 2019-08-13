// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/engine/engine_renderer_visitor.h"

#include "garnet/lib/ui/gfx/resources/camera.h"
#include "garnet/lib/ui/gfx/resources/import.h"
#include "garnet/lib/ui/gfx/resources/material.h"
#include "garnet/lib/ui/gfx/resources/nodes/entity_node.h"
#include "garnet/lib/ui/gfx/resources/nodes/node.h"
#include "garnet/lib/ui/gfx/resources/nodes/opacity_node.h"
#include "garnet/lib/ui/gfx/resources/nodes/scene.h"
#include "garnet/lib/ui/gfx/resources/nodes/shape_node.h"
#include "garnet/lib/ui/gfx/resources/nodes/traversal.h"
#include "garnet/lib/ui/gfx/resources/nodes/view_node.h"
#include "garnet/lib/ui/gfx/resources/shapes/circle_shape.h"
#include "garnet/lib/ui/gfx/resources/shapes/mesh_shape.h"
#include "garnet/lib/ui/gfx/resources/shapes/rectangle_shape.h"
#include "garnet/lib/ui/gfx/resources/shapes/rounded_rectangle_shape.h"
#include "garnet/lib/ui/gfx/resources/shapes/shape.h"
#include "garnet/lib/ui/gfx/resources/view.h"
#include "garnet/lib/ui/gfx/resources/view_holder.h"
#include "src/lib/fxl/logging.h"
#include "src/ui/lib/escher/paper/paper_renderer.h"

namespace scenic_impl {
namespace gfx {

EngineRendererVisitor::EngineRendererVisitor(escher::PaperRenderer* renderer,
                                             escher::BatchGpuUploader* gpu_uploader)
    : renderer_(renderer), gpu_uploader_(gpu_uploader) {}

void EngineRendererVisitor::Visit(Memory* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(Image* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(ImagePipe* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(Buffer* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(View* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(ViewNode* r) {
  const size_t previous_count = draw_call_count_;
  const bool previous_should_render_debug_bounds = should_render_debug_bounds_;
  if (auto view = r->GetView()) {
    should_render_debug_bounds_ = view->should_render_bounding_box();
  }
  VisitNode(r);

  bool view_is_rendering_element = draw_call_count_ > previous_count;
  if (r->GetView() && view_is_rendering_element) {
    // TODO(SCN-1099) Add a test to ensure this signal isn't triggered when this
    // view is not rendering.
    r->GetView()->SignalRender();
  }

  should_render_debug_bounds_ = previous_should_render_debug_bounds;
}

void EngineRendererVisitor::Visit(ViewHolder* r) {
  escher::PaperTransformStack* transform_stack = renderer_->transform_stack();
  transform_stack->PushTransform(static_cast<escher::mat4>(r->transform()));
  transform_stack->AddClipPlanes(r->clip_planes());

  // A view holder should render its bounds if either its embedding view has
  // debug rendering turned on (which will mean should_render_debug_bounds_=true)
  // or if its own view specifies that debug bounds should be rendered.
  if (should_render_debug_bounds_ || (r->view() && r->view()->should_render_bounding_box())) {
    auto bbox = r->GetLocalBoundingBox();
    // Create material and submit draw call.
    escher::PaperMaterialPtr escher_material = escher::Material::New(r->bounds_color());
    escher_material->set_type(escher::Material::Type::kWireframe);
    renderer_->DrawBoundingBox(bbox, escher_material, escher::PaperDrawableFlags());
    ++draw_call_count_;
  }

  ForEachDirectDescendantFrontToBack(*r, [this](Node* node) { node->Accept(this); });
  transform_stack->Pop();
}

void EngineRendererVisitor::Visit(EntityNode* r) { VisitNode(r); }

void EngineRendererVisitor::Visit(OpacityNode* r) {
  if (r->opacity() == 0) {
    return;
  }

  float old_opacity = opacity_;
  opacity_ *= r->opacity();

  VisitNode(r);

  opacity_ = old_opacity;
}

void EngineRendererVisitor::VisitNode(Node* r) {
  escher::PaperTransformStack* transform_stack = renderer_->transform_stack();
  transform_stack->PushTransform(static_cast<escher::mat4>(r->transform()));
  transform_stack->AddClipPlanes(r->clip_planes());

  ForEachDirectDescendantFrontToBack(*r, [this](Node* node) { node->Accept(this); });

  transform_stack->Pop();
}

void EngineRendererVisitor::Visit(Scene* r) { VisitNode(r); }

void EngineRendererVisitor::Visit(Compositor* r) { FXL_DCHECK(false); }

void EngineRendererVisitor::Visit(DisplayCompositor* r) { FXL_DCHECK(false); }

void EngineRendererVisitor::Visit(LayerStack* r) { FXL_DCHECK(false); }

void EngineRendererVisitor::Visit(Layer* r) { FXL_DCHECK(false); }

void EngineRendererVisitor::Visit(ShapeNode* r) {
  // We don't need to call |VisitNode| because shape nodes don't have
  // children or parts.
  FXL_DCHECK(r->children().empty() && r->parts().empty());

  auto& shape = r->shape();
  auto& material = r->material();
  if (!material || !shape)
    return;

  material->Accept(this);

  escher::MaterialPtr escher_material = material->escher_material();
  FXL_DCHECK(escher_material);

  if (opacity_ < 1.f) {
    // When we want to support other material types (e.g. metallic shaders),
    // we'll need to change this. If we want to support semitransparent
    // textures and materials, we'll need more pervasive changes.
    glm::vec4 color = escher_material->color();
    color.a *= opacity_;
    escher_material = escher::Material::New(color, escher_material->texture());
    escher_material->set_type(escher::Material::Type::kTranslucent);
  }

  escher::PaperTransformStack* transform_stack = renderer_->transform_stack();
  transform_stack->PushTransform(static_cast<escher::mat4>(r->transform()));

  escher::PaperDrawableFlags flags{};
  if (shape->IsKindOf<RoundedRectangleShape>()) {
    auto rect = static_cast<RoundedRectangleShape*>(shape.get());
    renderer_->DrawRoundedRect(rect->spec(), escher_material, flags);
  } else if (shape->IsKindOf<RectangleShape>()) {
    auto rect = static_cast<RectangleShape*>(shape.get());
    renderer_->DrawRect(rect->width(), rect->height(), escher_material, flags);
  } else if (shape->IsKindOf<CircleShape>()) {
    auto circle = static_cast<CircleShape*>(shape.get());

    // Only draw the circle if its radius is greater than epsilon.
    if (circle->radius() > escher::kEpsilon) {
      renderer_->DrawCircle(circle->radius(), escher_material, flags);
    }
  } else if (shape->IsKindOf<MeshShape>()) {
    auto mesh_shape = static_cast<MeshShape*>(shape.get());
    renderer_->DrawMesh(mesh_shape->escher_mesh(), escher_material, flags);
  } else {
    FXL_LOG(ERROR) << "Unsupported shape type encountered.";
  }
  transform_stack->Pop();

  ++draw_call_count_;
}

void EngineRendererVisitor::Visit(CircleShape* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(RectangleShape* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(RoundedRectangleShape* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(MeshShape* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(Material* r) { r->UpdateEscherMaterial(gpu_uploader_); }

void EngineRendererVisitor::Visit(Import* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(Camera* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(Renderer* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(Light* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(AmbientLight* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(DirectionalLight* r) { FXL_CHECK(false); }

void EngineRendererVisitor::Visit(PointLight* r) { FXL_CHECK(false); }

}  // namespace gfx
}  // namespace scenic_impl
