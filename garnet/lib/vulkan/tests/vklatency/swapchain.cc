// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/lib/vulkan/tests/vklatency/swapchain.h"

#include "src/lib/fxl/logging.h"
#include "third_party/skia/include/gpu/GrTypes.h"
#include "third_party/skia/include/gpu/vk/GrVkBackendContext.h"

namespace examples {

namespace {

constexpr uint32_t kVulkanApiVersion = VK_MAKE_VERSION(1, 1, 0);

vk::PipelineStageFlags GetPipelineStageFlags(vk::ImageLayout layout) {
  switch (layout) {
    case vk::ImageLayout::eUndefined:
      return vk::PipelineStageFlagBits::eTopOfPipe;
    case vk::ImageLayout::eGeneral:
      return vk::PipelineStageFlagBits::eAllCommands;
    case vk::ImageLayout::ePreinitialized:
      return vk::PipelineStageFlagBits::eHost;
    case vk::ImageLayout::eTransferSrcOptimal:
    case vk::ImageLayout::eTransferDstOptimal:
      return vk::PipelineStageFlagBits::eTransfer;
    case vk::ImageLayout::eColorAttachmentOptimal:
      return vk::PipelineStageFlagBits::eColorAttachmentOutput;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      return vk::PipelineStageFlagBits::eTessellationControlShader |
             vk::PipelineStageFlagBits::eTessellationEvaluationShader |
             vk::PipelineStageFlagBits::eGeometryShader |
             vk::PipelineStageFlagBits::eFragmentShader;
    case vk::ImageLayout::ePresentSrcKHR:
      return vk::PipelineStageFlagBits::eBottomOfPipe;
    default:
      FXL_CHECK(false) << "layout=" << static_cast<int>(layout);
  }
  return vk::PipelineStageFlags();
}

vk::AccessFlags GetAccessMask(vk::ImageLayout layout) {
  switch (layout) {
    case vk::ImageLayout::eUndefined:
      return vk::AccessFlags();
    case vk::ImageLayout::eGeneral:
      return vk::AccessFlagBits::eColorAttachmentWrite |
             vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eTransferWrite |
             vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eShaderRead |
             vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eHostRead;
    case vk::ImageLayout::ePreinitialized:
      return vk::AccessFlagBits::eHostWrite;
    case vk::ImageLayout::eColorAttachmentOptimal:
      return vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eInputAttachmentRead;
    case vk::ImageLayout::eTransferSrcOptimal:
      return vk::AccessFlagBits::eTransferRead;
    case vk::ImageLayout::eTransferDstOptimal:
      return vk::AccessFlagBits::eTransferWrite;
    case vk::ImageLayout::ePresentSrcKHR:
      return vk::AccessFlags();
    default:
      FXL_CHECK(false) << "layout=" << static_cast<int>(layout);
  }
  return vk::AccessFlags();
}

void SetImageLayoutOnCommandBuffer(vk::CommandBuffer command_buffer, vk::Image image,
                                   vk::ImageLayout layout, vk::ImageLayout old_layout) {
  if (layout == old_layout)
    return;

  auto image_memory_barrier =
      vk::ImageMemoryBarrier()
          .setOldLayout(old_layout)
          .setNewLayout(layout)
          .setSrcAccessMask(GetAccessMask(old_layout))
          .setDstAccessMask(GetAccessMask(layout))
          .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
          .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
          .setImage(image)
          .setSubresourceRange(vk::ImageSubresourceRange()
                                   .setAspectMask(vk::ImageAspectFlagBits::eColor)
                                   .setBaseMipLevel(0)
                                   .setLevelCount(1)
                                   .setBaseArrayLayer(0)
                                   .setLayerCount(1));
  command_buffer.pipelineBarrier(GetPipelineStageFlags(old_layout), GetPipelineStageFlags(layout),
                                 vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1,
                                 &image_memory_barrier);
}

}  // anonymous namespace

Swapchain::Swapchain(bool protected_output) : protected_output_(protected_output) {}

Swapchain::~Swapchain() {
  swapchain_image_resources_.clear();
  gr_context_.reset();

  for (auto& resource : swapchain_image_resources_) {
    vk_device_.freeCommandBuffers(command_pool_, 1, &resource.post_raster_command_buffer);
  }
  vk_device_.destroySwapchainKHR(swapchain_);
  vk_instance_.destroySurfaceKHR(surface_);
  vk_device_.destroyCommandPool(command_pool_);
  vk_device_.destroy();
  vk_instance_.destroy();
}

bool Swapchain::Initialize(zx::channel image_pipe_endpoint,
                           std::optional<vk::Extent2D> surface_size) {
  if (GetPhysicalDevice() && CreateSurface(std::move(image_pipe_endpoint), surface_size) &&
      CreateDeviceAndQueue() && InitializeSwapchain() && PrepareBuffers()) {
    AcquireNextImage();
    return true;
  }
  return false;
}

uint32_t Swapchain::GetNumberOfSwapchainImages() {
  FXL_DCHECK(swapchain_image_resources_.size());
  return swapchain_image_resources_.size();
}

vk::Extent2D Swapchain::GetImageSize() { return max_image_extent_; }

GrContext* Swapchain::GetGrContext() {
  FXL_DCHECK(swapchain_image_resources_.size());

  if (gr_context_)
    return gr_context_.get();

  GrVkBackendContext backend_context;
  backend_context.fInstance = static_cast<VkInstance>(vk_instance_);
  backend_context.fPhysicalDevice = static_cast<VkPhysicalDevice>(vk_physical_device_);
  backend_context.fDevice = static_cast<VkDevice>(vk_device_);
  backend_context.fQueue = static_cast<VkQueue>(graphics_queue_);
  backend_context.fGraphicsQueueIndex = graphics_queue_family_index_;
  backend_context.fInstanceVersion = kVulkanApiVersion;
  // Use fVkExtensions.
  backend_context.fExtensions = kKHR_swapchain_GrVkExtensionFlag | kKHR_surface_GrVkExtensionFlag;
  backend_context.fGetProc = [this](const char* proc_name, VkInstance instance, VkDevice device) {
    if (device != VK_NULL_HANDLE)
      return vk_device_.getProcAddr(proc_name);
    return vk_instance_.getProcAddr(proc_name);
  };
  backend_context.fOwnsInstanceAndDevice = false;
  backend_context.fProtectedContext = protected_output_ ? GrProtected::kYes : GrProtected::kNo;
  gr_context_ = GrContext::MakeVulkan(backend_context);
  FXL_CHECK(gr_context_);
  return gr_context_.get();
}

Swapchain::SwapchainImageResources* Swapchain::GetCurrentImageResources() {
  return &swapchain_image_resources_[current_image_];
}

bool Swapchain::GetPhysicalDevice() {
  // Add extensions necessary for fuchsia.
  std::vector<const char*> layer_names;
#ifdef VKLATENCY_USE_FB
  layer_names.push_back("VK_LAYER_FUCHSIA_imagepipe_swapchain_fb");
#else
  layer_names.push_back("VK_LAYER_FUCHSIA_imagepipe_swapchain");
#endif  // VKLATENCY_USE_FB
#ifndef NDEBUG
  layer_names.push_back("VK_LAYER_LUNARG_standard_validation");
#endif  // NDEBUG

  std::vector<const char*> extension_names;
  extension_names.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  extension_names.push_back(VK_FUCHSIA_IMAGEPIPE_SURFACE_EXTENSION_NAME);
  extension_names.push_back(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
  extension_names.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
  extension_names.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

  // Create vk instance.
  auto application_info =
      vk::ApplicationInfo().setPApplicationName("VkLatency Demo").setApiVersion(kVulkanApiVersion);
  auto instance_info = vk::InstanceCreateInfo()
                           .setPApplicationInfo(&application_info)
                           .setEnabledLayerCount(layer_names.size())
                           .setPpEnabledLayerNames(layer_names.data())
                           .setEnabledExtensionCount(extension_names.size())
                           .setPpEnabledExtensionNames(extension_names.data());
  auto vk_instance = vk::createInstance(instance_info);
  if (vk_instance.result != vk::Result::eSuccess) {
    FXL_LOG(WARNING) << "Could not create Vulkan Instance.";
    return false;
  }
  vk_instance_ = vk_instance.value;

  // Get vk physical device.
  // TODO(emircan): Check physical device extensions and surface support instead
  // of choosing the first device.
  auto physical_devices = vk_instance_.enumeratePhysicalDevices();
  if (physical_devices.result != vk::Result::eSuccess) {
    FXL_LOG(WARNING) << "Could not enumerate physical devices.";
    return false;
  }
  vk_physical_device_ = physical_devices.value[0];

  auto physical_device_memory_features = vk::PhysicalDeviceProtectedMemoryFeatures();
  auto physical_device_features =
      vk::PhysicalDeviceFeatures2().setPNext(&physical_device_memory_features);
  vk_physical_device_.getFeatures2(&physical_device_features);
  if (protected_output_ && !physical_device_memory_features.protectedMemory) {
    FXL_LOG(WARNING) << "Protected memoy is not supported.";
    return false;
  }

  return true;
}

bool Swapchain::CreateSurface(zx::channel image_pipe_endpoint,
                              std::optional<vk::Extent2D> surface_size) {
  // Create surface.
#ifdef VKLATENCY_USE_FB
  auto surface_create_info = vk::ImagePipeSurfaceCreateInfoFUCHSIA();
#else
  auto surface_create_info =
      vk::ImagePipeSurfaceCreateInfoFUCHSIA().setImagePipeHandle(image_pipe_endpoint.release());
#endif  // VKLATENCY_USE_FB

  auto create_image_pipe_surface =
      vk_instance_.createImagePipeSurfaceFUCHSIA(surface_create_info, nullptr);
  if (create_image_pipe_surface.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create image pipe surface";
    return false;
  }
  surface_ = create_image_pipe_surface.value;

  // Parse physical device properties.
  bool graphics_queue_found = false;
  auto queue_properties = vk_physical_device_.getQueueFamilyProperties();
  for (uint64_t i = 0; i < queue_properties.size(); ++i) {
    if (vk_physical_device_.getSurfaceSupportKHR(i, surface_).value) {
      graphics_queue_family_index_ = i;
      graphics_queue_found = true;
      break;
    }
  }
  if (!graphics_queue_found) {
    FXL_LOG(ERROR) << "failed to find graphics queue";
    return false;
  }

  // Parse surface properties.
  auto surface_formats = vk_physical_device_.getSurfaceFormatsKHR(surface_).value;
  bool format_supported = false;
  if (surface_formats.size() == 1 && surface_formats[0].format == vk::Format::eUndefined) {
    format_supported = true;
  } else {
    for (auto surface_format : surface_formats) {
      if (surface_format.format == format_) {
        format_supported = true;
        break;
      }
    }
  }
  if (!format_supported) {
    FXL_LOG(ERROR) << "failed to find supported format";
    return false;
  }

  // Get surface capabilities for width and height.
  if (surface_size.has_value()) {
    max_image_extent_ = surface_size.value();
  } else {
    auto surface_caps = vk_physical_device_.getSurfaceCapabilitiesKHR(surface_);
    if (surface_caps.result != vk::Result::eSuccess) {
      FXL_LOG(ERROR) << "failed to get surface capabilities";
      return false;
    }
    max_image_extent_ = surface_caps.value.maxImageExtent;
  }

  return true;
}

bool Swapchain::CreateDeviceAndQueue() {
  // Set device extensions.
  std::vector<const char*> device_ext;
  device_ext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

  // Create logical device.
  const std::vector<float> queue_priorities(1, 0.0);
  auto queue_create_info =
      vk::DeviceQueueCreateInfo()
          .setFlags(protected_output_ ? vk::DeviceQueueCreateFlagBits::eProtected
                                      : vk::DeviceQueueCreateFlags())
          .setQueueCount(1)
          .setQueueFamilyIndex(graphics_queue_family_index_)
          .setPQueuePriorities(queue_priorities.data());
  auto protected_memory = vk::PhysicalDeviceProtectedMemoryFeatures().setProtectedMemory(true);
  auto device_create_info = vk::DeviceCreateInfo()
                                .setPNext(protected_output_ ? &protected_memory : nullptr)
                                .setQueueCreateInfoCount(1)
                                .setPQueueCreateInfos(&queue_create_info)
                                .setEnabledExtensionCount(device_ext.size())
                                .setPpEnabledExtensionNames(device_ext.data());
  auto create_device = vk_physical_device_.createDevice(device_create_info);
  if (create_device.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create vulkan device";
    return false;
  }
  vk_device_ = create_device.value;

  // Get device queue.
  if (protected_output_) {
    auto queue_info2 = vk::DeviceQueueInfo2()
                           .setFlags(vk::DeviceQueueCreateFlagBits::eProtected)
                           .setQueueFamilyIndex(graphics_queue_family_index_)
                           .setQueueIndex(0);
    graphics_queue_ = vk_device_.getQueue2(queue_info2);
  } else {
    graphics_queue_ = vk_device_.getQueue(graphics_queue_family_index_, 0);
  }

  // Create command pool.
  vk::CommandPoolCreateFlags cmd_pool_flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  cmd_pool_flags |=
      protected_output_ ? vk::CommandPoolCreateFlagBits::eProtected : vk::CommandPoolCreateFlags();
  auto cmd_pool_info = vk::CommandPoolCreateInfo()
                           .setFlags(cmd_pool_flags)
                           .setQueueFamilyIndex(graphics_queue_family_index_);
  auto create_command_pool = vk_device_.createCommandPool(cmd_pool_info);
  if (create_command_pool.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create command pool";
    return false;
  }
  command_pool_ = create_command_pool.value;
  return true;
}

bool Swapchain::InitializeSwapchain() {
  // Create swapchain.
  auto swapchain_ci = vk::SwapchainCreateInfoKHR()
                          .setFlags(protected_output_ ? vk::SwapchainCreateFlagBitsKHR::eProtected
                                                      : vk::SwapchainCreateFlagsKHR())
                          .setSurface(surface_)
                          .setMinImageCount(desired_image_count_)
                          .setImageFormat(format_)
                          .setImageColorSpace(vk::ColorSpaceKHR::eSrgbNonlinear)
                          .setImageExtent(max_image_extent_)
                          .setImageArrayLayers(1)
                          .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
                          .setImageSharingMode(vk::SharingMode::eExclusive)
                          .setQueueFamilyIndexCount(0)
                          .setPQueueFamilyIndices(nullptr)
                          .setPreTransform(vk::SurfaceTransformFlagBitsKHR::eIdentity)
                          .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                          .setPresentMode(vk::PresentModeKHR::eFifo)
                          .setClipped(true);
  auto create_swapchain = vk_device_.createSwapchainKHR(swapchain_ci);
  if (create_swapchain.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create swapchain";
    return false;
  }
  swapchain_ = create_swapchain.value;

  // Create fences and semaphores.
  auto fence_ci = vk::FenceCreateInfo().setFlags(vk::FenceCreateFlagBits::eSignaled);
  auto create_fence = vk_device_.createFence(fence_ci);
  if (create_fence.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create fence";
    return false;
  }
  fence_ = create_fence.value;

  std::vector<vk::Semaphore> semaphores;
  auto semaphore_ci = vk::SemaphoreCreateInfo();
  auto create_semaphore = vk_device_.createSemaphore(semaphore_ci);
  if (create_semaphore.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create semaphore";
    return false;
  }
  next_present_semaphore_ = create_semaphore.value;
  return true;
}

bool Swapchain::PrepareBuffers() {
  // Get swapchain images.
  auto get_swapchain_images = vk_device_.getSwapchainImagesKHR(swapchain_);
  if (get_swapchain_images.result != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to create swapchain";
    return false;
  }
  FXL_LOG(INFO) << "Swapchain created with image count:" << get_swapchain_images.value.size();

  // Create image resources.
  for (uint64_t i = 0; i < get_swapchain_images.value.size(); ++i) {
    swapchain_image_resources_.emplace_back();
    auto& image_resource = swapchain_image_resources_.back();
    image_resource.index = i;
    image_resource.image = get_swapchain_images.value[i];
    image_resource.layout = vk::ImageLayout::eUndefined;

    std::vector<vk::Semaphore> semaphores;
    auto semaphore_ci = vk::SemaphoreCreateInfo();
    for (uint64_t s = 0; s < 2; ++s) {
      auto create_semaphore = vk_device_.createSemaphore(semaphore_ci);
      if (create_semaphore.result != vk::Result::eSuccess) {
        FXL_LOG(ERROR) << "failed to create semaphore";
        return false;
      }
      semaphores.push_back(create_semaphore.value);
    }
    image_resource.render_semaphore = semaphores[0];
    image_resource.present_semaphore = semaphores[1];

    // Allocate command buffers.
    auto cmd_ai = vk::CommandBufferAllocateInfo()
                      .setCommandPool(command_pool_)
                      .setLevel(vk::CommandBufferLevel::ePrimary)
                      .setCommandBufferCount(1);
    auto allocate_command_buffer = vk_device_.allocateCommandBuffers(cmd_ai);
    if (allocate_command_buffer.result != vk::Result::eSuccess) {
      FXL_LOG(ERROR) << "failed to allocate command buffers";
      return false;
    }
    image_resource.post_raster_command_buffer = allocate_command_buffer.value[0];
  }
  return true;
}

void Swapchain::AcquireNextImage() {
  auto acquire_next = vk_device_.acquireNextImageKHR(
      swapchain_, UINT64_MAX, next_present_semaphore_, vk::Fence(), &current_image_);
  if (acquire_next != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to acquire next image";
    return;
  }
  std::swap(swapchain_image_resources_[current_image_].present_semaphore, next_present_semaphore_);
}

void Swapchain::SwapImages() {
  auto& current_image_resources = swapchain_image_resources_[current_image_];

  // Set Current image for present.
  current_image_resources.post_raster_command_buffer.reset(vk::CommandBufferResetFlags());
  auto cmd_bi =
      vk::CommandBufferBeginInfo().setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
  current_image_resources.post_raster_command_buffer.begin(&cmd_bi);
  auto present_layout = vk::ImageLayout::ePresentSrcKHR;
  SetImageLayoutOnCommandBuffer(current_image_resources.post_raster_command_buffer,
                                current_image_resources.image, present_layout,
                                current_image_resources.layout);
  current_image_resources.layout = present_layout;
  current_image_resources.post_raster_command_buffer.end();

  // Submit info.
  auto protected_submit_info = vk::ProtectedSubmitInfo().setProtectedSubmit(true);
  vk::PipelineStageFlags pipe_stage_flags = vk::PipelineStageFlagBits::eAllCommands;
  auto submit_info = vk::SubmitInfo()
                         .setCommandBufferCount(1)
                         .setPNext(protected_output_ ? &protected_submit_info : nullptr)
                         .setPCommandBuffers(&current_image_resources.post_raster_command_buffer)
                         .setPWaitDstStageMask(&pipe_stage_flags)
                         .setSignalSemaphoreCount(1)
                         .setPSignalSemaphores(&current_image_resources.render_semaphore);
  auto queue_submit = graphics_queue_.submit(1, &submit_info, vk::Fence());
  if (queue_submit != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to queue submit";
    return;
  }

  // Present image.
  auto present_info = vk::PresentInfoKHR()
                          .setSwapchainCount(1)
                          .setPSwapchains(&swapchain_)
                          .setPImageIndices(&current_image_)
                          .setWaitSemaphoreCount(1)
                          .setPWaitSemaphores(&current_image_resources.render_semaphore);
  auto present = graphics_queue_.presentKHR(&present_info);
  if (present != vk::Result::eSuccess) {
    FXL_LOG(ERROR) << "failed to present";
    return;
  }

  // Acquire then next image.
  // We do not need to change ImageLayout from ePresentSrcKHR to
  // eColorAttachmentOptimal here because Skia already handles it before
  // drawing.
  AcquireNextImage();
}

}  // namespace examples
