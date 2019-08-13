// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "utils.h"

#include <optional>
#include <unordered_set>

namespace {

static void PrintProps(const std::vector<std::string> &props) {
  for (const auto &prop : props) {
    printf("\t%s\n", prop.c_str());
  }
  printf("\n");
}

}  // namespace

namespace vkp {

static bool MatchProperties(const std::vector<const char *> &desired_props, SearchProp search_prop,
                            const vk::PhysicalDevice &phys_device, const char *layer,
                            std::unordered_set<std::string> *props_found_set) {
  std::optional<vk::ResultValue<std::vector<vk::ExtensionProperties>>> rv_ext;
  std::optional<vk::ResultValue<std::vector<vk::LayerProperties>>> rv_layer;
  std::vector<vk::ExtensionProperties> ext_props;
  std::vector<vk::LayerProperties> layer_props;
  switch (search_prop) {
    case INSTANCE_EXT_PROP:
      if (layer) {
        rv_ext = vk::enumerateInstanceExtensionProperties(std::string(layer));
      } else {
        rv_ext = vk::enumerateInstanceExtensionProperties(nullptr);
      }
      if (vk::Result::eSuccess != rv_ext->result) {
        RTN_MSG(false, "VK Error: 0x%x - Failed to enumerate extension properties.",
                rv_ext->result);
      }
      ext_props = rv_ext->value;
      break;
    case INSTANCE_LAYER_PROP:
      rv_layer = vk::enumerateInstanceLayerProperties();
      if (vk::Result::eSuccess != rv_layer->result) {
        RTN_MSG(false, "VK Error: 0x%x - Failed to enumerate layer properties.", rv_layer->result);
      }
      layer_props = rv_layer->value;
      break;
    case PHYS_DEVICE_EXT_PROP:
      if (layer) {
        rv_ext = phys_device.enumerateDeviceExtensionProperties(std::string(layer));
      } else {
        rv_ext = phys_device.enumerateDeviceExtensionProperties(nullptr);
      }
      if (vk::Result::eSuccess != rv_ext->result) {
        RTN_MSG(false, "VK Error: 0x%x - Failed to enumerate layer properties.", rv_ext->result);
      }
      ext_props = rv_ext->value;
      break;
  }

  if (search_prop == INSTANCE_LAYER_PROP) {
    for (auto &prop : layer_props) {
      props_found_set->insert(std::string(prop.layerName));
    }
  } else {
    for (auto &prop : ext_props) {
      props_found_set->insert(std::string(prop.extensionName));
    }
  }

  return true;
}

bool FindMatchingProperties(const std::vector<const char *> &desired_props, SearchProp search_prop,
                            const vk::PhysicalDevice *phys_device, const char *layer,
                            std::vector<std::string> *missing_props_out) {
  std::unordered_set<std::string> props_found_set;

  // Match Vulkan properties.  "Vulkan properties" are those
  // found when the layer argument is set to null.
  bool success =
      MatchProperties(desired_props, search_prop, *phys_device, nullptr, &props_found_set);

  if (!success) {
    if (missing_props_out) {
      missing_props_out->insert(missing_props_out->end(), desired_props.begin(),
                                desired_props.end());
    }
    RTN_MSG(false, "Unable to match vulkan properties.\n");
  }

  // Match layer properties.
  if (search_prop != INSTANCE_LAYER_PROP && layer &&
      props_found_set.size() != desired_props.size()) {
    success = MatchProperties(desired_props, search_prop, *phys_device, layer, &props_found_set);
  }

  if (missing_props_out) {
    for (const auto &prop : desired_props) {
      auto iter = props_found_set.find(prop);
      if (iter == props_found_set.end()) {
        missing_props_out->emplace_back(*iter);
      }
    }
  }

  if (missing_props_out && !missing_props_out->empty()) {
    PrintProps(*missing_props_out);
  }

  return props_found_set.size() == desired_props.size();
}

bool FindGraphicsQueueFamilies(vk::PhysicalDevice phys_device, VkSurfaceKHR surface,
                               std::vector<uint32_t> *queue_family_indices) {
  auto queue_families = phys_device.getQueueFamilyProperties();
  int queue_family_index = 0;
  for (const auto &queue_family : queue_families) {
    auto rv = phys_device.getSurfaceSupportKHR(queue_family_index, surface);
    if (vk::Result::eSuccess != rv.result) {
      RTN_MSG(false, "VK Error: 0x%x - Failed to get surface present support.", rv.result);
    }
    const vk::Bool32 present_support = rv.value;

    if ((queue_family.queueCount > 0) && (queue_family.queueFlags & vk::QueueFlagBits::eGraphics) &&
        present_support) {
      if (queue_family_indices) {
        queue_family_indices->emplace_back(queue_family_index);
      }
      return true;
    }
    queue_family_index++;
  }
  RTN_MSG(false, "No queue family indices found.\n");
}

}  // namespace vkp
