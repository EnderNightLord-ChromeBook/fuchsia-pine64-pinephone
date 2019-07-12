// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dma-mgr.h"

#include <lib/syslog/global.h>

#include <cstdint>

#include "../mali-009/pingpong_regs.h"
#include "dma-format.h"

namespace camera {

zx_status_t DmaManager::Create(const zx::bti& bti, ddk::MmioView isp_mmio_local,
                               DmaManager::Stream stream_type,
                               std::unique_ptr<DmaManager>* out) {
  *out = std::make_unique<DmaManager>(stream_type, isp_mmio_local);

  zx_status_t status = bti.duplicate(ZX_RIGHT_SAME_RIGHTS, &(*out)->bti_);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Unable to duplicate bti for DmaManager \n",
            __func__);
    return status;
  }

  return ZX_OK;
}

auto DmaManager::GetPrimaryMisc() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Primary::DmaWriter_Misc::Get();
  } else {
    return ping::FullResolution::Primary::DmaWriter_Misc::Get();
  }
}

auto DmaManager::GetUvMisc() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Uv::DmaWriter_Misc::Get();
  } else {
    return ping::FullResolution::Uv::DmaWriter_Misc::Get();
  }
}

auto DmaManager::GetPrimaryBank0() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Primary::DmaWriter_Bank0Base::Get();
  } else {
    return ping::FullResolution::Primary::DmaWriter_Bank0Base::Get();
  }
}

auto DmaManager::GetUvBank0() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Uv::DmaWriter_Bank0Base::Get();
  } else {
    return ping::FullResolution::Uv::DmaWriter_Bank0Base::Get();
  }
}

auto DmaManager::GetPrimaryLineOffset() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Primary::DmaWriter_LineOffset::Get();
  } else {
    return ping::FullResolution::Primary::DmaWriter_LineOffset::Get();
  }
}

auto DmaManager::GetUvLineOffset() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Uv::DmaWriter_LineOffset::Get();
  } else {
    return ping::FullResolution::Uv::DmaWriter_LineOffset::Get();
  }
}

auto DmaManager::GetPrimaryActiveDim() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Primary::DmaWriter_ActiveDim::Get();
  } else {
    return ping::FullResolution::Primary::DmaWriter_ActiveDim::Get();
  }
}

auto DmaManager::GetUvActiveDim() {
  if (stream_type_ == Stream::Downscaled) {
    return ping::DownScaled::Uv::DmaWriter_ActiveDim::Get();
  } else {
    return ping::FullResolution::Uv::DmaWriter_ActiveDim::Get();
  }
}

zx_status_t DmaManager::Start(
    fuchsia_sysmem_BufferCollectionInfo buffer_collection,
    fit::function<void(fuchsia_camera_common_FrameAvailableEvent)>
        frame_available_callback) {
  current_format_ = DmaFormat(buffer_collection.format.image);
  // TODO(CAM-54): Provide a way to dump the previous set of write locked
  // buffers.
  write_locked_buffers_.clear();

  if (current_format_->GetImageSize() > buffer_collection.vmo_size) {
    FX_LOGF(ERROR, "", "%s: Buffer size (%lu) is less than image size (%lu)!\n",
            __func__, buffer_collection.vmo_size,
            current_format_->GetImageSize());
    return ZX_ERR_INTERNAL;
  }
  if (buffer_collection.buffer_count > countof(buffer_collection.vmos)) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx::vmo vmos[countof(buffer_collection.vmos)];
  for (uint32_t i = 0; i < buffer_collection.buffer_count; ++i) {
    vmos[i] = zx::vmo(buffer_collection.vmos[i]);
  }
  // Pin the buffers
  zx_status_t status = buffers_.Init(vmos, buffer_collection.buffer_count);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Unable to initialize buffers for DmaManager \n",
            __func__);
    return status;
  }
  // Release the vmos so that the buffer collection could be reused.
  for (uint32_t i = 0; i < buffer_collection.buffer_count; ++i) {
    buffer_collection.vmos[i] = vmos[i].release();
  }
  status = buffers_.PinVmos(bti_, fzl::VmoPool::RequireContig::Yes,
                            fzl::VmoPool::RequireLowMem::Yes);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, "", "%s: Unable to pin buffers for DmaManager \n", __func__);
    return status;
  }
  frame_available_callback_ = std::move(frame_available_callback);
  enabled_ = true;
  return ZX_OK;
}

void DmaManager::OnPrimaryFrameWritten() {
  if (!current_format_->HasSecondaryChannel() || secondary_frame_written_) {
    secondary_frame_written_ = false;
    OnFrameWritten();
  } else {
    primary_frame_written_ = true;
  }
}

void DmaManager::OnSecondaryFrameWritten() {
  if (primary_frame_written_) {
    primary_frame_written_ = false;
    OnFrameWritten();
  } else {
    secondary_frame_written_ = true;
  }
}

void DmaManager::OnFrameWritten() {
  ZX_ASSERT(frame_available_callback_ != nullptr);
  ZX_ASSERT(write_locked_buffers_.size() > 0);
  fuchsia_camera_common_FrameAvailableEvent event;
  event.buffer_id = write_locked_buffers_.back().ReleaseWriteLockAndGetIndex();
  event.frame_status = fuchsia_camera_common_FrameStatus_OK;
  // TODO(garratt): set metadata
  event.metadata.timestamp = 0;
  frame_available_callback_(event);
  write_locked_buffers_.pop_back();
}

// Called as one of the later steps when a new frame arrives.
void DmaManager::OnNewFrame() {
  // If we have not initialized yet with a format, just skip.
  if (!enabled_) {
    return;
  }
  // 1) Get another buffer
  auto buffer = buffers_.LockBufferForWrite();
  if (!buffer) {
    FX_LOG(ERROR, "", "Failed to get buffer\n");
    // TODO(garratt): what should we do when we run out of buffers?
    // If we run out of buffers, disable write and send the callback for
    // out of buffers:
    // clang-format off
        GetPrimaryMisc().ReadFrom(&isp_mmio_local_)
           .set_frame_write_on(0)
           .WriteTo(&isp_mmio_local_);
        if (current_format_->HasSecondaryChannel()) {
          GetUvMisc().ReadFrom(&isp_mmio_local_)
              .set_frame_write_on(0)
              .WriteTo(&isp_mmio_local_);
        }
    // clang-format on
    // Send callback:
    fuchsia_camera_common_FrameAvailableEvent event;
    event.buffer_id = 0;
    event.frame_status = fuchsia_camera_common_FrameStatus_ERROR_BUFFER_FULL;
    event.metadata.timestamp = 0;
    frame_available_callback_(event);
    return;
  }
  // 2) Optional?  Set the DMA settings again... seems unnecessary
  // 3) Set the DMA address
  uint32_t memory_address = static_cast<uint32_t>(buffer->physical_address());

  // clang-format off
    GetPrimaryBank0().FromValue(0)
      .set_value(memory_address + current_format_->GetBank0Offset())
      .WriteTo(&isp_mmio_local_);
    if (current_format_->HasSecondaryChannel()) {
        GetUvBank0().FromValue(0)
          .set_value(memory_address + current_format_->GetBank0OffsetUv())
          .WriteTo(&isp_mmio_local_);
    }
    // 4) Optional? Enable Write_on
    GetPrimaryMisc().ReadFrom(&isp_mmio_local_)
        .set_frame_write_on(1)
        .WriteTo(&isp_mmio_local_);
    if (current_format_->HasSecondaryChannel()) {
        GetUvMisc().ReadFrom(&isp_mmio_local_)
            .set_frame_write_on(1)
            .WriteTo(&isp_mmio_local_);
    }
  // clang-format on
  WriteFormat();
  // Add buffer to queue of buffers we are writing:
  write_locked_buffers_.push_front(std::move(*buffer));
}

zx_status_t DmaManager::ReleaseFrame(uint32_t buffer_index) {
  return buffers_.ReleaseBuffer(buffer_index);
}

void DmaManager::WriteFormat() {
  // Write format to registers
  // clang-format off
    GetPrimaryMisc().ReadFrom(&isp_mmio_local_)
        .set_base_mode(current_format_->GetBaseMode())
        .set_plane_select(current_format_->GetPlaneSelect())
        .WriteTo(&isp_mmio_local_);
    GetPrimaryActiveDim().ReadFrom(&isp_mmio_local_)
        .set_active_width(current_format_->width())
        .set_active_height(current_format_->height())
        .WriteTo(&isp_mmio_local_);
    GetPrimaryLineOffset().ReadFrom(&isp_mmio_local_)
        .set_value(current_format_->GetLineOffset())
        .WriteTo(&isp_mmio_local_);
    if (current_format_->HasSecondaryChannel()) {
        // TODO: should there be a format.WidthUv() ?
        GetUvMisc().ReadFrom(&isp_mmio_local_)
            .set_base_mode(current_format_->GetBaseMode())
            .set_plane_select(current_format_->GetPlaneSelect())
            .WriteTo(&isp_mmio_local_);
        GetUvActiveDim().ReadFrom(&isp_mmio_local_)
            .set_active_width(current_format_->width())
            .set_active_height(current_format_->height())
            .WriteTo(&isp_mmio_local_);
        GetUvLineOffset().ReadFrom(&isp_mmio_local_)
            .set_value(current_format_->GetLineOffset())
            .WriteTo(&isp_mmio_local_);
    }
  // clang-format on
}
}  // namespace camera
