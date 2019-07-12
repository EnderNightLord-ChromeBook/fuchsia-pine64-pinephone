// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_EXAMPLES_ESCHER_WATERFALL_SCENES_SHADOW_TEST_SCENE_H_
#define SRC_UI_EXAMPLES_ESCHER_WATERFALL_SCENES_SHADOW_TEST_SCENE_H_

#include "src/lib/fxl/macros.h"
#include "src/ui/lib/escher/scene/model.h"
#include "src/ui/lib/escher/scene/viewing_volume.h"

class ShadowTestScene {
 public:
  ShadowTestScene();
  ~ShadowTestScene();

  escher::Model GetModel(const escher::ViewingVolume& volume);

 private:
  escher::Material card_material_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ShadowTestScene);
};

#endif  // SRC_UI_EXAMPLES_ESCHER_WATERFALL_SCENES_SHADOW_TEST_SCENE_H_
