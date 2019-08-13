// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_GPU_MSD_IMG_RGX_NO_HARDWARE_NO_HARDWARE_H_
#define GARNET_DRIVERS_GPU_MSD_IMG_RGX_NO_HARDWARE_NO_HARDWARE_H_

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <fuchsia/gpu/magma/c/fidl.h>
#include <lib/fidl-utils/bind.h>

#include <memory>
#include <mutex>

#include "img-sys-device.h"
#include "magma_util/macros.h"
#include "sys_driver/magma_driver.h"

class NoHardwareGpu;

using DeviceType = ddk::Device<NoHardwareGpu, ddk::Messageable>;

class NoHardwareGpu : public DeviceType, public ImgSysDevice {
 public:
  NoHardwareGpu(zx_device_t* parent) : DeviceType(parent) {}

  virtual ~NoHardwareGpu();

  zx_status_t Bind();
  void DdkRelease();

  // DDKTL method that dispatches FIDL messages from clients.
  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);

  zx_status_t Query(uint64_t query_id, fidl_txn_t* transaction);
  zx_status_t QueryReturnsBuffer(uint64_t query_id, fidl_txn_t* transaction);
  zx_status_t Connect(uint64_t client_id, fidl_txn_t* transaction);
  zx_status_t DumpState(uint32_t dump_type);
  zx_status_t Restart();

  zx_status_t PowerUp() override;
  zx_status_t PowerDown() override;
  void* device() override { return parent(); }

 private:
  bool StartMagma() MAGMA_REQUIRES(magma_mutex_);
  void StopMagma() MAGMA_REQUIRES(magma_mutex_);

  std::mutex magma_mutex_;
  std::unique_ptr<MagmaDriver> magma_driver_ MAGMA_GUARDED(magma_mutex_);
  std::shared_ptr<MagmaSystemDevice> magma_system_device_ MAGMA_GUARDED(magma_mutex_);
};

#endif  // GARNET_DRIVERS_GPU_MSD_IMG_RGX_NO_HARDWARE_NO_HARDWARE_H_
