// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/escher.h"

#include "src/ui/lib/escher/defaults/default_shader_program_factory.h"
#include "src/ui/lib/escher/impl/command_buffer_pool.h"
#include "src/ui/lib/escher/impl/frame_manager.h"
#include "src/ui/lib/escher/impl/glsl_compiler.h"
#include "src/ui/lib/escher/impl/image_cache.h"
#include "src/ui/lib/escher/impl/mesh_manager.h"
#include "src/ui/lib/escher/impl/vk/pipeline_cache.h"
#include "src/ui/lib/escher/profiling/timestamp_profiler.h"
#include "src/ui/lib/escher/renderer/batch_gpu_uploader.h"
#include "src/ui/lib/escher/renderer/buffer_cache.h"
#include "src/ui/lib/escher/renderer/frame.h"
#include "src/ui/lib/escher/resources/resource_recycler.h"
#include "src/ui/lib/escher/util/hasher.h"
#include "src/ui/lib/escher/util/image_utils.h"
#include "src/ui/lib/escher/util/trace_macros.h"
#include "src/ui/lib/escher/vk/gpu_allocator.h"
#include "src/ui/lib/escher/vk/impl/descriptor_set_allocator.h"
#include "src/ui/lib/escher/vk/impl/framebuffer_allocator.h"
#include "src/ui/lib/escher/vk/impl/pipeline_layout_cache.h"
#include "src/ui/lib/escher/vk/impl/render_pass_cache.h"
#include "src/ui/lib/escher/vk/texture.h"
#include "src/ui/lib/escher/vk/vma_gpu_allocator.h"
#include "third_party/shaderc/libshaderc/include/shaderc/shaderc.hpp"

namespace escher {

namespace {

// Constructor helper.
std::unique_ptr<impl::CommandBufferPool> NewCommandBufferPool(
    const VulkanContext& context, impl::CommandBufferSequencer* sequencer) {
  return std::make_unique<impl::CommandBufferPool>(context.device, context.queue,
                                                   context.queue_family_index, sequencer, true);
}

// Constructor helper.
std::unique_ptr<impl::CommandBufferPool> NewTransferCommandBufferPool(
    const VulkanContext& context, impl::CommandBufferSequencer* sequencer) {
  if (!context.transfer_queue) {
    return nullptr;
  } else {
    return std::make_unique<impl::CommandBufferPool>(context.device, context.transfer_queue,
                                                     context.transfer_queue_family_index, sequencer,
                                                     false);
  }
}

// Constructor helper.
std::unique_ptr<impl::GpuUploader> NewGpuUploader(EscherWeakPtr escher,
                                                  impl::CommandBufferPool* main_pool,
                                                  impl::CommandBufferPool* transfer_pool,
                                                  GpuAllocator* allocator) {
  return std::make_unique<impl::GpuUploader>(std::move(escher),
                                             transfer_pool ? transfer_pool : main_pool, allocator);
}

// Constructor helper.
std::unique_ptr<impl::MeshManager> NewMeshManager(impl::CommandBufferPool* main_pool,
                                                  impl::CommandBufferPool* transfer_pool,
                                                  GpuAllocator* allocator,
                                                  impl::GpuUploader* uploader,
                                                  ResourceRecycler* resource_recycler) {
  return std::make_unique<impl::MeshManager>(transfer_pool ? transfer_pool : main_pool, allocator,
                                             uploader, resource_recycler);
}

}  // anonymous namespace

Escher::Escher(VulkanDeviceQueuesPtr device) : Escher(std::move(device), HackFilesystem::New()) {}

Escher::Escher(VulkanDeviceQueuesPtr device, HackFilesystemPtr filesystem)
    : renderer_count_(0),
      device_(std::move(device)),
      vulkan_context_(device_->GetVulkanContext()),
      gpu_allocator_(std::make_unique<VmaGpuAllocator>(vulkan_context_)),
      command_buffer_sequencer_(std::make_unique<impl::CommandBufferSequencer>()),
      command_buffer_pool_(NewCommandBufferPool(vulkan_context_, command_buffer_sequencer_.get())),
      transfer_command_buffer_pool_(
          NewTransferCommandBufferPool(vulkan_context_, command_buffer_sequencer_.get())),
      glsl_compiler_(std::make_unique<impl::GlslToSpirvCompiler>()),
      shaderc_compiler_(std::make_unique<shaderc::Compiler>()),
      pipeline_cache_(std::make_unique<impl::PipelineCache>()),
      weak_factory_(this) {
  FXL_DCHECK(vulkan_context_.instance);
  FXL_DCHECK(vulkan_context_.physical_device);
  FXL_DCHECK(vulkan_context_.device);
  FXL_DCHECK(vulkan_context_.queue);
  // TODO: additional validation, e.g. ensure that queue supports both graphics
  // and compute.

  // Initialize instance variables that require |weak_factory_| to already have
  // been initialized.
  image_cache_ = std::make_unique<impl::ImageCache>(GetWeakPtr(), gpu_allocator());
  buffer_cache_ = std::make_unique<BufferCache>(GetWeakPtr());
  gpu_uploader_ = NewGpuUploader(GetWeakPtr(), command_buffer_pool(),
                                 transfer_command_buffer_pool(), gpu_allocator());
  resource_recycler_ = std::make_unique<ResourceRecycler>(GetWeakPtr());
  mesh_manager_ = NewMeshManager(command_buffer_pool(), transfer_command_buffer_pool(),
                                 gpu_allocator(), gpu_uploader(), resource_recycler());
  pipeline_layout_cache_ = std::make_unique<impl::PipelineLayoutCache>(resource_recycler());
  render_pass_cache_ = std::make_unique<impl::RenderPassCache>(resource_recycler());
  framebuffer_allocator_ =
      std::make_unique<impl::FramebufferAllocator>(resource_recycler(), render_pass_cache_.get());
  shader_program_factory_ =
      std::make_unique<DefaultShaderProgramFactory>(GetWeakPtr(), std::move(filesystem));

  frame_manager_ = std::make_unique<impl::FrameManager>(GetWeakPtr());

  // Query relevant Vulkan properties.
  auto device_properties = vk_physical_device().getProperties();
  timestamp_period_ = device_properties.limits.timestampPeriod;
  auto queue_properties =
      vk_physical_device().getQueueFamilyProperties()[vulkan_context_.queue_family_index];
  supports_timer_queries_ = queue_properties.timestampValidBits > 0;
}

Escher::~Escher() {
  FXL_DCHECK(renderer_count_ == 0);
  shader_program_factory_->Clear();
  vk_device().waitIdle();
  Cleanup();

  // Everything that refers to a ResourceRecycler must be released before their
  // ResourceRecycler is.
  framebuffer_allocator_.reset();
  render_pass_cache_.reset();
  pipeline_layout_cache_.reset();
  mesh_manager_.reset();
  descriptor_set_allocators_.clear();

  // ResourceRecyclers must be released before the CommandBufferSequencer is,
  // since they register themselves with it.
  resource_recycler_.reset();
  gpu_uploader_.reset();
  buffer_cache_.reset();
}

bool Escher::Cleanup() {
  bool finished = true;
  finished = command_buffer_pool()->Cleanup() && finished;
  if (auto pool = transfer_command_buffer_pool()) {
    finished = pool->Cleanup() && finished;
  }
  return finished;
}

MeshBuilderPtr Escher::NewMeshBuilder(const MeshSpec& spec, size_t max_vertex_count,
                                      size_t max_index_count) {
  return mesh_manager()->NewMeshBuilder(spec, max_vertex_count, max_index_count);
}

ImagePtr Escher::NewRgbaImage(uint32_t width, uint32_t height, uint8_t* bytes) {
  BatchGpuUploader uploader(GetWeakPtr(), 0);
  ImagePtr image = image_utils::NewRgbaImage(image_cache(), &uploader, width, height, bytes);
  uploader.Submit();
  return image;
}

ImagePtr Escher::NewCheckerboardImage(uint32_t width, uint32_t height) {
  BatchGpuUploader uploader(GetWeakPtr(), 0);
  ImagePtr image = image_utils::NewCheckerboardImage(image_cache(), &uploader, width, height);
  uploader.Submit();
  return image;
}

ImagePtr Escher::NewGradientImage(uint32_t width, uint32_t height) {
  BatchGpuUploader uploader(GetWeakPtr(), 0);
  ImagePtr image = image_utils::NewGradientImage(image_cache(), &uploader, width, height);
  uploader.Submit();
  return image;
}

ImagePtr Escher::NewNoiseImage(uint32_t width, uint32_t height) {
  BatchGpuUploader uploader(GetWeakPtr(), 0);
  ImagePtr image = image_utils::NewNoiseImage(image_cache(), &uploader, width, height);
  uploader.Submit();
  return image;
}

TexturePtr Escher::NewTexture(ImagePtr image, vk::Filter filter, vk::ImageAspectFlags aspect_mask,
                              bool use_unnormalized_coordinates) {
  TRACE_DURATION("gfx", "Escher::NewTexture (from image)");
  return Texture::New(resource_recycler(), std::move(image), filter, aspect_mask,
                      use_unnormalized_coordinates);
}

BufferPtr Escher::NewBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage_flags,
                            vk::MemoryPropertyFlags memory_property_flags) {
  TRACE_DURATION("gfx", "Escher::NewBuffer");
  return gpu_allocator()->AllocateBuffer(resource_recycler(), size, usage_flags,
                                         memory_property_flags);
}

TexturePtr Escher::NewTexture(vk::Format format, uint32_t width, uint32_t height,
                              uint32_t sample_count, vk::ImageUsageFlags usage_flags,
                              vk::Filter filter, vk::ImageAspectFlags aspect_flags,
                              bool use_unnormalized_coordinates) {
  TRACE_DURATION("gfx", "Escher::NewTexture (new image)");
  ImageInfo image_info{.format = format,
                       .width = width,
                       .height = height,
                       .sample_count = sample_count,
                       .usage = usage_flags};
  ImagePtr image = gpu_allocator()->AllocateImage(resource_recycler(), image_info);
  return Texture::New(resource_recycler(), std::move(image), filter, aspect_flags,
                      use_unnormalized_coordinates);
}

TexturePtr Escher::NewAttachmentTexture(vk::Format format, uint32_t width, uint32_t height,
                                        uint32_t sample_count, vk::Filter filter,
                                        vk::ImageUsageFlags usage_flags,
                                        bool is_transient_attachment, bool is_input_attachment,
                                        bool use_unnormalized_coordinates) {
  const auto pair = image_utils::IsDepthStencilFormat(format);
  usage_flags |= (pair.first || pair.second) ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                                             : vk::ImageUsageFlagBits::eColorAttachment;
  if (is_transient_attachment) {
    // TODO(SCN-634): when specifying that it is being used as a transient
    // attachment, we should use lazy memory if supported by the Vulkan
    // device... but only if no non-attachment flags are present.
    // TODO(SCN-634): also, clients should probably just add this usage flag
    // themselves, rather than having a separate bool to do it.
    usage_flags |= vk::ImageUsageFlagBits::eTransientAttachment;
  }
  if (is_input_attachment) {
    usage_flags |= vk::ImageUsageFlagBits::eInputAttachment;
  }
  return NewTexture(format, width, height, sample_count, usage_flags, filter,
                    image_utils::FormatToColorOrDepthStencilAspectFlags(format),
                    use_unnormalized_coordinates);
}

ShaderProgramPtr Escher::GetProgram(const std::string shader_paths[EnumCount<ShaderStage>()],
                                    ShaderVariantArgs args) {
  return shader_program_factory_->GetProgram(shader_paths, std::move(args));
}

FramePtr Escher::NewFrame(const char* trace_literal, uint64_t frame_number, bool enable_gpu_logging,
                          escher::CommandBuffer::Type requested_type) {
  TRACE_DURATION("gfx", "escher::Escher::NewFrame ");

  // Check the type before cycling the framebuffer/descriptor-set allocators.
  // Without these checks it is possible to write into a Vulkan resource before
  // it is finished being used in a previous frame.
  // TODO(ES-103): The correct solution is not to use multiple Frames per frame.
  if (requested_type != CommandBuffer::Type::kTransfer) {
    for (auto& pair : descriptor_set_allocators_) {
      // TODO(ES-199): Nothing calls Clear() on the DescriptorSetAllocators, so
      // their internal allocations are currently able to grow without bound.
      // DescriptorSets are not managed by ResourceRecyclers, so just
      // adding a call to Clear() here would be dangerous.
      pair.second->BeginFrame();
    }
  }
  if (requested_type == CommandBuffer::Type::kGraphics) {
    framebuffer_allocator_->BeginFrame();
  }

  return frame_manager_->NewFrame(trace_literal, frame_number, enable_gpu_logging, requested_type);
}

uint64_t Escher::GetNumGpuBytesAllocated() { return gpu_allocator()->GetTotalBytesAllocated(); }

impl::DescriptorSetAllocator* Escher::GetDescriptorSetAllocator(
    const impl::DescriptorSetLayout& layout, const SamplerPtr& immutable_sampler) {
  TRACE_DURATION("gfx", "escher::Escher::GetDescriptorSetAllocator");
  static_assert(sizeof(impl::DescriptorSetLayout) == 32, "hash code below must be updated");
  Hasher h;
  if (immutable_sampler)
    h.struc(immutable_sampler->vk());
  h.u32(layout.sampled_image_mask);
  h.u32(layout.storage_image_mask);
  h.u32(layout.uniform_buffer_mask);
  h.u32(layout.storage_buffer_mask);
  h.u32(layout.sampled_buffer_mask);
  h.u32(layout.input_attachment_mask);
  h.u32(layout.fp_mask);
  h.u32(static_cast<uint32_t>(layout.stages));
  Hash hash = h.value();

  auto it = descriptor_set_allocators_.find(hash);
  if (it != descriptor_set_allocators_.end()) {
    FXL_DCHECK(layout == it->second->layout()) << "hash collision.";
    return it->second.get();
  }

  TRACE_DURATION("gfx", "escher::Escher::GetDescriptorSetAllocator[creation]");
  auto new_allocator = new impl::DescriptorSetAllocator(vk_device(), layout, immutable_sampler);

  // TODO(ES-200): This hash table never decreases in size. Users of Escher that
  // generate unique descriptor set layouts (e.g., with immutable samplers) can
  // cause this system to cache unbounded amounts of memory.
  descriptor_set_allocators_.emplace_hint(it, hash, new_allocator);
  return new_allocator;
}

}  // namespace escher
