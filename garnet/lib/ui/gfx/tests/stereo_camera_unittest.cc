// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/ui/gfx/resources/stereo_camera.h"

#include <lib/ui/scenic/cpp/commands.h>

#include <gtest/gtest.h>

#include "garnet/lib/ui/gfx/tests/session_test.h"
#include "src/ui/lib/escher/util/epsilon_compare.h"

#include <glm/gtc/type_ptr.hpp>

namespace scenic_impl {
namespace gfx {
namespace test {

using StereoCameraTest = SessionTest;

TEST_F(StereoCameraTest, Basic) {
  constexpr ResourceId invalid_id = 0;
  constexpr ResourceId scene_id = 1;
  constexpr ResourceId camera_id = 2;
  ASSERT_TRUE(Apply(scenic::NewCreateSceneCmd(scene_id)));
  EXPECT_TRUE(Apply(scenic::NewCreateStereoCameraCmd(camera_id, scene_id)));
  EXPECT_FALSE(Apply(scenic::NewCreateStereoCameraCmd(camera_id, invalid_id)));

  // Not really projection matrices but we're just testing the setters
  glm::mat4 left_projection = glm::mat4(2);
  glm::mat4 right_projection = glm::mat4(3);

  EXPECT_TRUE(Apply(scenic::NewSetStereoCameraProjectionCmd(
      camera_id, glm::value_ptr(left_projection), glm::value_ptr(right_projection))));

  auto camera = session()->resources()->FindResource<StereoCamera>(camera_id);
  EXPECT_TRUE(camera);

  EXPECT_TRUE(escher::CompareMatrix(left_projection,
                                    camera->GetEscherCamera(StereoCamera::Eye::LEFT).projection()));
  EXPECT_TRUE(escher::CompareMatrix(
      right_projection, camera->GetEscherCamera(StereoCamera::Eye::RIGHT).projection()));
}

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl
