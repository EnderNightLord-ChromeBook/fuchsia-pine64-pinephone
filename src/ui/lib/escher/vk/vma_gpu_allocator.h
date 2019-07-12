// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_VK_VMA_GPU_ALLOCATOR_H_
#define SRC_UI_LIB_ESCHER_VK_VMA_GPU_ALLOCATOR_H_

#include <vulkan/vulkan.hpp>

#include "src/ui/lib/escher/third_party/VulkanMemoryAllocator/vk_mem_alloc.h"
#include "src/ui/lib/escher/vk/gpu_allocator.h"
#include "src/ui/lib/escher/vk/vulkan_context.h"

namespace escher {

// This class implements the GpuAllocator interface on top of a vk_mem_alloc
// instance.
class VmaGpuAllocator : public GpuAllocator {
 public:
  VmaGpuAllocator(const VulkanContext& context);
  ~VmaGpuAllocator();

  // |GpuAllocator|
  GpuMemPtr AllocateMemory(vk::MemoryRequirements reqs, vk::MemoryPropertyFlags flags) override;

  // |GpuAllocator|
  BufferPtr AllocateBuffer(ResourceManager* manager, vk::DeviceSize size,
                           vk::BufferUsageFlags usage_flags,
                           vk::MemoryPropertyFlags memory_property_flags,
                           GpuMemPtr* out_ptr) override;

  // |GpuAllocator|
  ImagePtr AllocateImage(ResourceManager* manager, const escher::ImageInfo& info,
                         GpuMemPtr* out_ptr) override;

  // |GpuAllocator|
  uint32_t GetTotalBytesAllocated() const override;

 private:
  VmaAllocator allocator_;
};

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_VK_VMA_GPU_ALLOCATOR_H_
