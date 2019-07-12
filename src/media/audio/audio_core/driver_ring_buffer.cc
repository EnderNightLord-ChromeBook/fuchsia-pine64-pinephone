// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/driver_ring_buffer.h"

#include "src/lib/fxl/logging.h"

namespace media::audio {

// static
fbl::RefPtr<DriverRingBuffer> DriverRingBuffer::Create(zx::vmo vmo, uint32_t frame_size,
                                                       uint32_t frame_count, bool input) {
  auto ret = fbl::AdoptRef(new DriverRingBuffer());

  if (ret->Init(std::move(vmo), frame_size, frame_count, input) != ZX_OK) {
    return nullptr;
  }

  return ret;
}

zx_status_t DriverRingBuffer::Init(zx::vmo vmo, uint32_t frame_size, uint32_t frame_count,
                                   bool input) {
  if (!vmo.is_valid()) {
    FXL_LOG(ERROR) << "Invalid VMO!";
    return ZX_ERR_INVALID_ARGS;
  }

  if (!frame_size) {
    FXL_LOG(ERROR) << "Frame size may not be zero!";
    return ZX_ERR_INVALID_ARGS;
  }

  uint64_t vmo_size;
  zx_status_t res = vmo.get_size(&vmo_size);

  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to get ring buffer VMO size";
    return res;
  }

  uint64_t size = static_cast<uint64_t>(frame_size) * frame_count;
  if (size > vmo_size) {
    FXL_LOG(ERROR) << "Driver-reported ring buffer size (" << size << ") is greater than VMO size ("
                   << vmo_size << ")";
    return res;
  }

  // Map the VMO into our address space.
  // TODO(johngro): How do I specify the cache policy for this mapping?
  zx_vm_option_t flags = ZX_VM_PERM_READ | (input ? 0 : ZX_VM_PERM_WRITE);
  res = vmo_mapper_.Map(vmo, 0u, size, flags);

  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to map ring buffer VMO";
    return res;
  }

  frame_size_ = frame_size;
  frames_ = frame_count;

  return res;
}

}  // namespace media::audio
