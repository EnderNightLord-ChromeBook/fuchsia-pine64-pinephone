// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/power/test/llcpp/fidl.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>

using llcpp::fuchsia::device::DevicePowerStateInfo;
using llcpp::fuchsia::device::power::test::TestDevice;

class TestPowerDriverChild;
using DeviceType = ddk::Device<TestPowerDriverChild, ddk::Unbindable, ddk::Messageable,
                               ddk::SuspendableNew, ddk::ResumableNew>;
class TestPowerDriverChild : public DeviceType, public TestDevice::Interface {
 public:
  TestPowerDriverChild(zx_device_t* parent) : DeviceType(parent) {}
  static zx_status_t Create(void* ctx, zx_device_t* device);
  zx_status_t Bind();
  void DdkUnbind() { DdkRemove(); }

  void AddDeviceWithPowerArgs(::fidl::VectorView<DevicePowerStateInfo> info,
                              AddDeviceWithPowerArgsCompleter::Sync completer) override;

  void GetCurrentDevicePowerState(GetCurrentDevicePowerStateCompleter::Sync completer) override;
  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
    DdkTransaction transaction(txn);
    ::llcpp::fuchsia::device::power::test::TestDevice::Dispatch(this, msg, &transaction);
    return transaction.Status();
  }

  void DdkRelease() { delete this; }
  zx_status_t DdkSuspendNew(uint8_t requested_state, bool enable_wake, uint8_t* out_state);
  zx_status_t DdkResumeNew(uint8_t requested_state, uint8_t* out_state);

 private:
  uint8_t current_power_state_ = 0;
};

zx_status_t TestPowerDriverChild::DdkSuspendNew(uint8_t requested_state, bool enable_wake,
                                                uint8_t* out_state) {
  current_power_state_ = requested_state;
  *out_state = requested_state;
  return ZX_OK;
}

zx_status_t TestPowerDriverChild::DdkResumeNew(uint8_t requested_state, uint8_t* out_state) {
  current_power_state_ = requested_state;
  printf("MINE MINE Reached test-driver-child current_power_state: %d\n", current_power_state_);
  *out_state = requested_state;
  return ZX_OK;
}

void TestPowerDriverChild::AddDeviceWithPowerArgs(::fidl::VectorView<DevicePowerStateInfo> info,
                                                  AddDeviceWithPowerArgsCompleter::Sync completer) {
  ::llcpp::fuchsia::device::power::test::TestDevice_AddDeviceWithPowerArgs_Result response;
  fbl::AllocChecker ac;
  auto child2 = fbl::make_unique_checked<TestPowerDriverChild>(&ac, this->parent());
  if (!ac.check()) {
    response.set_err(ZX_ERR_NO_MEMORY);
    completer.Reply(std::move(response));
    return;
  }

  auto state_info = info.data();
  auto states = std::make_unique<device_power_state_info_t[]>(info.count());
  auto count = static_cast<uint8_t>(info.count());
  for (uint8_t i = 0; i < count; i++) {
    states[i].state_id = static_cast<fuchsia_device_DevicePowerState>(state_info[i].state_id);
    states[i].restore_latency = state_info[i].restore_latency;
    states[i].wakeup_capable = state_info[i].wakeup_capable;
    states[i].system_wake_state = state_info[i].system_wake_state;
  }
  zx_status_t status = child2->DdkAdd("power-test-child-2", 0, nullptr, 0, 0, nullptr,
                                      ZX_HANDLE_INVALID, states.get(), count);
  if (status != ZX_OK) {
    response.set_err(status);
  } else {
    response.set_response(
        llcpp::fuchsia::device::power::test::TestDevice_AddDeviceWithPowerArgs_Response{});
    __UNUSED auto ptr = child2.release();
  }
  completer.Reply(std::move(response));
}

void TestPowerDriverChild::GetCurrentDevicePowerState(
    GetCurrentDevicePowerStateCompleter::Sync completer) {
  ::llcpp::fuchsia::device::power::test::TestDevice_GetCurrentDevicePowerState_Result result;
  result.set_response(
      llcpp::fuchsia::device::power::test::TestDevice_GetCurrentDevicePowerState_Response{
          .cur_state = static_cast<llcpp::fuchsia::device::DevicePowerState>(current_power_state_),
      });

  completer.Reply(std::move(result));
}

zx_status_t TestPowerDriverChild::Bind() { return DdkAdd("power-test-child"); }

zx_status_t TestPowerDriverChild::Create(void* ctx, zx_device_t* device) {
  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<TestPowerDriverChild>(&ac, device);

  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  auto status = dev->Bind();
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    __UNUSED auto ptr = dev.release();
  }
  return status;
}

static zx_driver_ops_t test_power_child_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = TestPowerDriverChild::Create;
  return ops;
}();

// clang-format off
ZIRCON_DRIVER_BEGIN(TestPowerChild, test_power_child_driver_ops, "zircon", "0.1", 1)
  BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_TEST_POWER_CHILD),
ZIRCON_DRIVER_END(TestPowerChild)
    // clang-format on
