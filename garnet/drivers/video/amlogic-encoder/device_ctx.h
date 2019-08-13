// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_VIDEO_AMLOGIC_ENCODER_DEVICE_CTX_H_
#define GARNET_DRIVERS_VIDEO_AMLOGIC_ENCODER_DEVICE_CTX_H_

#include <ddk/driver.h>
#include <memory>

#include "driver_ctx.h"

// Manages context for the device's lifetime.
class DeviceCtx {
 public:
  // Creates and binds an instance of `DeviceCtx`. If successful, returns ZX_OK
  // and a bound instance of `DeviceCtx`, otherwise an error status and
  // `nullptr`.
  static std::pair<zx_status_t, std::unique_ptr<DeviceCtx>> Bind(DriverCtx* driver_ctx,
                                                                 zx_device_t* parent);

 private:
  DeviceCtx(DriverCtx* driver_ctx);

  DriverCtx* driver_ctx_;
};

#endif  // GARNET_DRIVERS_VIDEO_AMLOGIC_ENCODER_DEVICE_CTX_H_
