// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
#include "src/ui/lib/glm_workaround/glm_workaround.h"
// clang-format on

#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>
#include <gtest/gtest.h>
#include <lib/async/cpp/task.h>
#include <lib/fsl/vmo/vector.h>
#include <lib/sys/cpp/testing/test_with_environment.h>
#include <lib/ui/scenic/cpp/session.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <lib/images/cpp/images.h>
#include <lib/zx/clock.h>
#include <src/lib/fxl/logging.h>
#include <zircon/status.h>

#include <map>
#include <string>
#include <vector>

#include "garnet/lib/ui/gfx/tests/vk_session_test.h"
#include "garnet/testing/views/background_view.h"
#include "garnet/testing/views/coordinate_test_view.h"
#include "garnet/testing/views/opacity_view.h"
#include "garnet/testing/views/test_view.h"
#include "src/ui/lib/escher/impl/vulkan_utils.h"
#include "src/ui/lib/escher/hmd/pose_buffer.h"
#include "src/ui/lib/escher/test/gtest_vulkan.h"
#include "src/ui/lib/yuv/yuv.h"

namespace {

constexpr char kEnvironment[] = "ScenicPixelTest";
constexpr zx::duration kTimeout = zx::sec(15);
// If you change the size of YUV buffers, make sure that the YUV test in
// host_image_unittest.cc is also updated. Unlike that unit test,
// scenic_pixel_test.cc has no way to confirm that it is going through the
// direct-to-GPU path.
// TODO(SCN-1387): This number needs to be queried via sysmem or vulkan.
constexpr uint32_t kYuvSize = 64;

// These tests need Scenic and RootPresenter at minimum, which expand to the
// dependencies below. Using |TestWithEnvironment|, we use
// |fuchsia.sys.Environment| and |fuchsia.sys.Loader| from the system (declared
// in our *.cmx sandbox) and launch these other services in the environment we
// create in our test fixture.
//
// Another way to do this would be to whitelist these services in our sandbox
// and inject/start them via the |fuchsia.test| facet. However that has the
// disadvantage that it uses one instance of those services across all tests in
// the binary, making each test not hermetic wrt. the others. A trade-off is
// that the |TestWithEnvironment| method is more verbose.
const std::map<std::string, std::string> kServices = {
    {"fuchsia.tracing.provider.Registry",
     "fuchsia-pkg://fuchsia.com/trace_manager#meta/trace_manager.cmx"},
    {"fuchsia.ui.policy.Presenter",
     "fuchsia-pkg://fuchsia.com/root_presenter#meta/root_presenter.cmx"},
    {"fuchsia.ui.scenic.Scenic", "fuchsia-pkg://fuchsia.com/scenic#meta/scenic.cmx"},
    {"fuchsia.ui.shortcut.Manager", "fuchsia-pkg://fuchsia.com/shortcut#meta/shortcut_manager.cmx"},
    {"fuchsia.vulkan.loader.Loader",
     "fuchsia-pkg://fuchsia.com/vulkan_loader#meta/vulkan_loader.cmx"},
    {"fuchsia.sysmem.Allocator",
     "fuchsia-pkg://fuchsia.com/sysmem_connector#meta/sysmem_connector.cmx"}};

struct DisplayDimensions {
  float width, height;
};

struct TestSession {
  static constexpr float kDefaultCameraOffset = 1001;

  TestSession(fuchsia::ui::scenic::Scenic* scenic, const DisplayDimensions& display_dimensions)
      : session(scenic),
        display_dimensions(display_dimensions),
        compositor(&session),
        layer_stack(&session),
        layer(&session),
        renderer(&session),
        scene(&session),
        ambient_light(&session),
        root_node(&session) {
    compositor.SetLayerStack(layer_stack);
    layer_stack.AddLayer(layer);
    layer.SetSize(display_dimensions.width, display_dimensions.height);
    layer.SetRenderer(renderer);
    scene.AddLight(ambient_light);
    ambient_light.SetColor(1.f, 1.f, 1.f);
    scene.AddChild(root_node.id());
  }

  // Sets up a camera at (x, y) = (width / 2, height / 2) looking at +z such
  // that the near plane is at -1000 and the far plane is at 0.
  //
  // Note that the ortho camera (fov = 0) ignores the transform and is
  // effectively always set this way.
  template <typename Camera = scenic::Camera>
  Camera SetUpCamera(float offset = kDefaultCameraOffset) {
    // SCN-1276: The near plane is hardcoded at -1000 and far at 0 in camera
    // space.
    const float eye_position[3] = {display_dimensions.width / 2.f, display_dimensions.height / 2.f,
                                   -offset};
    const float look_at[3] = {display_dimensions.width / 2.f, display_dimensions.height / 2.f, 1};
    static const float up[3] = {0, -1, 0};
    Camera camera(scene);
    camera.SetTransform(eye_position, look_at, up);
    renderer.SetCamera(camera.id());
    return camera;
  }

  scenic::Session session;
  const DisplayDimensions display_dimensions;
  scenic::DisplayCompositor compositor;
  scenic::LayerStack layer_stack;
  scenic::Layer layer;
  scenic::Renderer renderer;
  scenic::Scene scene;
  scenic::AmbientLight ambient_light;
  scenic::EntityNode root_node;
};

// Test fixture that sets up an environment suitable for Scenic pixel tests
// and provides related utilities. The environment includes Scenic and
// RootPresenter, and their dependencies.
class ScenicPixelTest : public sys::testing::TestWithEnvironment {
 protected:
  ScenicPixelTest() {
    std::unique_ptr<sys::testing::EnvironmentServices> services = CreateServices();

    for (const auto& entry : kServices) {
      fuchsia::sys::LaunchInfo launch_info;
      launch_info.url = entry.second;
      services->AddServiceWithLaunchInfo(std::move(launch_info), entry.first);
    }

    environment_ = CreateNewEnclosingEnvironment(kEnvironment, std::move(services));

    environment_->ConnectToService(scenic_.NewRequest());
    scenic_.set_error_handler([](zx_status_t status) {
      FAIL() << "Lost connection to Scenic: " << zx_status_get_string(status);
    });
  }

  // Blocking wrapper around |Scenic::TakeScreenshot|. This should not be called
  // from within a loop |Run|, as it spins up its own to block and nested loops
  // are undefined behavior.
  scenic::Screenshot TakeScreenshot() {
    fuchsia::ui::scenic::ScreenshotData screenshot_out;
    scenic_->TakeScreenshot(
        [this, &screenshot_out](fuchsia::ui::scenic::ScreenshotData screenshot, bool status) {
          EXPECT_TRUE(status) << "Failed to take screenshot";
          screenshot_out = std::move(screenshot);
          QuitLoop();
        });
    EXPECT_FALSE(RunLoopWithTimeout(kTimeout)) << "Timed out waiting for screenshot.";
    return scenic::Screenshot(screenshot_out);
  }

  // Create a |ViewContext| that allows us to present a view via
  // |RootPresenter|. See also examples/ui/hello_base_view
  scenic::ViewContext CreatePresentationContext() {
    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();

    scenic::ViewContext view_context = {
        .session_and_listener_request =
            scenic::CreateScenicSessionPtrAndListenerRequest(scenic_.get()),
        .view_token = std::move(view_token),
    };

    fuchsia::ui::policy::PresenterPtr presenter;
    environment_->ConnectToService(presenter.NewRequest());
    presenter->PresentView(std::move(view_holder_token), nullptr);

    return view_context;
  }

  // Runs until the view renders its next frame.
  void RunUntilPresent(scenic::TestView* view) {
    // Typical sequence of events:
    // 1. We set up a view bound as a |SessionListener|.
    // 2. The view sends its initial |Present| to get itself connected, without
    //    a callback.
    // 3. We call |RunUntilPresent| which sets a present callback on our
    //    |TestView|.
    // 4. |RunUntilPresent| runs the message loop, which allows the view to
    //    receive a Scenic event telling us our metrics.
    // 5. In response, the view sets up the scene graph with the test scene.
    // 6. The view calls |Present| with the callback set in |RunUntilPresent|.
    // 7. The still-running message loop eventually dispatches the present
    //    callback, which quits the loop.

    view->set_present_callback([this](auto) { QuitLoop(); });
    ASSERT_FALSE(RunLoopWithTimeout(zx::sec(10)));
  }

  // Blocking call to |scenic::Session::Present|.
  void Present(scenic::Session* session, zx::time present_time = zx::time(0)) {
    session->Present(present_time, [this](auto) { QuitLoop(); });
    ASSERT_FALSE(RunLoopWithTimeout(zx::sec(10)));
  }

  DisplayDimensions GetDisplayDimensions() {
    DisplayDimensions display_dimensions;
    scenic_->GetDisplayInfo(
        [this, &display_dimensions](fuchsia::ui::gfx::DisplayInfo display_info) {
          display_dimensions = {.width = static_cast<float>(display_info.width_in_px),
                                .height = static_cast<float>(display_info.height_in_px)};
          QuitLoop();
        });
    RunLoop();
    return display_dimensions;
  }

  // As an alternative to using RootPresenter, tests can set up their own
  // session. This offers more control over the camera and compositor.
  std::unique_ptr<TestSession> SetUpTestSession() {
    auto test_session = std::make_unique<TestSession>(scenic_.get(), GetDisplayDimensions());

    test_session->session.set_error_handler([this](auto) {
      FXL_LOG(ERROR) << "Session terminated.";
      QuitLoop();
    });

    return test_session;
  }

  fuchsia::ui::scenic::ScenicPtr scenic_;

 private:
  std::unique_ptr<sys::testing::EnclosingEnvironment> environment_;
};

TEST_F(ScenicPixelTest, SolidColor) {
  scenic::BackgroundView view(CreatePresentationContext());
  RunUntilPresent(&view);

  scenic::Screenshot screenshot = TakeScreenshot();
  ASSERT_FALSE(screenshot.empty());

  // We could assert on each pixel individually, but a histogram might give us a
  // more meaningful failure.
  std::map<scenic::Color, size_t> histogram = screenshot.Histogram();

  EXPECT_GT(histogram[scenic::BackgroundView::kBackgroundColor], 0u);
  histogram.erase(scenic::BackgroundView::kBackgroundColor);
  // This assert is written this way so that, when it fails, it prints out all
  // the unexpected colors
  EXPECT_EQ((std::map<scenic::Color, size_t>){}, histogram) << "Unexpected colors";
}

TEST_F(ScenicPixelTest, NV12Texture) {
  scenic::BackgroundView view(CreatePresentationContext());
  fuchsia::images::ImageInfo image_info{
      .width = kYuvSize,
      .height = kYuvSize,
      .stride = static_cast<uint32_t>(
          kYuvSize * images::StrideBytesPerWidthPixel(fuchsia::images::PixelFormat::NV12)),
      .pixel_format = fuchsia::images::PixelFormat::NV12,
  };

  uint32_t num_pixels = image_info.width * image_info.height;
  uint64_t image_vmo_bytes = images::ImageSize(image_info);
  EXPECT_EQ((3 * num_pixels) / 2, image_vmo_bytes);

  zx::vmo image_vmo;
  zx_status_t status = zx::vmo::create(image_vmo_bytes, 0, &image_vmo);
  EXPECT_EQ(ZX_OK, status);
  uint8_t* vmo_base;
  status = zx::vmar::root_self()->map(0, image_vmo, 0, image_vmo_bytes,
                                      ZX_VM_PERM_WRITE | ZX_VM_PERM_READ,
                                      reinterpret_cast<uintptr_t*>(&vmo_base));
  EXPECT_EQ(ZX_OK, status);

  static const uint8_t kYValue = 110;
  static const uint8_t kUValue = 192;
  static const uint8_t kVValue = 192;

  // Set all the Y pixels at full res.
  for (uint32_t i = 0; i < num_pixels; ++i) {
    vmo_base[i] = kYValue;
  }

  // Set all the UV pixels pairwise at half res.
  for (uint32_t i = num_pixels; i < num_pixels + num_pixels / 2; i += 2) {
    vmo_base[i] = kUValue;
    vmo_base[i + 1] = kVValue;
  }

  view.SetHostImage(std::move(image_vmo), image_vmo_bytes, image_info);
  RunUntilPresent(&view);

  scenic::Screenshot screenshot = TakeScreenshot();
  ASSERT_FALSE(screenshot.empty());

  // We could assert on each pixel individually, but a histogram might give us a
  // more meaningful failure.
  std::map<scenic::Color, size_t> histogram = screenshot.Histogram();

  uint8_t bgra[4];
  yuv::YuvToBgra(kYValue, kUValue, kVValue, bgra);
  scenic::Color color(bgra[2], bgra[1], bgra[0], bgra[3]);
  EXPECT_GT(histogram[color], 0u);
  histogram.erase(color);

  // This assert is written this way so that, when it fails, it prints out all
  // the unexpected colors
  EXPECT_EQ((std::map<scenic::Color, size_t>){}, histogram) << "Unexpected colors";
}

TEST_F(ScenicPixelTest, ViewCoordinates) {
  scenic::CoordinateTestView view(CreatePresentationContext());
  RunUntilPresent(&view);

  scenic::Screenshot screenshot = TakeScreenshot();

  EXPECT_EQ(scenic::Color({0, 0, 0, 255}), screenshot.ColorAt(.25f, .25f));
  EXPECT_EQ(scenic::Color({0, 0, 255, 255}), screenshot.ColorAt(.25f, .75f));
  EXPECT_EQ(scenic::Color({255, 0, 0, 255}), screenshot.ColorAt(.75f, .25f));
  EXPECT_EQ(scenic::Color({255, 0, 255, 255}), screenshot.ColorAt(.75f, .75f));
  EXPECT_EQ(scenic::Color({0, 255, 0, 255}), screenshot.ColorAt(.5f, .5f));
}

// Draws and tests the following coordinate test pattern without views:
// ___________________________________
// |                |                |
// |     BLACK      |        RED     |
// |           _____|_____           |
// |___________|  GREEN  |___________|
// |           |_________|           |
// |                |                |
// |      BLUE      |     MAGENTA    |
// |________________|________________|
//
TEST_F(ScenicPixelTest, GlobalCoordinates) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;
  scenic::EntityNode* const root_node = &test_session->root_node;

  const float pane_width = display_width / 2;
  const float pane_height = display_height / 2;

  for (uint32_t i = 0; i < 2; i++) {
    for (uint32_t j = 0; j < 2; j++) {
      scenic::Rectangle pane_shape(session, pane_width, pane_height);
      scenic::Material pane_material(session);
      pane_material.SetColor(i * 255.f, 0, j * 255.f, 255);

      scenic::ShapeNode pane_node(session);
      pane_node.SetShape(pane_shape);
      pane_node.SetMaterial(pane_material);
      pane_node.SetTranslation((i + .5f) * pane_width, (j + .5f) * pane_height, -20);
      root_node->AddChild(pane_node);
    }
  }

  scenic::Rectangle pane_shape(session, display_width / 4, display_height / 4);
  scenic::Material pane_material(session);
  pane_material.SetColor(0, 255, 0, 255);

  scenic::ShapeNode pane_node(session);
  pane_node.SetShape(pane_shape);
  pane_node.SetMaterial(pane_material);
  pane_node.SetTranslation(.5f * display_width, .5f * display_height, -40);
  root_node->AddChild(pane_node);

  // Actual tests. Test the same scene with an orthographic and perspective
  // camera.
  std::string camera_type[2] = {"orthographic", "perspective"};
  auto camera = test_session->SetUpCamera();
  float fov[2] = {0, 2 * atan((display_height / 2.f) / TestSession::kDefaultCameraOffset)};

  for (int i = 0; i < 2; i++) {
    FXL_LOG(INFO) << "Testing " << camera_type[i] << " camera";
    camera.SetProjection(fov[i]);

    Present(session);
    scenic::Screenshot screenshot = TakeScreenshot();

    EXPECT_EQ(scenic::Color({0, 0, 0, 255}), screenshot.ColorAt(.25f, .25f));
    EXPECT_EQ(scenic::Color({0, 0, 255, 255}), screenshot.ColorAt(.25f, .75f));
    EXPECT_EQ(scenic::Color({255, 0, 0, 255}), screenshot.ColorAt(.75f, .25f));
    EXPECT_EQ(scenic::Color({255, 0, 255, 255}), screenshot.ColorAt(.75f, .75f));
    EXPECT_EQ(scenic::Color({0, 255, 0, 255}), screenshot.ColorAt(.5f, .5f));
  }
}

// Draws a white rectangle on a black background rendered with a stereo
// camera, which produces an image something like this:
// _____________________________________
// |                                   |
// |   ___________       ___________   |
// |   |         |       |         |   |
// |   |         |       |         |   |
// |   |  WHITE  | BLACK |  WHITE  |   |
// |   |         |       |         |   |
// |   |_________|       |_________|   |
// |                                   |
// |___________________________________|
//

TEST_F(ScenicPixelTest, StereoCamera) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;

  const float viewport_width = display_width / 2;
  const float viewport_height = display_height;

  float fovy = 2 * atan((display_height / 2.f) / TestSession::kDefaultCameraOffset);
  glm::mat4 projection = glm::perspective(fovy, viewport_width / viewport_height, 0.1f,
                                          TestSession::kDefaultCameraOffset);
  projection = glm::scale(projection, glm::vec3(1.f, -1.f, 1.f));

  test_session->SetUpCamera<scenic::StereoCamera>().SetStereoProjection(glm::value_ptr(projection),
                                                                        glm::value_ptr(projection));

  const float pane_width = viewport_width / 2;
  const float pane_height = viewport_height / 2;

  glm::vec3 translation(.5f * display_width, .5f * display_height, -10);

  scenic::Rectangle pane_shape(session, pane_width, pane_height);

  scenic::Material pane_material(session);
  pane_material.SetColor(255, 255, 255, 255);

  scenic::ShapeNode pane_shape_node(session);
  pane_shape_node.SetShape(pane_shape);
  pane_shape_node.SetMaterial(pane_material);
  pane_shape_node.SetTranslation(translation.x, translation.y, translation.z);
  test_session->root_node.AddChild(pane_shape_node);

  Present(session);
  scenic::Screenshot screenshot = TakeScreenshot();

  // Color array to index 0=BLACK 1=WHITE
  scenic::Color colors[2] = {scenic::Color{0, 0, 0, 0}, scenic::Color{255, 255, 255, 255}};

  // Expected results by index into colors array. Column major.
  // Note how this is a transposed, low-res version of the scene being drawn.
  // clang-format off
  int expected[8][4] = {{0, 0, 0, 0},
                        {0, 1, 1, 0},
                        {0, 1, 1, 0},
                        {0, 0, 0, 0},
                        {0, 0, 0, 0},
                        {0, 1, 1, 0},
                        {0, 1, 1, 0},
                        {0, 0, 0, 0}};
  // clang-format on

  // Test 8 columns of 4 samples each
  int num_x_samples = 8;
  int num_y_samples = 4;
  float x_step = 1.f / num_x_samples;
  float y_step = 1.f / num_y_samples;
  // i maps to x, j maps to y
  for (int i = 0; i < num_x_samples; i++) {
    for (int j = 0; j < num_y_samples; j++) {
      float x = x_step / 2 + i * x_step;
      float y = y_step / 2 + j * y_step;
      EXPECT_EQ(colors[expected[i][j]], screenshot.ColorAt(x, y))
          << "i = " << i << ", j = " << j << ", Sample Location: {" << x << ", " << y << "}";
    }
  }
}

// At a high level this test puts a camera inside a cube where each face is a
// different color, then uses a pose buffer to point the camera at different
// faces, using the colors to verify the pose buffer is working as expected.
TEST_F(ScenicPixelTest, PoseBuffer) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;
  scenic::EntityNode* root_node = &test_session->root_node;

  const float viewport_width = display_width / 2;
  const float viewport_height = display_height;
  static constexpr float kCameraOffset = 500;
  // View matrix matches vulkan clip space +Y down, looking in direction of +Z
  const glm::vec3 eye(display_width / 2.f, display_height / 2.f, -kCameraOffset);
  const glm::vec3 look_at(eye + glm::vec3(0, 0, 1));
  static const float up[] = {0, -1, 0};

  scenic::StereoCamera camera(test_session->scene);
  camera.SetTransform(glm::value_ptr(eye), glm::value_ptr(look_at), up);

  glm::mat4 projection =
      glm::perspective(glm::radians(120.f), viewport_width / viewport_height, 0.1f, kCameraOffset);
  // projection = glm::scale(projection, glm::vec3(1.f, -1.f, 1.f));

  // clang-format off
  glm::mat4 clip(1.0f,  0.0f, 0.0f, 0.0f,
                 0.0f, -1.0f, 0.0f, 0.0f,
                 0.0f,  0.0f, 0.5f, 0.0f,
                 0.0f,  0.0f, 0.5f, 1.0f);
  // clang-format on
  projection = clip * projection;

  camera.SetStereoProjection(glm::value_ptr(projection), glm::value_ptr(projection));

  test_session->renderer.SetCamera(camera.id());

  // Configure PoseBuffer

  const size_t kVmoSize = PAGE_SIZE;
  zx_status_t status;

  auto vulkan_queues = scenic_impl::gfx::test::VkSessionTest::CreateVulkanDeviceQueues();
  auto device = vulkan_queues->vk_device();
  auto physical_device = vulkan_queues->vk_physical_device();

  // TODO(SCN-1369): Scenic may use a different set of bits when creating a
  // buffer, resulting in a memory pool mismatch.
  const vk::BufferUsageFlags kUsageFlags =
      vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
      vk::BufferUsageFlagBits::eStorageTexelBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
      vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer;

  auto memory_requirements =
      scenic_impl::gfx::test::VkSessionTest::GetBufferRequirements(device, kVmoSize, kUsageFlags);
  auto memory = scenic_impl::gfx::test::VkSessionTest::AllocateExportableMemory(
      device, physical_device, memory_requirements,
      vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible);

  // If we can't make memory that is both host-visible and device-local, we
  // can't run this test.
  if (!memory) {
    FXL_LOG(INFO) << "Could not find UMA compatible memory pool, aborting test.";
    return;
  }

  zx::vmo pose_buffer_vmo = scenic_impl::gfx::test::VkSessionTest::ExportMemoryAsVmo(
      device, vulkan_queues->dispatch_loader(), memory);

  zx::vmo remote_vmo;
  status = pose_buffer_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &remote_vmo);
  FXL_CHECK(status == ZX_OK);

  zx::time base_time = zx::clock::get_monotonic();
  // Normally the time interval is the period of time between each entry in the
  // pose buffer. In this example we only use one entry so the time interval is
  // pretty meaningless. Set to 1 for simplicity (see ARGO-21).
  zx::duration time_interval(1);
  uint32_t num_entries = 1;

  scenic::Memory mem(session, std::move(remote_vmo), kVmoSize,
                     fuchsia::images::MemoryType::VK_DEVICE_MEMORY);
  scenic::Buffer pose_buffer(mem, 0, kVmoSize);

  camera.SetPoseBuffer(pose_buffer, num_entries, base_time, time_interval);

  // Set up scene.

  static constexpr float kPaneWidth = kCameraOffset / 2.f;
  scenic::Rectangle pane_shape(session, kPaneWidth, kPaneWidth);

  static const int num_panes = 6;

  scenic::Color colors[num_panes] = {
      scenic::Color({255, 0, 0, 255}),    // RED
      scenic::Color({0, 255, 255, 255}),  // CYAN
      scenic::Color({0, 255, 0, 255}),    // GREEN
      scenic::Color({255, 0, 255, 255}),  // MAGENTA
      scenic::Color({0, 0, 255, 255}),    // BLUE
      scenic::Color({255, 255, 0, 255}),  // YELLOW
  };

  static constexpr float kPaneOffset = kPaneWidth / 2;

  glm::vec3 translations[num_panes] = {
      eye + glm::vec3(0, 0, kPaneOffset),   // In front of camera.
      eye + glm::vec3(0, 0, -kPaneOffset),  // Behind camera.
      eye + glm::vec3(-kPaneOffset, 0, 0),  // Left of Camera
      eye + glm::vec3(kPaneOffset, 0, 0),   // Right of camera
      eye + glm::vec3(0, -kPaneOffset, 0),  // Above Camera
      eye + glm::vec3(0, kPaneOffset, 0),   // Below Camera
  };

  static const float pi = glm::pi<float>();
  glm::quat orientations[num_panes] = {
      glm::quat(),  // identity quaternion
      glm::angleAxis(pi, glm::vec3(1, 0, 0)),
      glm::angleAxis(-pi / 2, glm::vec3(0, 1, 0)),
      glm::angleAxis(pi / 2, glm::vec3(0, 1, 0)),
      glm::angleAxis(pi / 2, glm::vec3(1, 0, 0)),
      glm::angleAxis(-pi / 2, glm::vec3(1, 0, 0)),
  };

  for (int i = 0; i < num_panes; i++) {
    scenic::Color color = colors[i];
    glm::vec3 translation = translations[i];
    glm::quat orientation = orientations[i];

    FXL_LOG(ERROR) << "translation: " << glm::to_string(translation);
    FXL_LOG(ERROR) << "orientation: " << glm::to_string(orientation);

    scenic::Material pane_material(session);
    pane_material.SetColor(color.r, color.g, color.b, color.a);
    scenic::ShapeNode pane_shape_node(session);
    pane_shape_node.SetShape(pane_shape);
    pane_shape_node.SetMaterial(pane_material);
    pane_shape_node.SetTranslation(translation.x, translation.y, translation.z);
    pane_shape_node.SetRotation(orientation.x, orientation.y, orientation.z, orientation.w);
    root_node->AddChild(pane_shape_node);
  }

  static const int num_quaternions = 8;

  glm::quat quaternions[num_quaternions] = {
      glm::quat(),                                  // dead ahead
      glm::angleAxis(pi, glm::vec3(0, 0, 1)),       // dead ahead but upside down
      glm::angleAxis(pi, glm::vec3(1, 0, 0)),       // behind around X
      glm::angleAxis(pi, glm::vec3(0, 1, 0)),       // behind around Y
      glm::angleAxis(pi / 2, glm::vec3(0, 1, 0)),   // left
      glm::angleAxis(-pi / 2, glm::vec3(0, 1, 0)),  // right
      glm::angleAxis(pi / 2, glm::vec3(1, 0, 0)),   // up
      glm::angleAxis(-pi / 2, glm::vec3(1, 0, 0)),  // down
  };

  int expected_color_index[num_quaternions] = {0, 0, 1, 1, 2, 3, 4, 5};

  for (int i = 0; i < num_quaternions; i++) {
    // Put pose into pose buffer.
    // Only testing orientation so position is always the origin.
    // Quaternion describes head orientation, so invert it to get a transform
    // that takes you into head space.
    escher::hmd::Pose pose(glm::inverse(quaternions[i]), glm::vec3(0, 0, 0));

    // Use vmo::write here for test simplicity. In a real case the vmo should be
    // mapped into a vmar so we dont need a syscall per write
    zx_status_t status = pose_buffer_vmo.write(&pose, 0, sizeof(escher::hmd::Pose));
    FXL_CHECK(status == ZX_OK);

    Present(session);

    EXPECT_EQ(colors[expected_color_index[i]], TakeScreenshot().ColorAt(0.25, 0.5)) << "i = " << i;
  }
  device.freeMemory(memory);
}

TEST_F(ScenicPixelTest, Opacity) {
  constexpr int kNumTests = 3;

  // We use the same background/foreground color for each test iteration, but
  // vary the opacity.  When the opacity is 0% we expect the pure background
  // color, and when it is 100% we expect the pure foreground color.  When
  // opacity is 50% we expect a blend of the two.
  float opacities[kNumTests] = {0.f, 0.5f, 1.f};
  scenic::Color expected_colors[kNumTests] = {
      {0xff, 0x00, 0xf0, 0xff}, {0x80, 0x80, 0x80, 0xff}, {0x00, 0xff, 0x0f, 0xff}};

  for (int i = 0; i < kNumTests; ++i) {
    scenic::OpacityView view(CreatePresentationContext());

    view.set_background_color(0xff, 0x00, 0xf0);
    view.set_foreground_color(0x00, 0xff, 0x0f);
    view.set_foreground_opacity(opacities[i]);

    RunUntilPresent(&view);
    scenic::Screenshot screenshot = TakeScreenshot();
    ASSERT_FALSE(screenshot.empty());

    // We could assert on each pixel individually, but a histogram might give us
    // a more meaningful failure.
    std::map<scenic::Color, size_t> histogram = screenshot.Histogram();

    EXPECT_GT(histogram[expected_colors[i]], 0u);
    histogram.erase(expected_colors[i]);
    EXPECT_EQ((std::map<scenic::Color, size_t>){}, histogram) << "Unexpected colors";
  }
}

TEST_F(ScenicPixelTest, ViewBoundClipping) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;

  test_session->SetUpCamera().SetProjection(0);

  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();

  scenic::View view(session, std::move(view_token), "ClipView");
  scenic::ViewHolder view_holder(session, std::move(view_holder_token), "ClipViewHolder");

  static const float bmin[3] = {0.f, 0.f, -2.f};
  const float bmax[3] = {display_width / 2, display_height, 1.f};
  static const float imin[3] = {0, 0, 0};
  static const float imax[3] = {0, 0, 0};
  view_holder.SetViewProperties(bmin, bmax, imin, imax);

  // Pane extends all the way across the screen horizontally, but
  // the view is only on the left-hand side of the screen.
  int32_t pane_width = display_width;
  int32_t pane_height = 0.25 * display_height;
  scenic::Rectangle pane_shape(session, pane_width, pane_height);
  scenic::Material pane_material(session);
  pane_material.SetColor(255, 0, 255, 255);  // Magenta.

  scenic::ShapeNode pane_node(session);
  pane_node.SetShape(pane_shape);
  pane_node.SetMaterial(pane_material);
  pane_node.SetTranslation(0.5 * pane_width, 0.5 * display_height, 0);

  // Second pane node should be completely outside the view bounds
  // along the z-axis and get clipped entirely.
  scenic::ShapeNode pane_node2(session);
  pane_node2.SetShape(scenic::Rectangle(session, pane_width, pane_height));
  scenic::Material pane_material2(session);
  pane_material2.SetColor(0, 255, 255, 255);  // Another color.
  pane_node2.SetMaterial(pane_material2);
  pane_node2.SetTranslation(0.5 * pane_width, display_height - 0.5 * pane_height, 3);

  test_session->root_node.Attach(view_holder);
  view.AddChild(pane_node);
  view.AddChild(pane_node2);

  Present(session);
  scenic::Screenshot screenshot = TakeScreenshot();

  scenic::Color unclipped_color = screenshot.ColorAt(0.1, 0.5);
  scenic::Color clipped_color = screenshot.ColorAt(0.6, 0.5);
  scenic::Color clipped_color2 = screenshot.ColorAt(0.1, 0.95);

  // Unclipped color should be magenta, clipped should be black.
  EXPECT_EQ(unclipped_color, scenic::Color(255, 0, 255, 255));
  EXPECT_EQ(clipped_color, scenic::Color(0, 0, 0, 0));

  // For pane2, it should be black as well.
  EXPECT_EQ(clipped_color2, scenic::Color(0, 0, 0, 0));
}

// This unit test verifies the behavior of view bound clipping
// when the view exists under a node that itself has a translation
// applied to it. There are two views with a rectangle in each.
// The first view is under a node that is translated (display_width/2, 0,0).
// The second view is under a node that is placed under the first transform
// node, and then translated again by (0, display_height/2, 0,0). This means
// that what you see on the screen should look like the following:
//
//  xxxxxxxxxxvvvvvvvvvv
//  xxxxxxxxxxvvvvvvvvvv
//  xxxxxxxxxxvvvvvvvvvv
//  xxxxxxxxxxvvvvvvvvvv
//  xxxxxxxxxxvvvvvvvvvv
//  xxxxxxxxxxrrrrrrrrrr
//  xxxxxxxxxxrrrrrrrrrr
//  xxxxxxxxxxrrrrrrrrrr
//  xxxxxxxxxxrrrrrrrrrr
//  xxxxxxxxxxrrrrrrrrrr
//
// Where x refers to empty display pixels.
//       v refers to pixels covered by the first view's bounds.
//       r refers to pixels covered by the second view's bounds.
//
// All of the view bounds are given in local coordinates (so their min-point is
// at (0,0) in the xy plane) which means the test would fail if the bounds were
// not being updated properly to the correct world-space location by the
// transform stack before rendering.
TEST_F(ScenicPixelTest, ViewBoundClippingWithTransforms) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;

  // Initialize second session
  auto unique_session_2 = std::make_unique<scenic::Session>(scenic_.get());
  auto session2 = unique_session_2.get();
  session2->set_error_handler([this](zx_status_t status) {
    FXL_LOG(ERROR) << "Session terminated.";
    QuitLoop();
  });

  // Initialize third session
  auto unique_session_3 = std::make_unique<scenic::Session>(scenic_.get());
  auto session3 = unique_session_3.get();
  session3->set_error_handler([this](zx_status_t status) {
    FXL_LOG(ERROR) << "Session terminated.";
    QuitLoop();
  });

  test_session->SetUpCamera().SetProjection(0);

  // Add a transform node anchored in the top-middle of the display
  // along the x-axis and at the top with respect to the y-axis.
  scenic::EntityNode transform_node(session);
  transform_node.SetTranslation(display_width / 2, 0, 0);

  // Create a second transform node and add it as a child to the first transform
  // node.
  scenic::EntityNode transform_node_2(session);
  transform_node_2.SetTranslation(0, display_height / 2, 0);
  transform_node.AddChild(transform_node_2);

  // Add the transform node as a child of the root node.
  test_session->root_node.AddChild(transform_node);

  // Create two sets of view/view-holder token pairs.
  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  auto [view_token_2, view_holder_token_2] = scenic::ViewTokenPair::New();

  scenic::View view(session2, std::move(view_token), "ClipView");
  scenic::ViewHolder view_holder(session, std::move(view_holder_token), "ClipViewHolder");

  scenic::View view2(session3, std::move(view_token_2), "ClipView2");
  scenic::ViewHolder view_holder2(session, std::move(view_holder_token_2), "ClipViewHolder2");

  // Bounds of each view should be the size of a quarter of the display with
  // origin at 0,0 relative to its transform node.
  const float bmin[3] = {0.f, 0.f, -2.f};
  const float bmax[3] = {display_width / 2, display_height / 2, 1.f};
  const float imin[3] = {0, 0, 0};
  const float imax[3] = {0, 0, 0};
  view_holder.SetViewProperties(bmin, bmax, imin, imax);
  view_holder2.SetViewProperties(bmin, bmax, imin, imax);

  // Pane extends across the entire right-side of the display, even though
  // its containing view is only in the top-right corner.
  int32_t pane_width = display_width / 2;
  int32_t pane_height = display_height;
  scenic::Rectangle pane_shape(session2, pane_width, pane_height);
  scenic::Rectangle pane_shape2(session3, pane_width, pane_height);

  // Make two pane materials
  scenic::Material pane_material(session2);
  pane_material.SetColor(255, 0, 255, 255);  // Magenta.

  scenic::Material pane_material2(session3);
  pane_material2.SetColor(0, 255, 255, 255);  // Cyan

  scenic::ShapeNode pane_node(session2);
  pane_node.SetShape(pane_shape);
  pane_node.SetMaterial(pane_material);
  pane_node.SetTranslation(pane_width / 2, pane_height / 2, 0);

  scenic::ShapeNode pane_node2(session3);
  pane_node2.SetShape(pane_shape2);
  pane_node2.SetMaterial(pane_material2);

  // Pane node 2 improperly extends above view2's bounds in the y-axis,
  // overlapping with view1, but should still be clipped.
  pane_node2.SetTranslation(pane_width / 2, 0, 0);

  // Add first view holder to the first transform.
  transform_node.Attach(view_holder);
  view.AddChild(pane_node);

  // Add the second view holder to the second transform.
  transform_node_2.Attach(view_holder2);
  view2.AddChild(pane_node2);

  Present(session);
  Present(session2);
  Present(session3);

  scenic::Screenshot screenshot = TakeScreenshot();

  scenic::Color magenta_color = screenshot.ColorAt(0.6, 0.1);
  scenic::Color magenta_color2 = screenshot.ColorAt(0.9, 0.4);
  scenic::Color cyan_color = screenshot.ColorAt(0.6, 0.9);
  scenic::Color black_color = screenshot.ColorAt(0.0, 0.5);

  // Upper-right quadrant should be magenta, lower-right quadrant
  // should be cyan. The left half of the screen should be black.
  EXPECT_EQ(magenta_color, scenic::Color(255, 0, 255, 255));
  EXPECT_EQ(magenta_color2, scenic::Color(255, 0, 255, 255));
  EXPECT_EQ(cyan_color, scenic::Color(0, 255, 255, 255));
  EXPECT_EQ(black_color, scenic::Color(0, 0, 0, 0));
}

// Creates three views and renders their wireframe bounds.
// Looks like this:
//
// aaaaaaaaaabbbbbbbbbb
// a        ab        b
// a        ab        b
// a        abbbbbbbbbb
// a        acccccccccc
// a        ac        c
// a        ac        c
// aaaaaaaaaacccccccccc
//
// Where a,b, and c represent the bounds for views 1,2, and
// 3 respectively.
TEST_F(ScenicPixelTest, ViewBoundWireframeRendering) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;
  scenic::EntityNode* root_node = &test_session->root_node;
  test_session->SetUpCamera().SetProjection(0);

  // Initialize session 2.
  auto unique_session2 = std::make_unique<scenic::Session>(scenic_.get());
  auto session2 = unique_session2.get();
  session2->set_error_handler([this](zx_status_t status) {
    FXL_LOG(ERROR) << "Session terminated.";
    QuitLoop();
  });

  // Initialize session 3.
  auto unique_session3 = std::make_unique<scenic::Session>(scenic_.get());
  auto session3 = unique_session3.get();
  session3->set_error_handler([this](zx_status_t status) {
    FXL_LOG(ERROR) << "Session terminated.";
    QuitLoop();
  });

  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();
  auto [view_token2, view_holder_token2] = scenic::ViewTokenPair::New();
  auto [view_token3, view_holder_token3] = scenic::ViewTokenPair::New();

  scenic::View view(session, std::move(view_token), "ClipView");
  scenic::ViewHolder view_holder(session, std::move(view_holder_token), "ClipViewHolder");

  // View 2 is embedded by view 1.
  scenic::View view2(session2, std::move(view_token2), "ClipView2");
  scenic::ViewHolder view_holder2(session, std::move(view_holder_token2), "ClipViewHolder2");

  // View 3 is embedded by view 2 and thus doubly embedded within view 1.
  scenic::View view3(session3, std::move(view_token3), "ClipView3");
  scenic::ViewHolder view_holder3(session2, std::move(view_holder_token3), "ClipViewHolder3");

  const float bmin[3] = {0.f, 0.f, -2.f};
  const float bmax[3] = {display_width / 2, display_height, 1.f};
  const float imin[3] = {1, 1, 0};
  const float imax[3] = {1, 1, 0};
  view_holder.SetViewProperties(bmin, bmax, imin, imax);

  const float bmin2[3] = {0, 0, -2.f};
  const float bmax2[3] = {display_width / 2, display_height / 2, 1.f};
  view_holder2.SetViewProperties(bmin2, bmax2, imin, imax);
  view_holder3.SetViewProperties(bmin2, bmax2, imin, imax);

  // Set the debug bounds colors.
  view_holder.SetDebugBoundsColor(0, 255, 255);
  view_holder2.SetDebugBoundsColor(255, 0, 255);
  view_holder3.SetDebugBoundsColor(255, 255, 0);

  // Set bounds rendering on just the first view. This should turn on debug
  // wireframe for itself and view2, since view2 is a direct embedding. View3
  // should still be off.
  view.enableDebugBounds(true);

  root_node->Attach(view_holder);

  // Transform and embed view holder 2 in first view.
  scenic::EntityNode transform_node(session);
  transform_node.SetTranslation(display_width / 2, 0, 0);
  view.AddChild(transform_node);
  transform_node.Attach(view_holder2);

  // Transform and embed view holder 3 in view 2.
  scenic::EntityNode transform_node2(session2);
  transform_node2.SetTranslation(0, display_height / 2, 0);
  view2.AddChild(transform_node2);
  transform_node2.Attach(view_holder3);

  Present(session);
  Present(session2);
  Present(session3);

  // Take screenshot.
  scenic::Screenshot screenshot = TakeScreenshot();
  ASSERT_FALSE(screenshot.empty());
  auto histogram = screenshot.Histogram();

  histogram.erase({0, 0, 0, 0});
  scenic::Color expected_colors[2] = {{0, 255, 255, 255},   // First ViewHolder
                                      {255, 0, 255, 255}};  // Second ViewHolder
  for (uint32_t i = 0; i < 2; i++) {
    EXPECT_GT(histogram[expected_colors[i]], 0u);
    histogram.erase(expected_colors[i]);
  }
  EXPECT_EQ((std::map<scenic::Color, size_t>){}, histogram) << "Unexpected colors";

  // Now toggle debug rendering for view 2. This should tirgger view3's bounds to
  // display as view3 is directly embedded by view2.
  view2.enableDebugBounds(true);

  Present(session);
  Present(session2);
  Present(session3);

  // Take screenshot.
  scenic::Screenshot screenshot2 = TakeScreenshot();
  ASSERT_FALSE(screenshot2.empty());
  histogram = screenshot2.Histogram();

  histogram.erase({0, 0, 0, 0});
  scenic::Color expected_colors_2[3] = {{0, 255, 255, 255},   // First ViewHolder
                                        {255, 0, 255, 255},   // Second ViewHolder
                                        {255, 255, 0, 255}};  // Third ViewHolder
  for (uint32_t i = 0; i < 3; i++) {
    EXPECT_GT(histogram[expected_colors_2[i]], 0u);
    histogram.erase(expected_colors_2[i]);
  }
  EXPECT_EQ((std::map<scenic::Color, size_t>){}, histogram) << "Unexpected colors";
}

// TODO(SCN-1375): Blocked against hardware inability
// to provide accurate screenshots from the physical
// display. Our "TakeScreenshot()" method only grabs
// pixel data from Escher before it gets sent off to
// the display controller and thus cannot accurately
// capture color conversion information.
TEST_F(ScenicPixelTest, DISABLED_Compositor) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;
  scenic::EntityNode* root_node = &test_session->root_node;

  test_session->SetUpCamera().SetProjection(0);

  // Color correction data
  static const std::array<float, 3> preoffsets = {0, 0, 0};
  static const std::array<float, 9> matrix = {.288299,  0.052709, -0.257912, 0.711701, 0.947291,
                                              0.257912, 0.000000, -0.000000, 1.000000};
  static const std::array<float, 3> postoffsets = {0, 0, 0};

  static const glm::mat4 glm_matrix(.288299, 0.052709, -0.257912, 0.00000, 0.711701, 0.947291,
                                    0.257912, 0.00000, 0.000000, -0.000000, 1.000000, 0.00000,
                                    0.000000, 0.000000, 0.00000, 1.00000);

  const float pane_width = display_width / 5;
  const float pane_height = display_height;

  static const float colors[15] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0};

  for (uint32_t i = 0; i < 5; i++) {
    scenic::Rectangle pane_shape(session, pane_width, pane_height);
    scenic::Material pane_material(session);
    pane_material.SetColor(255 * colors[3 * i], 255 * colors[3 * i + 1], 255 * colors[3 * i + 2],
                           255);

    scenic::ShapeNode pane_node(session);
    pane_node.SetShape(pane_shape);
    pane_node.SetMaterial(pane_material);
    pane_node.SetTranslation((i + 0.5) * pane_width, 0.5 * pane_height, -20);
    root_node->AddChild(pane_node);
  }

  // Display uncorrected version first.
  Present(session);
  scenic::Screenshot prev_screenshot = TakeScreenshot();

  // Apply color correction.
  test_session->compositor.SetColorConversion(preoffsets, matrix, postoffsets);

  // Display color corrected version.
  Present(session, zx::time(1000000));
  scenic::Screenshot post_screenshot = TakeScreenshot();

  for (uint32_t i = 0; i < 5; i++) {
    scenic::Color prev_color = prev_screenshot.ColorAt(i * .2, 0.5);
    scenic::Color post_color = post_screenshot.ColorAt(i * .2, 0.5);

    glm::vec4 vec = glm_matrix * glm::vec4(prev_color.r, prev_color.g, prev_color.b, 1);
    scenic::Color res(vec.x, vec.y, vec.z, vec.w);
    EXPECT_EQ(res, post_color);
  }
}

// This test sets up a scene, takes a screenshot, rotates display configuration
// by 90 degrees and takes a second screenshot, then makes sure that the pixels
// in both screenshots map onto each other how you would expect.
TEST_F(ScenicPixelTest, RotationTest) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;
  scenic::EntityNode* root_node = &test_session->root_node;

  test_session->SetUpCamera().SetProjection(0);

  const float pane_width = display_width / 5;
  const float pane_height = display_height;

  // For this test, create 5 vertical bands. This is an array of
  // the rgb colors for each of the five bands that will be
  // created below.
  static const float colors[15] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0};

  for (uint32_t i = 0; i < 5; i++) {
    scenic::Rectangle pane_shape(session, pane_width, pane_height);
    scenic::Material pane_material(session);
    pane_material.SetColor(255 * colors[3 * i], 255 * colors[3 * i + 1], 255 * colors[3 * i + 2],
                           255);

    scenic::ShapeNode pane_node(session);
    pane_node.SetShape(pane_shape);
    pane_node.SetMaterial(pane_material);
    pane_node.SetTranslation((i + 0.5) * pane_width, 0.5 * pane_height, -20);
    root_node->AddChild(pane_node);
  }

  // Display unrotated version first.
  Present(session);
  scenic::Screenshot prev_screenshot = TakeScreenshot();

  test_session->compositor.SetLayoutRotation(90);

  // Display rotated version.
  Present(session, zx::time(1000000));
  scenic::Screenshot post_screenshot = TakeScreenshot();

  // The pre and post width and height should be the reverse of each other.
  EXPECT_EQ(prev_screenshot.width(), post_screenshot.height());
  EXPECT_EQ(prev_screenshot.height(), post_screenshot.width());

  // All of the colors should be transposed.
  for (uint32_t x = 0; x < prev_screenshot.width(); x++) {
    for (uint32_t y = 0; y < prev_screenshot.height(); y++) {
      EXPECT_EQ(prev_screenshot[y][x], post_screenshot[x][y]);
    }
  }
}

// Test to make sure scenic can properly render basic shapes like circles.
TEST_F(ScenicPixelTest, BasicShapeTest) {
  auto test_session = SetUpTestSession();
  scenic::Session* const session = &test_session->session;
  const auto [display_width, display_height] = test_session->display_dimensions;
  scenic::EntityNode* root_node = &test_session->root_node;

  test_session->SetUpCamera().SetProjection(0);

  const float kRadius = 10;

  scenic::Circle circle_shape(session, kRadius);
  scenic::Material circle_material(session);
  circle_material.SetColor(255, 0, 255, 255);

  scenic::ShapeNode circle_node(session);
  circle_node.SetShape(circle_shape);
  circle_node.SetMaterial(circle_material);
  circle_node.SetTranslation(display_width / 2, display_height / 2, -20);
  root_node->AddChild(circle_node);

  Present(session);
  scenic::Screenshot screenshot = TakeScreenshot();
  EXPECT_EQ(screenshot.ColorAt(0.5, 0.5), scenic::Color(255, 0, 255, 255));
}

}  // namespace
