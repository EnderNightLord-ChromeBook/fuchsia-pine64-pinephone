// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_PAPER_PAPER_RENDERER_H_
#define SRC_UI_LIB_ESCHER_PAPER_PAPER_RENDERER_H_

#include "src/lib/fxl/logging.h"
#include "src/ui/lib/escher/debug/debug_rects.h"
#include "src/ui/lib/escher/paper/paper_draw_call_factory.h"
#include "src/ui/lib/escher/paper/paper_drawable.h"
#include "src/ui/lib/escher/paper/paper_light.h"
#include "src/ui/lib/escher/paper/paper_readme.h"
#include "src/ui/lib/escher/paper/paper_render_queue.h"
#include "src/ui/lib/escher/paper/paper_renderer_config.h"
#include "src/ui/lib/escher/paper/paper_shape_cache.h"
#include "src/ui/lib/escher/paper/paper_transform_stack.h"
#include "src/ui/lib/escher/renderer/renderer.h"
#include "src/ui/lib/escher/renderer/uniform_binding.h"

namespace escher {

// |PaperRenderer| provides a convenient and flexible interface for rendering
// shapes in a 3D space, as required by Scenic.  Clients achieve this primarily
// by passing instances of |PaperDrawable| to the |Draw()| method, using either
// pre-existing drawable types or their own subclasses.  For convenience, other
// drawing methods are provided, such as |DrawCircle()|.
//
// These draw methods are legal only between |BeginFrame()| and |EndFrame()|.
// Respectively, these two methods prepare the renderer to render a frame, and
// generate the Vulkan commands which actually perform the rendering.
//
// All other public methods must *not* be called between |BeginFrame()| and
// |EndFrame()|.  For example, |SetConfig()| can be used to choose a different
// shadow algorithm; changing this during the frame would cause incompatibility
// between the |PaperDrawCalls| previously and subsequently enqueued by the
// |PaperDrawCallFactory|.
//
// Implementation details follow...
//
// |PaperRenderer| is responsible for coordinating its sub-components:
//   - |PaperDrawCallFactory|
//   - |PaperShapeCache|
//   - |PaperRenderQueue|
// See their class comments for details.
//
// Clients call |SetConfig()| to specify the coordination policies that will be
// used to render subsequent frames.  When the config changes, the renderer
// applies the appropriate changes to its sub-components.
//
// When |BeginFrame()| is called, each sub-component is made ready to render the
// new frame.  This depends on both the policies specified by |SetConfig()|, as
// well as the |PaperScene|, |Camera|, and |output_image| parameters.  Together,
// these determine how:
//   - shader data is encoded in the draw calls built by |PaperDrawCallFactory|
//   - tessellated meshes are post-processed before they are cached/uploaded
// ... and so forth.
//
// During |EndFrame()| the renderer first builds |RenderPassInfo| descriptions
// of the Vulkan render passes necessary to render the scene.  During each of
// these render passes, the renderer directs the render-queue to iterate over
// its draw calls and emit Vulkan commands into a |CommandBuffer|.  This is
// controlled by two parameters passed to the queue:
//   - |PaperRenderQueueFlags|, to control iteration over draw calls.
//   - |PaperRenderQueueContext|, used by draw calls to emit Vulkan commands.
class PaperRenderer final : public Renderer {
 public:
  static PaperRendererPtr New(EscherWeakPtr escher,
                              const PaperRendererConfig& config = {
                                  .shadow_type = PaperRendererShadowType::kNone});
  ~PaperRenderer() override;

  // Set configuration parameters which affect how the renderer will render
  // subsequent frames.
  void SetConfig(const PaperRendererConfig& config);
  const PaperRendererConfig& config() const { return config_; }

  // Does the renderer support the specified shadow type?
  bool SupportsShadowType(PaperRendererShadowType shadow_type) const;

  // Configures the renderer to render a frame into |output_image|.  The
  // renderer configures its sub-components to render the frame based on the
  // |scene| and |camera| parameters, along with the configuration params
  // previously set by |SetConfig()|.
  //
  // |PaperScene| describes aspects of the scene that affect the appearance of
  // scene object (e.g. lighting parameters), but does not provide the list of
  // scene objects to be rendered.  To render the scene, clients should follow
  // these steps:
  //   - |BeginFrame()|
  //   - |Draw()| each object in the scene.
  //   - |EndFrame()| emits the Vulkan commands that actually render the scene.
  //
  // Multiple cameras are supported, each rendering into its own viewport.
  // However, the position of the first camera is the one used for depth-sorting
  // the scene contents.  For use-cases such as stereo rendering this is not a
  // problem, however there can be problems with e.g. translucent objects if two
  // cameras have dramatically different positions.
  void BeginFrame(const FramePtr& frame, const PaperScenePtr& scene,
                  const std::vector<Camera>& cameras, const escher::ImagePtr& output_image);

  // See |BeginFrame()|.  After telling the renderer to draw the scene content,
  // |EndFrame()| emits commands into a Vulkan command buffer.  Submitting this
  // command buffer causes the scene to be rendered into |output_image|.
  void EndFrame();

  // The following methods may only be used between during a frame, i.e. between
  // calls to |BeginFrame()| and |EndFrame()|.

  // Return the transform stack, which affects the transform and clipping that
  // is applied to subsequently-drawn |PaperDrawables|.
  PaperTransformStack* transform_stack() {
    FXL_DCHECK(frame_data_) << "transform_stack only accessible during frame.";
    return &transform_stack_;
  }

  // Invokes DrawInScene() on the drawable object to generate and enqueue the
  // draw-calls that be transformed into Vulkan commands during EndFrame().
  void Draw(PaperDrawable* drawable, PaperDrawableFlags flags = {});

  // Draw predefined shapes: circles, rectangles, and rounded-rectangles.
  // Generates and enqueues draw-calls that will emit Vulkan commands during
  // EndFrame().
  void DrawCircle(float radius, const PaperMaterialPtr& material, PaperDrawableFlags flags = {});
  void DrawRect(vec2 min, vec2 max, const PaperMaterialPtr& material,
                PaperDrawableFlags flags = {});

  // Convenience function for the above DrawRect function that takes in the width/ height
  // of the rect and centers it at (0,0).
  void DrawRect(float width, float height, const PaperMaterialPtr& material,
                PaperDrawableFlags flags = {});

  void DrawRoundedRect(const RoundedRectSpec& spec, const PaperMaterialPtr& material,
                       PaperDrawableFlags flags = {});
  void DrawBoundingBox(const BoundingBox& box, const PaperMaterialPtr& material,
                       PaperDrawableFlags flags = {});
  void DrawMesh(const MeshPtr& mesh, const PaperMaterialPtr& material,
                PaperDrawableFlags flags = {});

  // TODO(ES-203) - We will remove this once PaperDrawCallFactory becomes
  // injectable. We should never have to access this directly from the
  // renderer - it should be completely opaque.
  PaperDrawCallFactory* draw_call_factory() { return &draw_call_factory_; }

  // Draws debug text on top of output image.
  void DrawDebugText(std::string text, vk::Offset2D offset, int32_t scale);

  // Draws vertical line to the output image. The entire line will be to the right of |x_coord|.
  void DrawVLine(DebugRects::Color kColor, uint32_t x_coord, int32_t y_start, uint32_t y_end,
                 uint32_t thickness);

  // Draws horizontal line to the output image. The entire line will be below |y_coord|.
  void DrawHLine(DebugRects::Color kColor, int32_t y_coord, int32_t x_start, uint32_t x_end,
                 int32_t thickness);

  // Draws a graph onto the screen using DrawDebugText and Draw Line calls. The graph corners are:
  // (150, 100)                                          (width - 150, 100)
  //
  // (150, height - 100)                        (width - 150, height - 100)
  void DrawDebugGraph(std::string x_label, std::string y_label, DebugRects::Color lineColor);

  // Corresponds to FrameTimings::Timestamps and will be used to calculate values to graph.
  struct TimeStamp {
    int16_t latch_point;
    int16_t update_done;
    int16_t render_start;
    int16_t render_done;
    int16_t target_present;
    int16_t actual_present;
  };

  // Used to calculate the area of the debug graph that bars will be drawn in.
  const static int32_t kHeightPadding = 100;

  // Add TimeStamp to be graphed.
  void AddDebugTimeStamp(TimeStamp ts);

 private:
  explicit PaperRenderer(EscherWeakPtr escher, const PaperRendererConfig& config);
  PaperRenderer(const PaperRenderer&) = delete;

  // Store relevant info from cameras passed to BeginFrame().
  struct CameraData {
    UniformBinding binding;
    vk::Rect2D rect;
    vk::Viewport viewport;
    uint32_t eye_index;  // For PaperShaderPushConstants.
  };

  // Store relevant info about text to draw to the output image.
  struct TextData {
    std::string text;
    vk::Offset2D offset;
    int32_t scale;
  };

  // Store relevant info about lines to draw to the output image.
  struct LineData {
    DebugRects::Color kColor;
    vk::Rect2D rect;
  };

  // Stores all per-frame data in one place.
  struct FrameData {
    FrameData(const FramePtr& frame, const PaperScenePtr& scene, const ImagePtr& output_image,
              std::pair<TexturePtr, TexturePtr> depth_and_msaa_textures,
              const std::vector<Camera>& cameras);
    ~FrameData();
    FramePtr frame;
    PaperScenePtr scene;
    ImagePtr output_image;
    TexturePtr depth_texture;
    TexturePtr msaa_texture;

    size_t num_lights;

    std::vector<CameraData> cameras;

    std::vector<TextData> texts;
    std::vector<LineData> lines;

    // UniformBindings returned by PaperDrawCallFactory::BeginFrame().  These
    // contain camera and lighting parameters that are shared between draw
    // calls.  The contents are opaque to the PaperRenderer, who trusts that
    // the PaperDrawCallFactory will generate DrawCalls that are compatible with
    // these UniformBindings.
    std::vector<UniformBinding> scene_uniform_bindings;

    std::unique_ptr<BatchGpuUploader> gpu_uploader;
  };

  // Called in BeginFrame() to obtain suitable render targets.
  // NOTE: call only once per frame.
  std::pair<TexturePtr, TexturePtr> ObtainDepthAndMsaaTextures(const FramePtr& frame,
                                                               const ImageInfo& info);

  // Called during EndFrame().
  void BindSceneAndCameraUniforms(uint32_t camera_index);
  void GenerateCommandsForNoShadows(uint32_t camera_index);
  void GenerateCommandsForShadowVolumes(uint32_t camera_index);
  static void InitRenderPassInfo(RenderPassInfo* render_pass_info, ResourceRecycler* recycler,
                                 const FrameData& frame_data, uint32_t camera_index);

  // Called to write text onto screen
  void GenerateDebugCommands(CommandBuffer* cmd_buf);

  // Loops through debug_times_ array to draw data to the graph.

  // Red      render_time         Blue     random_value
  // Yellow   random_value        Purple   presentation_time
  void GraphDebugData();

  // Called when |config_.debug_frame_number| is true.  Uses |debug_font_| to
  // blit the current frame number to the output image.
  void RenderFrameCounter();

  PaperRendererConfig config_;

  PaperDrawCallFactory draw_call_factory_;
  PaperRenderQueue render_queue_;
  PaperShapeCache shape_cache_;
  PaperTransformStack transform_stack_;

  std::vector<TexturePtr> depth_buffers_;
  std::vector<TexturePtr> msaa_buffers_;

  std::unique_ptr<FrameData> frame_data_;

  ShaderProgramPtr ambient_light_program_;
  ShaderProgramPtr no_lighting_program_;
  ShaderProgramPtr point_light_program_;
  ShaderProgramPtr point_light_falloff_program_;
  ShaderProgramPtr shadow_volume_geometry_program_;
  ShaderProgramPtr shadow_volume_geometry_debug_program_;
  ShaderProgramPtr shadow_volume_lighting_program_;

  std::unique_ptr<DebugFont> debug_font_;
  std::unique_ptr<DebugRects> debug_lines_;

  // A list of TimeStamps where each cell represents the data we want to
  // display on the graph for each frame.
  std::vector<TimeStamp> debug_times_;
};

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_PAPER_PAPER_RENDERER_H_
