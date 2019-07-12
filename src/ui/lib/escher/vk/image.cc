// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/vk/image.h"

#include "src/ui/lib/escher/util/image_utils.h"

namespace escher {

const ResourceTypeInfo Image::kTypeInfo("Image", ResourceType::kResource,
                                        ResourceType::kWaitableResource, ResourceType::kImage);

ImagePtr Image::WrapVkImage(ResourceManager* image_owner, ImageInfo info, vk::Image vk_image) {
  return fxl::AdoptRef(new Image(image_owner, info, vk_image, 0, nullptr));
}

Image::Image(ResourceManager* image_owner, ImageInfo info, vk::Image image, vk::DeviceSize size,
             uint8_t* host_ptr)
    : WaitableResource(image_owner), info_(info), image_(image), size_(size), host_ptr_(host_ptr) {
  auto is_depth_stencil = image_utils::IsDepthStencilFormat(info.format);
  has_depth_ = is_depth_stencil.first;
  has_stencil_ = is_depth_stencil.second;
}

}  // namespace escher
