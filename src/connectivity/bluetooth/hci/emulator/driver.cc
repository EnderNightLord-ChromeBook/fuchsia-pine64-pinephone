// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/test.h>

#include <zircon/status.h>

#include <cstdio>
#include <future>
#include <memory>
#include <thread>

#include "src/connectivity/bluetooth/hci/emulator/log.h"

namespace {

zx_status_t DriverBind(void* ctx, zx_device_t* device) {
  logf(TRACE, "DriverBind\n");

  auto dev = std::make_unique<bt_hci_emulator::Device>(device);
  zx_status_t status = dev->Bind();
  if (status != ZX_OK) {
    logf(ERROR, "failed to bind: %s\n", zx_status_get_string(status));
  } else {
    dev.release();
  }

  return status;
}

static constexpr zx_driver_ops_t bt_hci_emulator_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = DriverBind,
};

}  // namespace

// clang-format off
ZIRCON_DRIVER_BEGIN(bt_hci_emulator, bt_hci_emulator_driver_ops, "zircon", "0.1", 2)
  BI_ABORT_IF_AUTOBIND,
  BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST),
ZIRCON_DRIVER_END(bt_hci_emulator)
