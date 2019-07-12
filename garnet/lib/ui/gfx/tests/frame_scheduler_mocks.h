// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_TESTS_FRAME_SCHEDULER_MOCKS_H_
#define GARNET_LIB_UI_GFX_TESTS_FRAME_SCHEDULER_MOCKS_H_

#include <set>
#include <unordered_map>

#include "garnet/lib/ui/gfx/displays/display.h"
#include "garnet/lib/ui/gfx/engine/frame_scheduler.h"
#include "garnet/lib/ui/gfx/engine/frame_timings.h"

namespace scenic_impl {
namespace gfx {
namespace test {

class MockFrameScheduler : public FrameScheduler {
 public:
  MockFrameScheduler() = default;

  void SetFrameRenderer(fxl::WeakPtr<FrameRenderer> frame_renderer) override {}
  void AddSessionUpdater(fxl::WeakPtr<SessionUpdater> session_updater) override {}

  void SetRenderContinuously(bool render_continuously) override {}
  void ScheduleUpdateForSession(zx_time_t presentation_time,
                                scenic_impl::SessionId session) override {}

  void OnFramePresented(const FrameTimings& timings) override { ++frame_presented_call_count_; }
  void OnFrameRendered(const FrameTimings& timings) override { ++frame_rendered_call_count_; }

  uint32_t frame_presented_call_count() { return frame_presented_call_count_; }
  uint32_t frame_rendered_call_count() { return frame_rendered_call_count_; }

 private:
  uint32_t frame_presented_call_count_ = 0;
  uint32_t frame_rendered_call_count_ = 0;
};

class FakeDisplay : public Display {
 public:
  FakeDisplay()
      : Display(/* id */ 0,
                /* width_in_px */ 0,
                /* height_in_px */ 0) {}

  // Manually sets the values returned by
  // GetVsyncInterval() and GetLastVsyncTime().
  void SetVsyncInterval(zx_duration_t new_interval) { vsync_interval_ = new_interval; }
  void SetLastVsyncTime(zx_duration_t new_last_vsync) { last_vsync_time_ = new_last_vsync; }
};

class MockSessionUpdater : public SessionUpdater {
 public:
  MockSessionUpdater() : weak_factory_(this) {}

  // |SessionUpdater|
  SessionUpdater::UpdateResults UpdateSessions(std::unordered_set<SessionId> sessions_to_update,
                                               zx_time_t presentation_time,
                                               uint64_t trace_id = 0) override;

  // |SessionUpdater|
  void PrepareFrame(zx_time_t presentation_time, uint64_t frame_number) override {
    ++prepare_frame_call_count_;
  }

  // AddCallback() adds a closure to be returned by UpdateSessions(), and returns a
  // CallbackStatus struct that can be used to observe the current status of the callback.
  struct CallbackStatus {
    // The SessionId that this update corresponds to.
    SessionId session_id = 0;
    // Number of times that the update was rescheduled due to the fences not
    // being ready.
    size_t reschedule_count = 0;
    // Becomes true when the associated callback is passed to the UpdateManager, i.e. after the
    // fences are reached and the update has been "applied" before "rendering".
    bool callback_passed = false;
    // Becomes true when the associated callback is invoked (the callback itself is created within
    // ScheduleUpdate(), and is not visible to the caller).
    bool callback_invoked = false;
    // Becomes true when the updater disappears before the callback is invoked.
    bool updater_disappeared = false;
    // The PresentationInfo that was passed to the callback, valid only if |callback_invoked| is
    // true.
    PresentationInfo presentation_info;
  };
  std::shared_ptr<const CallbackStatus> AddCallback(SessionId id, zx_time_t presentation_time,
                                                    zx_time_t acquire_fence_time);

  // By default, rendering is enabled and |UpdateSessions()| will return ".needs_render = true" if
  // any session updates were applied.  This allows a test to override that behavior to
  // unconditionally disable rendering.
  void SuppressNeedsRendering(bool should_suppress) { rendering_suppressed_ = should_suppress; }

  // By default, we expect that the sessions identified by UpdateSessions()'s |sessions_to_update|
  // will all have at least one update queued.  This will not be the case in multi-updater scenarios
  // (because each updater is responsible for only some of the sessions, and will therefore receive
  // unknown session IDs); this method relaxes the restriction for those tests.
  void BeRelaxedAboutUnexpectedSessionUpdates() {
    be_relaxed_about_unexpected_session_updates_ = true;
  }

  // Simulate killing of a session.  This simply treats the session (and any associated updates) as
  // absent during |UpdateSession()|.
  void KillSession(SessionId session_id) { dead_sessions_.insert(session_id); }

  uint32_t update_sessions_call_count() { return update_sessions_call_count_; }
  uint32_t prepare_frame_call_count() { return prepare_frame_call_count_; }
  uint32_t signal_successful_present_callback_count() {
    return signal_successful_present_callback_count_;
  }

  fxl::WeakPtr<MockSessionUpdater> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  uint32_t update_sessions_call_count_ = 0;
  uint32_t prepare_frame_call_count_ = 0;
  uint32_t signal_successful_present_callback_count_ = 0;

  // Instances are generated by |AddCallback()|, and model the queuing of batched session
  // updates, and the callback that is invoked once the update has been applied to the
  // scene, and the corresponding frame rendered.
  struct Update {
    // Target presentation time.
    zx_time_t target;
    // Time that the fences will be finished.
    zx_time_t fences_done;
    // Updated to allow the test to track progress.
    std::shared_ptr<CallbackStatus> status;
    // Callback that will be invoked when UpdateManager::SignalPresentCallbacks() is called.
    OnPresentedCallback callback;
  };
  std::map<SessionId, std::queue<Update>> updates_;

  // Stores session IDs that were passed to |UpdateSessions()|, but for which no corresponding
  // updates were registered.
  std::set<SessionId> dead_sessions_;
  bool be_relaxed_about_unexpected_session_updates_ = false;

  // See |SuppressNeedsRendering()|.
  bool rendering_suppressed_ = false;

  fxl::WeakPtrFactory<MockSessionUpdater> weak_factory_;  // must be last
};

class MockFrameRenderer : public FrameRenderer {
 public:
  MockFrameRenderer() : weak_factory_(this) {}

  // |FrameRenderer|
  bool RenderFrame(const FrameTimingsPtr& frame_timings, zx_time_t presentation_time);

  // Need to call this in order to trigger the OnFramePresented() callback in
  // FrameScheduler, but is not valid to do until after RenderFrame has returned
  // to FrameScheduler. Hence this separate method.
  void EndFrame(size_t frame_index, zx_time_t time_done);

  // Signal frame |frame_index| that it has been rendered.
  void SignalFrameRendered(uint64_t frame_number, zx_time_t time_done);

  // Signal frame |frame_index| that it has been presented.
  void SignalFramePresented(uint64_t frame_number, zx_time_t time_done);

  // Signal frame |frame_index| that it has been dropped.
  void SignalFrameDropped(uint64_t frame_number);

  // Manually set value returned from RenderFrame.
  void SetRenderFrameReturnValue(bool new_value) { render_frame_return_value_ = new_value; }
  uint32_t render_frame_call_count() { return render_frame_call_count_; }
  size_t pending_frames() { return frames_.size(); }

  fxl::WeakPtr<MockFrameRenderer> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  void CleanUpFrame(uint64_t frame_number);

  bool render_frame_return_value_ = true;
  uint32_t render_frame_call_count_ = 0;

  struct Timings {
    FrameTimingsPtr frame_timings;
    size_t swapchain_index = -1;
    uint32_t frame_rendered = false;
    uint32_t frame_presented = false;
  };
  std::unordered_map<uint64_t, Timings> frames_;

  uint64_t last_frame_number_ = -1;

  fxl::WeakPtrFactory<MockFrameRenderer> weak_factory_;  // must be last
};

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_TESTS_FRAME_SCHEDULER_MOCKS_H_
