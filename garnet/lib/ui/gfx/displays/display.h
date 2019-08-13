// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_DISPLAYS_DISPLAY_H_
#define GARNET_LIB_UI_GFX_DISPLAYS_DISPLAY_H_

#include <zircon/types.h>

#include <array>
#include <cstdint>
#include <vector>

#include "garnet/lib/ui/gfx/displays/color_transform.h"
#include "lib/zx/event.h"
#include "src/lib/fxl/macros.h"
#include "zircon/pixelformat.h"

namespace scenic_impl {
namespace gfx {

// Display is a placeholder that provides make-believe values for screen
// resolution, vsync interval, last vsync time, etc.
class Display {
 public:
  Display(uint64_t id, uint32_t width_in_px, uint32_t height_in_px,
          std::vector<zx_pixel_format_t> pixel_formats);
  Display(uint64_t id, uint32_t width_in_px, uint32_t height_in_px);
  virtual ~Display() = default;

  // Should be registered by DisplayCompositor to be called on every received
  // vsync signal.
  void OnVsync(zx::time timestamp);

  // Obtain the time of the last Vsync, in nanoseconds.
  zx::time GetLastVsyncTime() const { return last_vsync_time_; }

  // Obtain the interval between Vsyncs, in nanoseconds.
  zx::duration GetVsyncInterval() const { return vsync_interval_; };

  // Claiming a display means that no other display renderer can use it.
  bool is_claimed() const { return claimed_; }
  void Claim();
  void Unclaim();

  // The display's ID in the context of the DisplayManager's DisplayController.
  uint64_t display_id() { return display_id_; };
  uint32_t width_in_px() { return width_in_px_; };
  uint32_t height_in_px() { return height_in_px_; };
  const std::vector<zx_pixel_format_t>& pixel_formats() const { return pixel_formats_; }

  // Event signaled by DisplayManager when ownership of the display
  // changes. This event backs Scenic's GetDisplayOwnershipEvent API.
  const zx::event& ownership_event() { return ownership_event_; };

  void set_color_transform(const ColorTransform& transform) { color_transform_ = transform; }

  const ColorTransform& color_transform() const { return color_transform_; }

 protected:
  // Protected for testing purposes.
  zx::duration vsync_interval_;
  zx::time last_vsync_time_;

 private:
  // The maximum vsync interval we would ever expect.
  static constexpr zx::duration kMaximumVsyncInterval = zx::msec(100);

  // Vsync interval of a 60 Hz screen.
  // Used as a default value before real timings arrive.
  static constexpr zx::duration kNsecsFor60fps = zx::duration(16'666'667);  // 16.666667ms

  // This is the color transform used by the display controller
  // to augment the final display color. See |ColorTransform|
  // comments for details on how this transform modifies the
  // display pixels.
  ColorTransform color_transform_;

  const uint64_t display_id_;
  const uint32_t width_in_px_;
  const uint32_t height_in_px_;
  zx::event ownership_event_;
  std::vector<zx_pixel_format_t> pixel_formats_;

  bool claimed_ = false;

  FXL_DISALLOW_COPY_AND_ASSIGN(Display);
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_DISPLAYS_DISPLAY_H_
