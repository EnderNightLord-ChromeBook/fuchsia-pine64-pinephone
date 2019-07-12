// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/displays/display.h"

#include <trace/event.h>
#include <zircon/syscalls.h>

#include "garnet/lib/ui/gfx/util/time.h"
#include "src/lib/fxl/logging.h"

namespace scenic_impl {
namespace gfx {

Display::Display(uint64_t id, uint32_t width_in_px, uint32_t height_in_px,
                 std::vector<zx_pixel_format_t> pixel_formats)
    : vsync_interval_(kNsecsFor60fps),
      last_vsync_time_(dispatcher_clock_now()),
      display_id_(id),
      width_in_px_(width_in_px),
      height_in_px_(height_in_px),
      pixel_formats_(pixel_formats) {
  zx::event::create(0, &ownership_event_);
}
Display::Display(uint64_t id, uint32_t width_in_px, uint32_t height_in_px)
    : Display(id, width_in_px, height_in_px, {ZX_PIXEL_FORMAT_ARGB_8888}) {}

void Display::Claim() {
  FXL_DCHECK(!claimed_);
  claimed_ = true;
}

void Display::Unclaim() {
  FXL_DCHECK(claimed_);
  claimed_ = false;
}

void Display::OnVsync(zx_time_t timestamp) {
  zx_duration_t time_since_last_vsync = timestamp - last_vsync_time_;
  last_vsync_time_ = timestamp;

  // Estimate current vsync interval. Need to include a maximum to mitigate any
  // potential issues during startup and long breaks.
  vsync_interval_ =
      time_since_last_vsync < kMaximumVsyncInterval ? time_since_last_vsync : vsync_interval_;

  TRACE_INSTANT("gfx", "Display::OnVsync", TRACE_SCOPE_PROCESS, "Timestamp", timestamp,
                "Vsync interval", vsync_interval_);
}

}  // namespace gfx
}  // namespace scenic_impl
