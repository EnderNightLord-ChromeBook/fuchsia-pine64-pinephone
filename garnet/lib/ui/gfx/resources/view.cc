// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/resources/view.h"

#include "garnet/lib/ui/gfx/engine/engine.h"
#include "garnet/lib/ui/gfx/engine/object_linker.h"
#include "garnet/lib/ui/gfx/engine/session.h"
#include "garnet/lib/ui/gfx/resources/nodes/node.h"
#include "garnet/lib/ui/gfx/util/validate_eventpair.h"
#include "src/lib/fxl/logging.h"

namespace scenic_impl {
namespace gfx {

const ResourceTypeInfo View::kTypeInfo = {ResourceType::kView, "View"};

View::View(Session* session, ResourceId id, ViewLinker::ImportLink link,
           fuchsia::ui::views::ViewRefControl control_ref, fuchsia::ui::views::ViewRef view_ref,
           std::shared_ptr<ErrorReporter> error_reporter, EventReporter* event_reporter)
    : Resource(session, id, View::kTypeInfo),
      link_(std::move(link)),
      control_ref_(std::move(control_ref)),
      view_ref_(std::move(view_ref)),
      error_reporter_(std::move(error_reporter)),
      event_reporter_(event_reporter),
      weak_factory_(this) {
  FXL_DCHECK(error_reporter_);
  FXL_DCHECK(event_reporter_);

  node_ = fxl::AdoptRef<ViewNode>(new ViewNode(session, weak_factory_.GetWeakPtr()));

  FXL_DCHECK(link_.valid());
  FXL_DCHECK(!link_.initialized());
  FXL_DCHECK(validate_viewref(control_ref_, view_ref_));
}

View::~View() {
  // Explicitly detach the phantom node to ensure it is cleaned up.
  node_->Detach(error_reporter_.get());
}

void View::Connect() {
  link_.Initialize(this, fit::bind_member(this, &View::LinkResolved),
                   fit::bind_member(this, &View::LinkDisconnected));
}

void View::SignalRender() {
  if (!render_handle_) {
    return;
  }

  // Verify the render_handle_ is still valid before attempting to signal it.
  if (zx_object_get_info(render_handle_, ZX_INFO_HANDLE_VALID, /*buffer=*/NULL,
                         /*buffer_size=*/0, /*actual=*/NULL,
                         /*avail=*/NULL) == ZX_OK) {
    zx_status_t status = zx_object_signal(render_handle_, /*clear_mask=*/0u, ZX_EVENT_SIGNALED);
    ZX_ASSERT(status == ZX_OK);
  }
}

void View::LinkResolved(ViewHolder* view_holder) {
  FXL_DCHECK(!view_holder_);
  view_holder_ = view_holder;

  // Attaching our node to the holder should never fail.
  FXL_CHECK(view_holder_->AddChild(node_, ErrorReporter::Default().get()))
      << "View::LinkResolved(): error while adding ViewNode as child of ViewHolder";

  SendViewHolderConnectedEvent();
}

void View::LinkDisconnected() {
  // The connection ViewHolder no longer exists, detach the phantom node from
  // the ViewHolder.
  node_->Detach(error_reporter_.get());

  view_holder_ = nullptr;
  // ViewHolder was disconnected. There are no guarantees on liveness of the
  // render event, so invalidate the handle.
  InvalidateRenderEventHandle();

  SendViewHolderDisconnectedEvent();
}

void View::SendViewHolderConnectedEvent() {
  fuchsia::ui::gfx::Event event;
  event.set_view_holder_connected({.view_id = id()});
  event_reporter_->EnqueueEvent(std::move(event));
}

void View::SendViewHolderDisconnectedEvent() {
  fuchsia::ui::gfx::Event event;
  event.set_view_holder_disconnected({.view_id = id()});
  event_reporter_->EnqueueEvent(std::move(event));
}

}  // namespace gfx
}  // namespace scenic_impl
