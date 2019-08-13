// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "utils.h"
#include "vulkan_command_buffers.h"
#include "vulkan_command_pool.h"
#include "vulkan_framebuffer.h"
#include "vulkan_graphics_pipeline.h"
#include "vulkan_instance.h"
#include "vulkan_layer.h"
#include "vulkan_logical_device.h"
#include "vulkan_physical_device.h"
#include "vulkan_render_pass.h"
#include "vulkan_surface.h"
#include "vulkan_swapchain.h"
#include "vulkan_sync.h"

#if USE_GLFW
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

bool DrawFrame(const VulkanLogicalDevice& logical_device, const VulkanSync& sync,
               const VulkanSwapchain& swap_chain, const VulkanCommandBuffers& command_buffers);

void glfwErrorCallback(int error, const char* description) {
  fprintf(stderr, "glfwErrorCallback: %d : %s\n", error, description);
}

int main() {
  // INSTANCE
  const bool kEnableValidation = true;
  auto instance = std::make_shared<VulkanInstance>();
#if USE_GLFW
  glfwInit();
  glfwSetErrorCallback(glfwErrorCallback);
  if (!glfwVulkanSupported()) {
    RTN_MSG(1, "glfwVulkanSupported has returned false.\n");
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(1024, 768, "VkPrimer", nullptr, nullptr);
  if (!window) {
    RTN_MSG(1, "glfwCreateWindow failed.\n");
  }
  if (!instance->Init(kEnableValidation, window)) {
    RTN_MSG(1, "Instance Initialization Failed.\n");
  }
#else
  if (!instance->Init(kEnableValidation)) {
    RTN_MSG(1, "Instance Initialization Failed.\n");
  }
#endif

  // LAYERS
  VulkanLayer vulkan_layer(instance);
  if (!vulkan_layer.Init()) {
    RTN_MSG(1, "Layer Initialization Failed.\n");
  }

  // SURFACE
#if USE_GLFW
  auto surface = std::make_shared<VulkanSurface>(instance, window);
#else
  auto surface = std::make_shared<VulkanSurface>(instance);
#endif
  if (!surface->Init()) {
    RTN_MSG(1, "Surface Initialization Failed.\n");
  }

  // PHYSICAL DEVICE
  VulkanPhysicalDevice physical_device(instance, surface->surface());
  if (!physical_device.Init()) {
    RTN_MSG(1, "Phys Device Initialization Failed.\n");
  }

  // LOGICAL DEVICE
  auto logical_device = std::make_shared<VulkanLogicalDevice>(
      physical_device.phys_device(), surface->surface(), kEnableValidation);
  if (!logical_device->Init()) {
    RTN_MSG(1, "Logical Device Initialization Failed.\n");
  }

  // SWAP CHAIN
  auto swap_chain =
      std::make_shared<VulkanSwapchain>(physical_device.phys_device(), logical_device, surface);
  if (!swap_chain->Init()) {
    RTN_MSG(1, "Swap Chain Initialization Failed.\n");
  }

  // RENDER PASS
  auto render_pass = std::make_shared<VulkanRenderPass>(logical_device, swap_chain->image_format());
  if (!render_pass->Init()) {
    RTN_MSG(1, "Render Pass Initialization Failed.\n");
  }

  // GRAPHICS PIPELINE
  auto graphics_pipeline =
      std::make_unique<VulkanGraphicsPipeline>(logical_device, swap_chain->extent(), render_pass);
  if (!graphics_pipeline->Init()) {
    RTN_MSG(1, "Graphics Pipeline Initialization Failed.\n");
  }

  // FRAMEBUFFER
  auto framebuffer =
      std::make_unique<VulkanFramebuffer>(logical_device, swap_chain, *render_pass->render_pass());
  if (!framebuffer->Init()) {
    RTN_MSG(1, "Framebuffer Initialization Failed.\n");
  }

  // COMMAND POOL
  auto command_pool = std::make_shared<VulkanCommandPool>(
      logical_device, physical_device.phys_device(), surface->surface());
  if (!command_pool->Init()) {
    RTN_MSG(1, "Command Pool Initialization Failed.\n");
  }

  // COMMAND BUFFER
  auto command_buffers = std::make_unique<VulkanCommandBuffers>(
      logical_device, command_pool, *framebuffer, swap_chain->extent(), *render_pass->render_pass(),
      *graphics_pipeline->graphics_pipeline());
  if (!command_buffers->Init()) {
    RTN_MSG(1, "Command Buffer Initialization Failed.\n");
  }

  // SYNC
  auto sync = std::make_unique<VulkanSync>(logical_device, 3 /* max_frames_in_flight */);
  if (!sync->Init()) {
    RTN_MSG(1, "Sync Initialization Failed.\n");
  }

#if USE_GLFW
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    DrawFrame(*logical_device, *sync, *swap_chain, *command_buffers);
  }
#else
  DrawFrame(*logical_device, *sync, *swap_chain, *command_buffers);
  sleep(3);
#endif
  logical_device->device()->waitIdle();

#if USE_GLFW
  glfwDestroyWindow(window);
  glfwTerminate();
#endif

  return 0;
}

bool DrawFrame(const VulkanLogicalDevice& logical_device, const VulkanSync& sync,
               const VulkanSwapchain& swap_chain, const VulkanCommandBuffers& command_buffers) {
  static int current_frame = 0;

  // Compact variables for readability derived from |current_frame|.
  const vk::Device& device = *logical_device.device();

  const vk::Fence& fence = *(sync.in_flight_fences()[current_frame]);

  const vk::Semaphore& image_available_semaphore =
      *(sync.image_available_semaphores()[current_frame]);

  const vk::Semaphore& render_finished_semaphore =
      *(sync.render_finished_semaphores()[current_frame]);

  // Wait for any outstanding command buffers to be processed.
  device.waitForFences({fence}, VK_TRUE, std::numeric_limits<uint64_t>::max());
  device.resetFences({fence});

  // Obtain next swap chain image in which to draw.
  auto [result, image_index] =
      device.acquireNextImageKHR(*swap_chain.swap_chain(), std::numeric_limits<uint64_t>::max(),
                                 image_available_semaphore, nullptr);
  if (vk::Result::eSuccess != result) {
    RTN_MSG(false, "VK Error: 0x%x - Failed to acquire swap chain image.", result);
  }

  // Define stage that |image_available_semaphore| is waiting on.
  const vk::PipelineStageFlags image_available_wait_stage =
      vk::PipelineStageFlagBits::eColorAttachmentOutput;

  vk::CommandBuffer command_buffer = *(command_buffers.command_buffers()[image_index]);

  vk::SubmitInfo submit_info;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &image_available_semaphore;
  submit_info.pWaitDstStageMask = &image_available_wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &render_finished_semaphore;

  if (logical_device.queue().submit(1, &submit_info, fence) != vk::Result::eSuccess) {
    RTN_MSG(false, "Failed to submit draw command buffer.\n");
  }

  vk::PresentInfoKHR present_info;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &render_finished_semaphore;
  present_info.swapchainCount = 1;
  present_info.setPSwapchains(&(*swap_chain.swap_chain()));
  present_info.pImageIndices = &image_index;

  logical_device.queue().presentKHR(&present_info);

  current_frame = (current_frame + 1) % sync.max_frames_in_flight();

  return true;
}
