// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_VULKAN_TESTS_VKLATENCY_SWAPCHAIN_H_
#define GARNET_LIB_VULKAN_TESTS_VKLATENCY_SWAPCHAIN_H_

#include <lib/zx/channel.h>

#include <optional>
#include <vector>

#include "src/lib/fxl/macros.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "third_party/skia/include/gpu/GrContext.h"

#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

namespace examples {

class Swapchain {
 public:
  explicit Swapchain(bool protected_output);
  ~Swapchain();

  typedef struct {
    uint32_t index;
    vk::Image image;
    vk::ImageLayout layout;
    vk::Semaphore render_semaphore;
    vk::Semaphore present_semaphore;
    vk::CommandBuffer post_raster_command_buffer;
  } SwapchainImageResources;

  bool Initialize(zx::channel image_pipe_endpoint, std::optional<vk::Extent2D> surface_size);
  uint32_t GetNumberOfSwapchainImages();
  vk::Extent2D GetImageSize();
  GrContext* GetGrContext();
  SwapchainImageResources* GetCurrentImageResources();
  void SwapImages();
  bool protected_output() const { return protected_output_; }

 private:
  bool GetPhysicalDevice();
  bool CreateSurface(zx::channel image_pipe_endpoint, std::optional<vk::Extent2D> surface_size);
  bool CreateDeviceAndQueue();
  bool InitializeSwapchain();
  bool PrepareBuffers();
  void AcquireNextImage();

  vk::Instance vk_instance_;
  vk::PhysicalDevice vk_physical_device_;
  vk::SurfaceKHR surface_;
  vk::Extent2D max_image_extent_;
  vk::Device vk_device_;
  vk::Queue graphics_queue_;
  vk::CommandPool command_pool_;
  vk::SwapchainKHR swapchain_;
  vk::Semaphore next_present_semaphore_;
  vk::Fence fence_;
  uint32_t graphics_queue_family_index_ = 0;
  vk::Format format_ = vk::Format::eB8G8R8A8Unorm;

  sk_sp<GrContext> gr_context_;

  std::vector<SwapchainImageResources> swapchain_image_resources_;
  uint32_t desired_image_count_ = 2;
  uint32_t current_image_ = 0;
  bool protected_output_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Swapchain);
};

}  // namespace examples

#endif  // GARNET_LIB_VULKAN_TESTS_VKLATENCY_SWAPCHAIN_H_
