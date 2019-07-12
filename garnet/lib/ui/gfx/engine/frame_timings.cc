// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/engine/frame_timings.h"

#include "garnet/lib/ui/gfx/engine/frame_scheduler.h"

namespace scenic_impl {
namespace gfx {

FrameTimings::FrameTimings(FrameScheduler* frame_scheduler, uint64_t frame_number,
                           zx_time_t target_presentation_time, zx_time_t latch_time,
                           zx_time_t rendering_started_time)
    : frame_scheduler_(frame_scheduler),
      frame_number_(frame_number),
      target_presentation_time_(target_presentation_time),
      latch_point_time_(latch_time),
      rendering_started_time_(rendering_started_time) {}

size_t FrameTimings::RegisterSwapchain() {
  // All swapchains that we are timing must be added before any of them finish.
  // The purpose of this is to verify that we cannot notify the FrameScheduler
  // that the frame has finished before all swapchains have been added.
  FXL_DCHECK(frame_rendered_count_ == 0);
  FXL_DCHECK(frame_presented_count_ == 0);
  FXL_DCHECK(actual_presentation_time_ == kTimeUninitialized);
  swapchain_records_.push_back({});
  return swapchain_records_.size() - 1;
}

void FrameTimings::OnFrameUpdated(zx_time_t time) {
  FXL_DCHECK(!finalized()) << "Frame was finalized, cannot record update time";
  FXL_DCHECK(updates_finished_time_ == kTimeUninitialized)
      << "Error, update time already recorded.";
  updates_finished_time_ = time;

  FXL_DCHECK(updates_finished_time_ >= latch_point_time_) << "Error, updates took negative time";
}

void FrameTimings::OnFrameRendered(size_t swapchain_index, zx_time_t time) {
  FXL_DCHECK(swapchain_index < swapchain_records_.size());
  FXL_DCHECK(time > 0);

  auto& record = swapchain_records_[swapchain_index];
  FXL_DCHECK(record.frame_rendered_time == kTimeUninitialized)
      << "Frame render time already recorded for swapchain. Render time: "
      << record.frame_rendered_time;

  record.frame_rendered_time = time;
  ++frame_rendered_count_;
  if (time > rendering_finished_time_) {
    rendering_finished_time_ = time;
  }
  FXL_DCHECK(rendering_finished_time_ >= rendering_started_time_)
      << "Error, rendering took negative time";

  // TODO(SCN-1324): We currently only return the time of the longest received
  // render time. This is not a problem right now, since we only have cases with
  // a single swapchain/display, but need to figure out how to handle the
  // general case.
  // Note: Because there is a delay between when rendering is actually completed
  // and when EventTimestamper generates the timestamp, it's possible that the
  // rendering timestamp is adjusted when the present timestamp is applied. So,
  // the render_done_time might change between the call to the
  // |FrameScheduler::OnFrameRendered| and |finalized()|.
  if (received_all_frame_rendered_callbacks() && frame_scheduler_) {
    frame_scheduler_->OnFrameRendered(*this);
  }

  if (received_all_callbacks()) {
    Finalize();
  }
}

void FrameTimings::OnFramePresented(size_t swapchain_index, zx_time_t time) {
  FXL_DCHECK(swapchain_index < swapchain_records_.size());
  FXL_DCHECK(frame_presented_count_ < swapchain_records_.size());
  FXL_DCHECK(time > 0);

  auto& record = swapchain_records_[swapchain_index];
  FXL_DCHECK(record.frame_presented_time == kTimeUninitialized)
      << "Frame present time already recorded for swapchain. Present time: "
      << record.frame_presented_time;

  record.frame_presented_time = time;
  ++frame_presented_count_;
  // TODO(SCN-1324): We currently only return the time of the longest received
  // render time. This is not a problem right now, since we only have cases with
  // a single swapchain/display, but need to figure out how to handle the
  // general case.
  if (time > actual_presentation_time_) {
    actual_presentation_time_ = time;
  }

  if (received_all_callbacks()) {
    Finalize();
  }
}

void FrameTimings::OnFrameDropped(size_t swapchain_index) {
  // Indicates that "frame was dropped".
  actual_presentation_time_ = kTimeDropped;
  frame_was_dropped_ = true;

  // The record should also reflect that "frame was dropped". Additionally,
  // update counts to simulate calls to OnFrameRendered/OnFramePresented; this
  // maintains count-related invariants.
  auto& record = swapchain_records_[swapchain_index];
  record.frame_presented_time = kTimeDropped;
  actual_presentation_time_ = kTimeDropped;
  ++frame_presented_count_;

  // Do scheduler-related cleanup.
  if (received_all_callbacks()) {
    Finalize();
  }
}

FrameTimings::Timestamps FrameTimings::GetTimestamps() const {
  // Copy the current time values to a Timestamps struct. Some callers may call
  // this before all times are finalized - it is the caller's responsibility to
  // check if this is |finalized()| if it wants timestamps that are guaranteed
  // not to change. Additionally, some callers will maintain this struct beyond
  // the lifetime of the FrameTimings object (ie for collecting FrameStats), and
  // so the values are copied to allow the FrameTiming object to be destroyed.
  FrameTimings::Timestamps timestamps = {
      .latch_point_time = latch_point_time_,
      .update_done_time = updates_finished_time_,
      .render_start_time = rendering_started_time_,
      .render_done_time = rendering_finished_time_,
      .target_presentation_time = target_presentation_time_,
      .actual_presentation_time = actual_presentation_time_,
  };
  return timestamps;
}

void FrameTimings::ValidateRenderTime() {
  FXL_DCHECK(rendering_finished_time_ != kTimeUninitialized);
  FXL_DCHECK(actual_presentation_time_ != kTimeUninitialized);
  // NOTE: Because there is a delay between when rendering is actually
  // completed and when EventTimestamper generates the timestamp, it's
  // possible that the rendering timestamp is later than the present
  // timestamp. Since we know that's actually impossible, adjust the render
  // timestamp to make it a bit more accurate.
  if (rendering_finished_time_ > actual_presentation_time_) {
    // Reset redering_finished_time_ and adjust rendering times so that they are
    // all less than or equal to the corresponding present time.
    rendering_finished_time_ = kTimeUninitialized;
    for (auto& record : swapchain_records_) {
      FXL_DCHECK(record.frame_rendered_time != kTimeUninitialized);
      FXL_DCHECK(record.frame_presented_time != kTimeUninitialized);
      if (record.frame_rendered_time > record.frame_presented_time) {
        record.frame_rendered_time = record.frame_presented_time;
      }

      if (record.frame_rendered_time > rendering_finished_time_) {
        rendering_finished_time_ = record.frame_rendered_time;
      }
    }
  }
}

void FrameTimings::Finalize() {
  FXL_DCHECK(!finalized());
  finalized_ = true;

  ValidateRenderTime();

  if (frame_scheduler_) {
    frame_scheduler_->OnFramePresented(*this);
  }
}

}  // namespace gfx
}  // namespace scenic_impl
