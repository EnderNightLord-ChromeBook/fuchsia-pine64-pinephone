// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/testing/views/background_view.h"

#include <lib/ui/gfx/cpp/math.h>
#include <src/lib/fxl/logging.h>
#include <zircon/status.h>

namespace scenic {

BackgroundView::BackgroundView(ViewContext context, const std::string& debug_name)
    : binding_(this, std::move(context.session_and_listener_request.second)),
      session_(std::move(context.session_and_listener_request.first)),
      view_(&session_, std::move(context.view_token), debug_name),
      background_node_(&session_) {
  binding_.set_error_handler([](zx_status_t status) {
    FXL_LOG(FATAL) << "Session listener binding: " << zx_status_get_string(status);
  });

  session_.Present(0, [](auto) {});

  Material background_material(&session_);
  background_material.SetColor(kBackgroundColor.r, kBackgroundColor.g, kBackgroundColor.b,
                               kBackgroundColor.a);
  background_node_.SetMaterial(background_material);
  view_.AddChild(background_node_);
}

void BackgroundView::SetHostImage(zx::vmo vmo, uint64_t size, fuchsia::images::ImageInfo info) {
  Memory memory(&session_, std::move(vmo), size, fuchsia::images::MemoryType::HOST_MEMORY);
  Image image(&session_, memory.id(), 0, info);
  Material background_material(&session_);
  background_material.SetTexture(image);
  background_node_.SetMaterial(background_material);
}

void BackgroundView::set_present_callback(Session::PresentCallback present_callback) {
  present_callback_ = std::move(present_callback);
}

void BackgroundView::Draw(float cx, float cy, float sx, float sy) {
  Rectangle background_shape(&session_, sx, sy);
  background_node_.SetShape(background_shape);
  background_node_.SetTranslation((float[]){cx, cy, -kBackgroundElevation});
}

void BackgroundView::Present() {
  session_.Present(
      0, present_callback_ ? std::move(present_callback_) : [](auto) {});
}

void BackgroundView::OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events) {
  FXL_LOG(INFO) << "OnScenicEvent";
  for (const auto& event : events) {
    if (event.Which() == fuchsia::ui::scenic::Event::Tag::kGfx &&
        event.gfx().Which() == fuchsia::ui::gfx::Event::Tag::kViewPropertiesChanged) {
      const auto& evt = event.gfx().view_properties_changed();
      fuchsia::ui::gfx::BoundingBox layout_box = ViewPropertiesLayoutBox(evt.properties);

      const auto sz = Max(layout_box.max - layout_box.min, 0.f);
      OnViewPropertiesChanged(sz);
    }
  }
}

void BackgroundView::OnScenicError(std::string error) {
  FXL_LOG(FATAL) << "OnScenicError: " << error;
}

void BackgroundView::OnViewPropertiesChanged(const fuchsia::ui::gfx::vec3& sz) {
  FXL_LOG(INFO) << "Metrics: " << sz.x << "x" << sz.y << "x" << sz.z;
  if (!sz.x || !sz.y || !sz.z)
    return;

  Draw(sz.x * .5f, sz.y * .5f, sz.x, sz.y);
  Present();
}

}  // namespace scenic
