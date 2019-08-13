// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_TESTING_VIEWS_ROTATED_SQUARE_VIEW_H_
#define GARNET_TESTING_VIEWS_ROTATED_SQUARE_VIEW_H_

#include "garnet/testing/views/background_view.h"

namespace scenic {

// Displays a static frame of the Spinning Square example.
// See also examples/ui/spinning_square
class RotatedSquareView : public BackgroundView {
 public:
  static constexpr float kSquareElevation = 8.f;
  static constexpr float kSquareAngle = M_PI / 4;

  RotatedSquareView(ViewContext context, const std::string& debug_name = "RotatedSquareView");

 private:
  // |BackgroundView|
  void Draw(float cx, float cy, float sx, float sy) override;

  ShapeNode square_node_;
};

}  // namespace scenic
#endif  // GARNET_TESTING_VIEWS_ROTATED_SQUARE_VIEW_H_
