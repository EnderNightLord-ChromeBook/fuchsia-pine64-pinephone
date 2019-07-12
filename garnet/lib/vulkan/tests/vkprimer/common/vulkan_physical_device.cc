// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vulkan_physical_device.h"

#include "utils.h"
#include "vulkan_queue.h"
#include "vulkan_swapchain.h"

namespace {

const char *kMagmaLayer = "VK_LAYER_FUCHSIA_imagepipe_swapchain_fb";

static const std::vector<const char *> s_required_phys_device_props = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef __Fuchsia__
    VK_FUCHSIA_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_FUCHSIA_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#endif
};

bool ChooseGraphicsDevice(const VkPhysicalDevice &phys_device,
                          const VkSurfaceKHR &surface,
                          VkPhysicalDevice *phys_device_out) {
  if (FindMatchingProperties(s_required_phys_device_props, PHYS_DEVICE_EXT_PROP,
                             &phys_device, kMagmaLayer,
                             nullptr /* missing_props */)) {
    return false;
  }

  VulkanSwapchain::Info swapchain_info;
  if (!VulkanSwapchain::QuerySwapchainSupport(phys_device, surface,
                                              &swapchain_info)) {
    return false;
  }

  VulkanQueue vulkan_queue(phys_device, surface);
  if (!vulkan_queue.Init()) {
    return false;
  }
  *phys_device_out = phys_device;
  return true;
}

}  // namespace

VulkanPhysicalDevice::VulkanPhysicalDevice(
    std::shared_ptr<VulkanInstance> instance, const VkSurfaceKHR &surface)
    : initialized_(false), instance_(instance) {
  params_ = std::make_unique<InitParams>(surface);
}

VulkanPhysicalDevice::InitParams::InitParams(const VkSurfaceKHR &surface)
    : surface_(surface) {}

bool VulkanPhysicalDevice::Init() {
  if (initialized_) {
    RTN_MSG(false, "VulkanPhysicalDevice already initialized.\n");
  }
  uint32_t num_phys_devices = 0;
  auto err = vkEnumeratePhysicalDevices(instance_->instance(),
                                        &num_phys_devices, nullptr);
  if (VK_SUCCESS != err || 0 == num_phys_devices) {
    RTN_MSG(false, "VK Error: 0x%x - No physical device found.\n", err);
  }
  std::vector<VkPhysicalDevice> phys_devices(num_phys_devices);
  vkEnumeratePhysicalDevices(instance_->instance(), &num_phys_devices,
                             phys_devices.data());

  for (const auto &phys_device : phys_devices) {
    if (ChooseGraphicsDevice(phys_device, params_->surface_, &phys_device_)) {
      initialized_ = true;
      break;
    }
  }

  if (!initialized_) {
    RTN_MSG(false, "Couldn't find graphics family device.\n");
  }
  params_.reset();
  return initialized_;
}

void VulkanPhysicalDevice::AppendRequiredPhysDeviceExts(
    std::vector<const char *> *exts) {
  exts->insert(exts->end(), s_required_phys_device_props.begin(),
               s_required_phys_device_props.end());
}

VkPhysicalDevice VulkanPhysicalDevice::phys_device() const {
  if (!initialized_) {
    RTN_MSG(nullptr, "%s", "Request for uninitialized instance.\n");
  }
  return phys_device_;
}
