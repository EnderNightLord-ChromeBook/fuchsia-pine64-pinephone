// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lp50xx-light.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include <memory>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <ddktl/fidl.h>
#include <ddktl/protocol/platform/device.h>
#include <fbl/alloc_checker.h>

#include "lp50xx-regs.h"

namespace lp50xx_light {

static bool run_blink_test(void* ctx, zx_device_t* parent, zx_handle_t channel) {
  fbl::AllocChecker ac;
  auto dev = std::unique_ptr<Lp50xxLight>(new (&ac) Lp50xxLight(parent));
  if (!ac.check()) {
    return false;
  }
  auto status = dev->Init();
  if (status != ZX_OK) {
    return false;
  }
  return dev->BlinkTest();
}

bool Lp50xxLight::BlinkTest() {
  zx_status_t status;
  llcpp::fuchsia::hardware::light::Rgb rgb = {};

  for (uint32_t led = 0; led < led_count_; led++) {
    // incrementing color in steps of 16 to reduce time taken for the test
    for (uint32_t red = 0; red <= 0xff; red += 16) {
      for (uint32_t green = 0; green <= 0xff; green += 16) {
        for (uint32_t blue = 0; blue <= 0xff; blue += 16) {
          rgb.red = static_cast<uint8_t>(red);
          rgb.blue = static_cast<uint8_t>(blue);
          rgb.green = static_cast<uint8_t>(green);
          status = SetRgbValue(led, rgb);
          if (status != ZX_OK) {
            zxlogf(ERROR, "%s: Failed to set color R:%d G:%d B:%d\n", __func__, red, green, blue);
          }
          status = GetRgbValue(led, &rgb);
          if (status != ZX_OK) {
            zxlogf(ERROR, "%s: Failed to get color R:%d G:%d B:%d\n", __func__, red, green, blue);
          }
        }
      }
    }
  }

  for (uint32_t i = 0; i < led_count_; i++) {
    rgb.red = 0;
    rgb.green = 0;
    rgb.blue = 0;
    status = SetRgbValue(i, rgb);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: Failed to reset color\n", __PRETTY_FUNCTION__);
    }
  }

  zxlogf(INFO, "Lp50xxLight Blink test complete\n");
  return ZX_OK;
}

zx_status_t Lp50xxLight::Lp50xxRegConfig() {
  switch (pid_) {
    case PDEV_PID_TI_LP5018:
      led_count_ = 6;
      led_color_addr_ = 0x0f;
      reset_addr_ = 0x27;
      break;
    case PDEV_PID_TI_LP5024:
      led_count_ = 8;
      led_color_addr_ = 0x0f;
      reset_addr_ = 0x27;
      break;
    case PDEV_PID_TI_LP5030:
      led_count_ = 10;
      led_color_addr_ = 0x14;
      reset_addr_ = 0x38;
      break;
    case PDEV_PID_TI_LP5036:
      led_count_ = 12;
      led_color_addr_ = 0x14;
      reset_addr_ = 0x38;
      break;
    default:
      zxlogf(ERROR, "%s: unsupported PID %u\n", __func__, pid_);
      return ZX_ERR_NOT_SUPPORTED;
  }

  return ZX_OK;
}

zx_status_t Lp50xxLight::SetRgbValue(uint32_t index, llcpp::fuchsia::hardware::light::Rgb rgb) {
  auto status = RedColorReg::Get(led_color_addr_, index).FromValue(rgb.red).WriteTo(i2c_);
  if (status != ZX_OK) {
    return status;
  }

  status = GreenColorReg::Get(led_color_addr_, index).FromValue(rgb.green).WriteTo(i2c_);
  if (status != ZX_OK) {
    return status;
  }

  status = BlueColorReg::Get(led_color_addr_, index).FromValue(rgb.blue).WriteTo(i2c_);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

zx_status_t Lp50xxLight::GetRgbValue(uint32_t index, llcpp::fuchsia::hardware::light::Rgb* rgb) {
  auto red = RedColorReg::Get(led_color_addr_, index).FromValue(0);
  auto green = GreenColorReg::Get(led_color_addr_, index).FromValue(0);
  auto blue = BlueColorReg::Get(led_color_addr_, index).FromValue(0);

  if (red.ReadFrom(i2c_) || green.ReadFrom(i2c_) || blue.ReadFrom(i2c_)) {
    zxlogf(ERROR, "Failed to read I2C color registers\n");
    return ZX_ERR_INTERNAL;
  }

  rgb->red = red.reg_value();
  rgb->green = green.reg_value();
  rgb->blue = blue.reg_value();

  return ZX_OK;
}

void Lp50xxLight::GetName(uint32_t index, GetNameCompleter::Sync completer) {
  if (index >= led_count_) {
    completer.Reply(ZX_ERR_OUT_OF_RANGE, fidl::StringView(0, nullptr));
    return;
  }

  if (names_.size() > 0) {
    // TODO(puneetha): Currently names_ is not set from metadata. This code will not be executed.
    auto* name = names_[index];
    completer.Reply(ZX_OK, fidl::StringView(strlen(name) + 1, name));
  } else {
    // Return "lp50xx-led-X" if no metadata was provided.
    char name[kNameLength];
    snprintf(name, sizeof(name), "lp50xx-led-%u\n", index);
    completer.Reply(ZX_OK, fidl::StringView(strlen(name) + 1, name));
  }
  return;
}

void Lp50xxLight::GetCount(GetCountCompleter::Sync completer) { completer.Reply(led_count_); }

void Lp50xxLight::HasCapability(uint32_t index,
                                llcpp::fuchsia::hardware::light::Capability capability,
                                HasCapabilityCompleter::Sync completer) {
  if (index >= led_count_) {
    completer.Reply(ZX_ERR_OUT_OF_RANGE, false);
    return;
  }
  completer.Reply(ZX_OK, true);
}

void Lp50xxLight::GetSimpleValue(uint32_t index, GetSimpleValueCompleter::Sync completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED, 0);
}

void Lp50xxLight::SetSimpleValue(uint32_t index, uint8_t value,
                                 SetSimpleValueCompleter::Sync completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED);
}

void Lp50xxLight::GetRgbValue(uint32_t index, GetRgbValueCompleter::Sync completer) {
  llcpp::fuchsia::hardware::light::Rgb rgb = {};
  if (index >= led_count_) {
    completer.Reply(ZX_ERR_OUT_OF_RANGE, rgb);
    return;
  }

  auto status = GetRgbValue(index, &rgb);
  completer.Reply(status, rgb);
}

void Lp50xxLight::SetRgbValue(uint32_t index, llcpp::fuchsia::hardware::light::Rgb value,
                              SetRgbValueCompleter::Sync completer) {
  if (index >= led_count_) {
    completer.Reply(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  auto status = SetRgbValue(index, value);
  completer.Reply(status);
}

zx_status_t Lp50xxLight::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  DdkTransaction transaction(txn);
  llcpp::fuchsia::hardware::light::Light::Dispatch(this, msg, &transaction);
  return transaction.Status();
}

void Lp50xxLight::DdkRelease() { delete this; }

zx_status_t Lp50xxLight::InitHelper() {
  // Get Pdev and I2C protocol.
  composite_protocol_t composite;
  auto status = device_get_protocol(parent(), ZX_PROTOCOL_COMPOSITE, &composite);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Get ZX_PROTOCOL_COMPOSITE failed\n", __func__);
    return status;
  }

  zx_device_t* components[kComponentCount];
  size_t actual;
  composite_get_components(&composite, components, countof(components), &actual);
  if (actual != kComponentCount) {
    zxlogf(ERROR, "Invalid component count (need %d, have %zu)", kComponentCount, actual);
    return ZX_ERR_INTERNAL;
  }

  // status = device_get_protocol(components[kI2cComponent], ZX_PROTOCOL_I2C, &i2c_);
  ddk::I2cProtocolClient i2c(components[kI2cComponent]);
  if (!i2c.is_valid()) {
    zxlogf(ERROR, "ZX_PROTOCOL_I2C not found, err=%d\n", status);
    return status;
  }

  ddk::PDevProtocolClient pdev(components[kPdevComponent]);
  if (!pdev.is_valid()) {
    zxlogf(ERROR, "%s: Get PBusProtocolClient failed\n", __func__);
    return ZX_ERR_INTERNAL;
  }

  pdev_device_info info = {};
  status = pdev.GetDeviceInfo(&info);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: GetDeviceInfo failed: %d\n", __func__, status);
    return ZX_ERR_INTERNAL;
  }
  pid_ = info.pid;
  i2c_ = std::move(i2c);
  return ZX_OK;
}

zx_status_t Lp50xxLight::Init() {
  InitHelper();

  // Set device specific register configuration
  zx_status_t status = Lp50xxRegConfig();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Device register configuration failed %d\n", __func__, status);
    return status;
  }

  // Enable device.
  auto dev_conf0 = DeviceConfig0Reg::Get().FromValue(0);
  dev_conf0.set_chip_enable(1);

  status = dev_conf0.WriteTo(i2c_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Device enable failed %d\n", __func__, status);
    return status;
  }

  // Set Log_Scale_EN, Power_save_EN, Auto_incr_EN and PWm_Dithering_EN
  auto dev_conf1 = DeviceConfig1Reg::Get().FromValue(0);
  dev_conf1.set_log_scale_enable(1);
  dev_conf1.set_power_save_enable(1);
  dev_conf1.set_auto_incr_enable(1);
  dev_conf1.set_pwm_dithering_enable(1);
  status = dev_conf1.WriteTo(i2c_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Device conf1 failed %d\n", __func__, status);
    return status;
  }

  return ZX_OK;
}

zx_status_t Lp50xxLight::Create(void* ctx, zx_device_t* parent) {
  fbl::AllocChecker ac;
  auto dev = std::unique_ptr<Lp50xxLight>(new (&ac) Lp50xxLight(parent));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  auto status = dev->Init();
  if (status != ZX_OK) {
    return status;
  }

  status = dev->DdkAdd("lp50xx-light", DEVICE_ADD_NON_BINDABLE);
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  __UNUSED auto* dummy = dev.release();
  return ZX_OK;
}

static zx_driver_ops_t driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .init = nullptr,
    .bind = Lp50xxLight::Create,
    .create = nullptr,
    .release = nullptr,
    .run_unit_tests = run_blink_test,
};

}  // namespace lp50xx_light

// clang-format off
ZIRCON_DRIVER_BEGIN(lp50xx_light, lp50xx_light::driver_ops, "zircon", "0.1", 7)
  BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
  BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TI),
  BI_ABORT_IF(NE, BIND_PLATFORM_DEV_DID, PDEV_DID_TI_LED),
  BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_TI_LP5018),
  BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_TI_LP5024),
  BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_TI_LP5030),
  BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_TI_LP5036),
ZIRCON_DRIVER_END(lp50xx_light)
//clang-format on
