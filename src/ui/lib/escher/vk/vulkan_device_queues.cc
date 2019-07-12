// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/vk/vulkan_device_queues.h"

#include <set>

#include "src/lib/fxl/logging.h"
#include "src/ui/lib/escher/impl/vulkan_utils.h"

namespace escher {

VulkanDeviceQueues::Caps::Caps(vk::PhysicalDevice device) {
  {
    vk::PhysicalDeviceProperties props = device.getProperties();
    max_image_width = props.limits.maxImageDimension2D;
    max_image_height = props.limits.maxImageDimension2D;
  }

  {
    auto formats = impl::GetSupportedDepthFormats(device, {
                                                              vk::Format::eD16Unorm,
                                                              vk::Format::eX8D24UnormPack32,
                                                              vk::Format::eD32Sfloat,
                                                              vk::Format::eS8Uint,
                                                              vk::Format::eD16UnormS8Uint,
                                                              vk::Format::eD24UnormS8Uint,
                                                              vk::Format::eD32SfloatS8Uint,
                                                          });
    depth_stencil_formats.insert(formats.begin(), formats.end());
  }
}

vk::Format VulkanDeviceQueues::Caps::GetMatchingDepthStencilFormat(
    std::vector<vk::Format> formats) const {
  for (auto& fmt : formats) {
    if (depth_stencil_formats.find(fmt) != depth_stencil_formats.end()) {
      return fmt;
    }
  }
  FXL_CHECK(false) << "no matching depth format found.";
  return vk::Format::eUndefined;
}

namespace {

// Helper for PopulateProcAddrs().
template <typename FuncT>
static FuncT GetDeviceProcAddr(vk::Device device, const char* func_name) {
  FuncT func = reinterpret_cast<FuncT>(device.getProcAddr(func_name));
  FXL_CHECK(func) << "failed to find function address for: " << func_name;
  return func;
}

// Helper for VulkanDeviceQueues constructor.
VulkanDeviceQueues::ProcAddrs PopulateProcAddrs(vk::Device device,
                                                const VulkanDeviceQueues::Params& params) {
#define GET_DEVICE_PROC_ADDR(XXX) result.XXX = GetDeviceProcAddr<PFN_vk##XXX>(device, "vk" #XXX)

  VulkanDeviceQueues::ProcAddrs result;
  if (params.required_extension_names.find(VK_KHR_SWAPCHAIN_EXTENSION_NAME) !=
          params.required_extension_names.end() ||
      params.desired_extension_names.find(VK_KHR_SWAPCHAIN_EXTENSION_NAME) !=
          params.desired_extension_names.end()) {
    GET_DEVICE_PROC_ADDR(CreateSwapchainKHR);
    GET_DEVICE_PROC_ADDR(DestroySwapchainKHR);
    GET_DEVICE_PROC_ADDR(GetSwapchainImagesKHR);
    GET_DEVICE_PROC_ADDR(AcquireNextImageKHR);
    GET_DEVICE_PROC_ADDR(QueuePresentKHR);
  }
  return result;

#undef GET_DEVICE_PROC_ADDR
}

// Return value for FindSuitablePhysicalDeviceAndQueueFamilies(). Valid if and
// only if device is non-null.  Otherwise, no suitable device was found.
struct SuitablePhysicalDeviceAndQueueFamilies {
  vk::PhysicalDevice physical_device;
  uint32_t main_queue_family;
  uint32_t transfer_queue_family;
};

SuitablePhysicalDeviceAndQueueFamilies FindSuitablePhysicalDeviceAndQueueFamilies(
    const VulkanInstancePtr& instance, const VulkanDeviceQueues::Params& params) {
  auto physical_devices =
      ESCHER_CHECKED_VK_RESULT(instance->vk_instance().enumeratePhysicalDevices());

  // A suitable main queue needs to support graphics and compute.
  const auto kMainQueueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;

  // A specialized transfer queue will only support transfer; see comment below,
  // where these flags are used.
  const auto kTransferQueueFlags =
      vk::QueueFlagBits::eTransfer | vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;

  for (auto& physical_device : physical_devices) {
    // Look for a physical device that has all required extensions.
    if (!VulkanDeviceQueues::ValidateExtensions(physical_device, params.required_extension_names,
                                                instance->params().layer_names)) {
      continue;
    }

    // Find the main queue family.  If none is found, continue on to the next
    // physical device.
    auto queues = physical_device.getQueueFamilyProperties();
    const bool filter_queues_for_present =
        params.surface &&
        !(params.flags & VulkanDeviceQueues::Params::kDisableQueueFilteringForPresent);
    for (size_t i = 0; i < queues.size(); ++i) {
      if (kMainQueueFlags == (queues[i].queueFlags & kMainQueueFlags)) {
        if (filter_queues_for_present) {
          // TODO: it is possible that there is no queue family that supports
          // both graphics/compute and present.  In this case, we would need a
          // separate present queue.  For now, just look for a single queue that
          // meets all of our needs.
          VkBool32 supports_present;
          auto result = instance->proc_addrs().GetPhysicalDeviceSurfaceSupportKHR(
              physical_device, i, params.surface, &supports_present);
          FXL_CHECK(result == VK_SUCCESS);
          if (supports_present != VK_TRUE) {
            FXL_LOG(INFO) << "Queue supports graphics/compute, but not presentation";
            continue;
          }
        }

        // At this point, we have already succeeded.  Now, try to find the
        // optimal transfer queue family.
        SuitablePhysicalDeviceAndQueueFamilies result;
        result.physical_device = physical_device;
        result.main_queue_family = i;
        result.transfer_queue_family = i;
        for (size_t j = 0; j < queues.size(); ++j) {
          if ((queues[i].queueFlags & kTransferQueueFlags) == vk::QueueFlagBits::eTransfer) {
            // We have found a transfer-only queue.  This is the fastest way to
            // upload data to the GPU.
            result.transfer_queue_family = j;
            break;
          }
        }
        return result;
      }
    }
  }
  return {vk::PhysicalDevice(), 0, 0};
}

// Helper for ValidateExtensions().
bool ValidateExtension(vk::PhysicalDevice device, const std::string name,
                       const std::vector<vk::ExtensionProperties>& base_extensions,
                       const std::set<std::string>& required_layer_names) {
  auto found = std::find_if(base_extensions.begin(), base_extensions.end(),
                            [&name](const vk::ExtensionProperties& extension) {
                              return !strncmp(extension.extensionName, name.c_str(),
                                              VK_MAX_EXTENSION_NAME_SIZE);
                            });
  if (found != base_extensions.end())
    return true;

  // Didn't find the extension in the base list of extensions.  Perhaps it is
  // implemented in a layer.
  for (auto& layer_name : required_layer_names) {
    auto layer_extensions =
        ESCHER_CHECKED_VK_RESULT(device.enumerateDeviceExtensionProperties(layer_name));
    FXL_LOG(INFO) << "Looking for Vulkan device extension: " << name << " in layer: " << layer_name;

    auto found = std::find_if(layer_extensions.begin(), layer_extensions.end(),
                              [&name](vk::ExtensionProperties& extension) {
                                return !strncmp(extension.extensionName, name.c_str(),
                                                VK_MAX_EXTENSION_NAME_SIZE);
                              });
    if (found != layer_extensions.end())
      return true;
  }

  return false;
}

}  // namespace

fxl::RefPtr<VulkanDeviceQueues> VulkanDeviceQueues::New(VulkanInstancePtr instance, Params params) {
  // Escher requires the memory_requirements_2 extension for the
  // vma_gpu_allocator to function.
  params.required_extension_names.insert(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);

  // If the params contain a surface, then ensure that the swapchain extension
  // is supported so that we can render to that surface.
  if (params.surface) {
    params.required_extension_names.insert(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

#if defined(OS_FUCHSIA)
  // If we're running on Fuchsia, make sure we have our semaphore extensions.
  params.required_extension_names.insert(VK_FUCHSIA_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
  params.required_extension_names.insert(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
#endif

  vk::PhysicalDevice physical_device;
  uint32_t main_queue_family;
  uint32_t transfer_queue_family;
  {
    SuitablePhysicalDeviceAndQueueFamilies result =
        FindSuitablePhysicalDeviceAndQueueFamilies(instance, params);
    FXL_CHECK(result.physical_device) << "Unable to find a suitable physical device.";
    physical_device = result.physical_device;
    main_queue_family = result.main_queue_family;
    transfer_queue_family = result.transfer_queue_family;
  }

  // Partially populate device capabilities from the physical device.
  // Other stuff (e.g. which extensions are supported) will be added below.
  VulkanDeviceQueues::Caps caps(physical_device);

  // Prepare to create the Device and Queues.
  vk::DeviceQueueCreateInfo queue_info[2];
  const float kQueuePriority = 0;
  queue_info[0] = vk::DeviceQueueCreateInfo();
  queue_info[0].queueFamilyIndex = main_queue_family;
  queue_info[0].queueCount = 1;
  queue_info[0].pQueuePriorities = &kQueuePriority;
  queue_info[1] = vk::DeviceQueueCreateInfo();
  queue_info[1].queueFamilyIndex = transfer_queue_family;
  queue_info[1].queueCount = 1;
  queue_info[1].pQueuePriorities = &kQueuePriority;

  // Prepare the list of extension names that will be used to create the device.
  {
    // These extensions were already validated by
    // FindSuitablePhysicalDeviceAndQueueFamilies();
    caps.extensions = params.required_extension_names;

    // Request as many of the desired (but optional) extensions as possible.
    auto extensions =
        ESCHER_CHECKED_VK_RESULT(physical_device.enumerateDeviceExtensionProperties());

    std::set<std::string> available_desired_extensions;
    for (auto& name : params.desired_extension_names) {
      if (ValidateExtension(physical_device, name, extensions, instance->params().layer_names)) {
        caps.extensions.insert(name);
      }
    }
  }
  std::vector<const char*> extension_names;
  for (auto& extension : caps.extensions) {
    extension_names.push_back(extension.c_str());
  }

  // Specify the required physical device features, and verify that they are all
  // supported.
  // TODO(ES-111): instead of hard-coding the required features here, provide a
  // mechanism for Escher clients to specify additional required features.
  vk::PhysicalDeviceFeatures supported_device_features;
  physical_device.getFeatures(&supported_device_features);
  bool device_has_all_required_features = true;

#define ADD_DESIRED_FEATURE(X)                                              \
  if (supported_device_features.X) {                                        \
    caps.enabled_features.X = true;                                         \
  } else {                                                                  \
    FXL_LOG(INFO) << "Desired Vulkan Device feature not supported: " << #X; \
  }

#define ADD_REQUIRED_FEATURE(X)                                               \
  caps.enabled_features.X = true;                                             \
  if (!supported_device_features.X) {                                         \
    FXL_LOG(ERROR) << "Required Vulkan Device feature not supported: " << #X; \
    device_has_all_required_features = false;                                 \
  }

  // TODO(MA-478): We would like to make 'shaderClipDistance' a requirement on
  // all Scenic platforms.  For now, treat it as a DESIRED_FEATURE.
  ADD_DESIRED_FEATURE(shaderClipDistance);
  ADD_DESIRED_FEATURE(fillModeNonSolid);

#undef ADD_DESIRED_FEATURE
#undef ADD_REQUIRED_FEATURE

  if (!device_has_all_required_features) {
    return fxl::RefPtr<VulkanDeviceQueues>();
  }

  // Almost ready to create the device; start populating the VkDeviceCreateInfo.
  vk::DeviceCreateInfo device_info;
  device_info.queueCreateInfoCount = 2;
  device_info.pQueueCreateInfos = queue_info;
  device_info.enabledExtensionCount = extension_names.size();
  device_info.ppEnabledExtensionNames = extension_names.data();
  device_info.pEnabledFeatures = &caps.enabled_features;

  // It's possible that the main queue and transfer queue are in the same
  // queue family.  Adjust the device-creation parameters to account for this.
  uint32_t main_queue_index = 0;
  uint32_t transfer_queue_index = 0;
  if (main_queue_family == transfer_queue_family) {
#if 0
    // TODO: it may be worthwhile to create multiple queues in the same family.
    // However, we would need to look at VkQueueFamilyProperties.queueCount to
    // make sure that we can create multiple queues for that family.  For now,
    // it is easier to share a single queue when the main/transfer queues are in
    // the same family.
    queue_info[0].queueCount = 2;
    device_info.queueCreateInfoCount = 1;
    transfer_queue_index = 1;
#else
    device_info.queueCreateInfoCount = 1;
#endif
  }

  // Create the device.
  auto result = physical_device.createDevice(device_info);
  if (result.result != vk::Result::eSuccess) {
    FXL_LOG(WARNING) << "Could not create Vulkan Device.";
    return fxl::RefPtr<VulkanDeviceQueues>();
  }
  vk::Device device = result.value;

  // Obtain the queues that we requested to be created with the device.
  vk::Queue main_queue = device.getQueue(main_queue_family, main_queue_index);
  vk::Queue transfer_queue = device.getQueue(transfer_queue_family, transfer_queue_index);

  return fxl::AdoptRef(new VulkanDeviceQueues(
      device, physical_device, main_queue, main_queue_family, transfer_queue, transfer_queue_family,
      std::move(instance), std::move(params), std::move(caps)));
}

VulkanDeviceQueues::VulkanDeviceQueues(vk::Device device, vk::PhysicalDevice physical_device,
                                       vk::Queue main_queue, uint32_t main_queue_family,
                                       vk::Queue transfer_queue, uint32_t transfer_queue_family,
                                       VulkanInstancePtr instance, Params params, Caps caps)
    : device_(device),
      physical_device_(physical_device),
      dispatch_loader_(instance->vk_instance(), device_),
      main_queue_(main_queue),
      main_queue_family_(main_queue_family),
      transfer_queue_(transfer_queue),
      transfer_queue_family_(transfer_queue_family),
      instance_(std::move(instance)),
      params_(std::move(params)),
      caps_(std::move(caps)),
      proc_addrs_(PopulateProcAddrs(device_, params_)) {}

VulkanDeviceQueues::~VulkanDeviceQueues() { device_.destroy(); }

bool VulkanDeviceQueues::ValidateExtensions(vk::PhysicalDevice device,
                                            const std::set<std::string>& required_extension_names,
                                            const std::set<std::string>& required_layer_names) {
  auto extensions = ESCHER_CHECKED_VK_RESULT(device.enumerateDeviceExtensionProperties());

  for (auto& name : required_extension_names) {
    if (!ValidateExtension(device, name, extensions, required_layer_names)) {
      FXL_LOG(WARNING) << "Vulkan has no device extension named: " << name;
      return false;
    }
  }
  return true;
}

VulkanContext VulkanDeviceQueues::GetVulkanContext() const {
  return escher::VulkanContext(instance_->vk_instance(), vk_physical_device(), vk_device(),
                               dispatch_loader(), vk_main_queue(), vk_main_queue_family(),
                               vk_transfer_queue(), vk_transfer_queue_family());
}

}  // namespace escher
