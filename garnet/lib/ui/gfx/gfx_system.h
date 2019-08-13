// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_GFX_SYSTEM_H_
#define GARNET_LIB_UI_GFX_GFX_SYSTEM_H_

#include <memory>

#include "garnet/lib/ui/gfx/displays/display_manager.h"
#include "garnet/lib/ui/gfx/engine/engine.h"
#include "garnet/lib/ui/gfx/engine/gfx_command_applier.h"
#include "garnet/lib/ui/gfx/resources/compositor/compositor.h"
#include "garnet/lib/ui/scenic/scenic.h"
#include "garnet/lib/ui/scenic/system.h"
#include "src/ui/lib/escher/escher.h"

namespace scenic_impl {
namespace gfx {

class Compositor;
class GfxSystem;
using GfxSystemWeakPtr = fxl::WeakPtr<GfxSystem>;

class GfxSystem : public System, public TempScenicDelegate, public SessionUpdater {
 public:
  static constexpr TypeId kTypeId = kGfx;
  static const char* kName;

  GfxSystem(SystemContext context, Display* display, Engine* engine, escher::EscherWeakPtr escher);

  GfxSystemWeakPtr GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  CommandDispatcherUniquePtr CreateCommandDispatcher(CommandDispatcherContext context) override;

  // TODO(SCN-452): Remove this when we externalize Displays.
  void GetDisplayInfo(fuchsia::ui::scenic::Scenic::GetDisplayInfoCallback callback) override;
  void TakeScreenshot(fuchsia::ui::scenic::Scenic::TakeScreenshotCallback callback) override;
  void GetDisplayOwnershipEvent(
      fuchsia::ui::scenic::Scenic::GetDisplayOwnershipEventCallback callback) override;

  // |SessionUpdater|
  virtual UpdateResults UpdateSessions(std::unordered_set<SessionId> sessions_to_update,
                                       zx::time presentation_time, uint64_t trace_id) override;

  // |SessionUpdater|
  virtual void PrepareFrame(zx::time presentation_time, uint64_t trace_id) override;

  // For tests.
  SessionManager* session_manager() { return &session_manager_; }

  static escher::EscherUniquePtr CreateEscher(sys::ComponentContext* app_context);

 private:
  static VkBool32 HandleDebugReport(VkDebugReportFlagsEXT flags,
                                    VkDebugReportObjectTypeEXT objectType, uint64_t object,
                                    size_t location, int32_t messageCode, const char* pLayerPrefix,
                                    const char* pMessage, void* pUserData);

  void DumpSessionMapResources(std::ostream& output,
                               std::unordered_set<GlobalId, GlobalId::Hash>* visited_resources);

  escher::EscherWeakPtr escher_;
  Display* const display_;
  Engine* const engine_;
  SessionManager session_manager_;

  std::optional<CommandContext> command_context_;

  // Tracks the number of sessions returning ApplyUpdateResult::needs_render
  // and uses it for tracing.
  uint64_t needs_render_count_ = 0;
  uint64_t processed_needs_render_count_ = 0;

  fxl::WeakPtrFactory<GfxSystem> weak_factory_;  // must be last
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_GFX_SYSTEM_H_
