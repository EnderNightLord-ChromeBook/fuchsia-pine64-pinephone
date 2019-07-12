// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_ENGINE_DEFAULT_FRAME_SCHEDULER_H_
#define GARNET_LIB_UI_GFX_ENGINE_DEFAULT_FRAME_SCHEDULER_H_

#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/inspect/inspect.h>
#include <lib/zx/time.h>

#include <queue>

#include "garnet/lib/ui/gfx/engine/frame_predictor.h"
#include "garnet/lib/ui/gfx/engine/frame_scheduler.h"
#include "garnet/lib/ui/gfx/engine/frame_stats.h"
#include "garnet/lib/ui/gfx/id.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace scenic_impl {
namespace gfx {

class Display;

// TODOs can be found in the frame scheduler epic: SCN-1202. Any new bugs filed concerning the frame
// scheduler should be added to it as well.
class DefaultFrameScheduler : public FrameScheduler {
 public:
  explicit DefaultFrameScheduler(const Display* display, std::unique_ptr<FramePredictor> predictor,
                                 inspect::Node inspect_node = inspect::Node());
  ~DefaultFrameScheduler();

  // |FrameScheduler|
  void SetFrameRenderer(fxl::WeakPtr<FrameRenderer> frame_renderer) override;

  // |FrameScheduler|
  void AddSessionUpdater(fxl::WeakPtr<SessionUpdater> session_updater) override;

  // |FrameScheduler|
  //
  // If |render_continuously|, we keep rendering frames regardless of whether they're requested
  // using RequestFrame().
  void SetRenderContinuously(bool render_continuously) override;

  // |FrameScheduler|
  //
  // Tell the FrameScheduler to schedule a frame. This is also used for updates triggered by
  // something other than a Session update i.e. an ImagePipe with a new Image to present.
  void ScheduleUpdateForSession(zx_time_t presentation_time,
                                scenic_impl::SessionId session) override;

  constexpr static zx::duration kInitialRenderDuration = zx::msec(5);
  constexpr static zx::duration kInitialUpdateDuration = zx::msec(1);

  // Public for testing.
  constexpr static size_t kMaxOutstandingFrames = 2;

  // Helper class that manages:
  // - registration of SessionUpdaters
  // - tracking callbacks that need to be invoked.
  class UpdateManager {
   public:
    UpdateManager() = default;

    // Add |session_updater| to the list of updaters on which |UpdateSessions()| and
    // |PrepareFrame()| will be invoked.
    void AddSessionUpdater(fxl::WeakPtr<SessionUpdater> session_updater);

    // Schedules an update for the specified session.  All updaters registered by
    // |AddSessionUpdater()| are notified when |ApplyUpdates()| is called with an equal or later
    // presentation time.
    void ScheduleUpdate(zx_time_t presentation_time, SessionId session);

    // Returned by |ApplyUpdates()|; used by a |FrameScheduler| to decide whether to render a frame
    // and/or schedule another frame to be rendered.
    struct ApplyUpdatesResult {
      bool needs_render;
      bool needs_reschedule;
    };
    // Calls |SessionUpdater::UpdateSessions()| on all updaters, and uses the returned
    // |SessionUpdater::UpdateResults| to generate the returned |ApplyUpdatesResult|.
    ApplyUpdatesResult ApplyUpdates(zx_time_t presentation_time, zx_time_t vsync_interval,
                                    uint64_t frame_number);

    // Return true if there are any scheduled session updates that have not yet been applied.
    bool HasUpdatableSessions() const { return !updatable_sessions_.empty(); }

    zx_time_t EarliestRequestedPresentationTime() {
      FXL_DCHECK(HasUpdatableSessions());
      return updatable_sessions_.top().requested_presentation_time;
    }

    // Creates a ratchet point for the updater. All present calls that were updated before this
    // point will be signaled with the next call to |SignalPresentCallbacks()|.
    void RatchetPresentCallbacks(zx_time_t presentation_time, uint64_t frame_number);

    // Signal that all updates before the last ratchet point have been presented.  The signaled
    // callbacks are every successful present between the last time |SignalPresentCallbacks()| was
    // called and the most recent call to |RatchetPresentCallbacks()|.
    void SignalPresentCallbacks(fuchsia::images::PresentationInfo info);

   private:
    std::vector<fxl::WeakPtr<SessionUpdater>> session_updaters_;

    // Sessions that have updates to apply, sorted by requested presentation time from earliest to
    // latest.
    struct SessionUpdate {
      SessionId session_id;
      zx_time_t requested_presentation_time;

      bool operator>(const SessionUpdate& rhs) const {
        return requested_presentation_time > rhs.requested_presentation_time;
      }
    };
    std::priority_queue<SessionUpdate, std::vector<SessionUpdate>, std::greater<SessionUpdate>>
        updatable_sessions_;

    std::queue<OnPresentedCallback> callbacks_this_frame_;
    std::queue<OnPresentedCallback> pending_callbacks_;
  };

 protected:
  // |FrameScheduler|
  void OnFramePresented(const FrameTimings& timings) override;

  // |FrameScheduler|
  void OnFrameRendered(const FrameTimings& timings) override;

 private:
  // Requests a new frame to be drawn, which schedules the next wake up time for rendering. If we've
  // already scheduled a wake up time, it checks if it needs rescheduling and deals with it
  // appropriately.
  void RequestFrame();

  // Update the global scene and then draw it... maybe.  There are multiple reasons why this might
  // not happen.  For example, the swapchain might apply back-pressure if we can't hit our target
  // frame rate. Or, the frame before this one has yet to finish rendering. Etc.
  void MaybeRenderFrame(async_dispatcher_t*, async::TaskBase*, zx_status_t);

  // Computes the target presentation time for the requested presentation time, and a wake-up time
  // that is early enough to start rendering in order to hit the target presentation time. These
  // times are guaranteed to be in the future.
  std::pair<zx_time_t, zx_time_t> ComputePresentationAndWakeupTimesForTargetTime(
      zx_time_t requested_presentation_time) const;

  // Executes updates that are scheduled up to and including a given presentation time.
  UpdateManager::ApplyUpdatesResult ApplyUpdates(zx_time_t presentation_time);

  // References.
  async_dispatcher_t* const dispatcher_;
  const Display* const display_;

  fxl::WeakPtr<FrameRenderer> frame_renderer_;

  // State.
  uint64_t frame_number_ = 0;
  std::vector<FrameTimingsPtr> outstanding_frames_;
  bool render_continuously_ = false;
  bool currently_rendering_ = false;
  bool render_pending_ = false;
  zx_time_t wakeup_time_;
  zx_time_t next_presentation_time_;
  UpdateManager update_manager_;
  std::unique_ptr<FramePredictor> frame_predictor_;

  // The async task that wakes up to start rendering.
  async::TaskMethod<DefaultFrameScheduler, &DefaultFrameScheduler::MaybeRenderFrame>
      frame_render_task_{this};

  inspect::Node inspect_node_;
  inspect::UIntMetric inspect_frame_number_;
  inspect::UIntMetric inspect_last_successful_update_start_time_;
  inspect::UIntMetric inspect_last_successful_render_start_time_;

  FrameStats stats_;

  fxl::WeakPtrFactory<DefaultFrameScheduler> weak_factory_;  // must be last

  FXL_DISALLOW_COPY_AND_ASSIGN(DefaultFrameScheduler);
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_ENGINE_DEFAULT_FRAME_SCHEDULER_H_
