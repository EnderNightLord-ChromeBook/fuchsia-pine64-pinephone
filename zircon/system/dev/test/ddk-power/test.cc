// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/device/power/test/llcpp/fidl.h>
#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/directory.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <ddk/platform-defs.h>
#include <zxtest/zxtest.h>

using driver_integration_test::IsolatedDevmgr;
using llcpp::fuchsia::device::Controller;
using llcpp::fuchsia::device::DevicePowerState;
using llcpp::fuchsia::device::DevicePowerStateInfo;
using llcpp::fuchsia::device::SystemPowerStateInfo;
using llcpp::fuchsia::device::MAX_DEVICE_POWER_STATES;
using llcpp::fuchsia::device::power::test::TestDevice;

class PowerTestCase : public zxtest::Test {
  public:
  ~PowerTestCase() override = default;
  void SetUp() override {
    IsolatedDevmgr::Args args;
    args.load_drivers.push_back("/boot/driver/ddk-power-test.so");
    args.load_drivers.push_back("/boot/driver/ddk-power-test-child.so");

    board_test::DeviceEntry dev = {};
    dev.vid = PDEV_VID_TEST;
    dev.pid = PDEV_PID_POWER_TEST;
    dev.did = 0;
    args.device_list.push_back(dev);

    zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr);
    ASSERT_OK(status);
    fbl::unique_fd parent_fd, child_fd;
    ASSERT_OK(devmgr_integration_test::RecursiveWaitForFile(
        devmgr.devfs_root(), "sys/platform/11:0b:0/power-test", &parent_fd));
    ASSERT_GT(parent_fd.get(), 0);

    ASSERT_OK(devmgr_integration_test::RecursiveWaitForFile(
        devmgr.devfs_root(), "sys/platform/11:0b:0/power-test/power-test-child", &child_fd));
    ASSERT_GT(child_fd.get(), 0);

    ASSERT_OK(
        fdio_get_service_handle(child_fd.release(), child_device_handle.reset_and_get_address()));
    ASSERT_NE(child_device_handle.get(), ZX_HANDLE_INVALID);
  }
  void AddChildWithPowerArgs(DevicePowerStateInfo* states, uint8_t count) {
    auto power_states =
        ::fidl::VectorView<DevicePowerStateInfo>(count, reinterpret_cast<DevicePowerStateInfo *>(states));
    auto response = TestDevice::Call::AddDeviceWithPowerArgs(zx::unowned(child_device_handle),
                                                             power_states);
    ASSERT_OK(response.status());
    zx_status_t call_status = ZX_OK;
    if (response->result.is_err()) {
      call_status = response->result.err();
    }
    ASSERT_OK(call_status);

    fbl::unique_fd child2_fd;
    ASSERT_OK(devmgr_integration_test::RecursiveWaitForFile(
        devmgr.devfs_root(), "sys/platform/11:0b:0/power-test/power-test-child-2", &child2_fd));
    ASSERT_GT(child2_fd.get(), 0);
    ASSERT_OK(
        fdio_get_service_handle(child2_fd.release(), child2_device_handle.reset_and_get_address()));
    ASSERT_NE(child2_device_handle.get(), ZX_HANDLE_INVALID);
  }
  zx::channel child_device_handle;
  zx::channel child2_device_handle;
  IsolatedDevmgr devmgr;
};
/*
TEST_F(PowerTestCase, InvalidDevicePowerCaps_Less) {
  DevicePowerStateInfo states[1];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[0].is_supported = true;
  auto power_states =
      ::fidl::VectorView<DevicePowerStateInfo>(1, reinterpret_cast<DevicePowerStateInfo *>(states));
  auto response = TestDevice::Call::AddDeviceWithPowerArgs(zx::unowned(child_device_handle),
                                                           power_states);
  ASSERT_OK(response.status());
  zx_status_t call_status = ZX_OK;
  if (response->result.is_err()) {
    call_status = response->result.err();
  }
  ASSERT_STATUS(call_status, ZX_ERR_INVALID_ARGS);
}

TEST_F(PowerTestCase, InvalidDevicePowerCaps_More) {
  DevicePowerStateInfo states[MAX_DEVICE_POWER_STATES + 1];
  for (uint8_t i = 0; i < MAX_DEVICE_POWER_STATES + 1; i++) {
    states[i].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
    states[i].is_supported = true;
  }
  auto power_states = ::fidl::VectorView<DevicePowerStateInfo>(
      MAX_DEVICE_POWER_STATES + 1, reinterpret_cast<DevicePowerStateInfo *>(states));
  auto response = TestDevice::Call::AddDeviceWithPowerArgs(zx::unowned(child_device_handle),
                                                           power_states);
  ASSERT_OK(response.status());
  zx_status_t call_status = ZX_OK;
  if (response->result.is_err()) {
    call_status = response->result.err();
  }

  ASSERT_STATUS(call_status, ZX_ERR_INVALID_ARGS);
}

TEST_F(PowerTestCase, InvalidDevicePowerCaps_MissingRequired) {
  DevicePowerStateInfo states[MAX_DEVICE_POWER_STATES];
  for (uint8_t i = 0; i < MAX_DEVICE_POWER_STATES; i++) {
    // Missing D0 and D3COLD
    states[i].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
    states[i].is_supported = true;
  }
  auto power_states = ::fidl::VectorView<DevicePowerStateInfo>(
      MAX_DEVICE_POWER_STATES, reinterpret_cast<DevicePowerStateInfo *>(states));
  auto response = TestDevice::Call::AddDeviceWithPowerArgs(zx::unowned(child_device_handle),
                                                           power_states);
  ASSERT_OK(response.status());
  zx_status_t call_status = ZX_OK;
  if (response->result.is_err()) {
    call_status = response->result.err();
  }

  ASSERT_STATUS(call_status, ZX_ERR_INVALID_ARGS);
}

TEST_F(PowerTestCase, InvalidDevicePowerCaps_DuplicateCaps) {
  DevicePowerStateInfo states[MAX_DEVICE_POWER_STATES];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[1].is_supported = true;
  // Repeat
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  auto power_states = ::fidl::VectorView<DevicePowerStateInfo>(
      MAX_DEVICE_POWER_STATES, reinterpret_cast<DevicePowerStateInfo *>(states));
  auto response = TestDevice::Call::AddDeviceWithPowerArgs(zx::unowned(child_device_handle),
                                                           power_states);
  ASSERT_OK(response.status());
  zx_status_t call_status = ZX_OK;
  if (response->result.is_err()) {
    call_status = response->result.err();
  }

  ASSERT_STATUS(call_status, ZX_ERR_INVALID_ARGS);
}

TEST_F(PowerTestCase, AddDevicePowerCaps_Success) {
  DevicePowerStateInfo states[2];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[1].is_supported = true;
  auto power_states =
      ::fidl::VectorView<DevicePowerStateInfo>(2, reinterpret_cast<DevicePowerStateInfo *>(states));
  auto response = TestDevice::Call::AddDeviceWithPowerArgs(zx::unowned(child_device_handle),
                                                           power_states);
  ASSERT_OK(response.status());
  zx_status_t call_status = ZX_OK;
  if (response->result.is_err()) {
    call_status = response->result.err();
  }

  ASSERT_STATUS(call_status, ZX_OK);
}

TEST_F(PowerTestCase, GetDevicePowerCaps_Success) {
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);

  const DevicePowerStateInfo *out_dpstates;
  auto response2 = Controller::Call::GetDevicePowerCaps(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  zx_status_t call_status;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_STATUS(call_status, ZX_OK);
  out_dpstates = &response2->result.response().dpstates[0];

  ASSERT_TRUE(
      out_dpstates[static_cast<uint8_t>(DevicePowerState::DEVICE_POWER_STATE_D0)].is_supported);
  ASSERT_TRUE(
      out_dpstates[static_cast<uint8_t>(DevicePowerState::DEVICE_POWER_STATE_D1)].is_supported);
  ASSERT_EQ(
      out_dpstates[static_cast<uint8_t>(DevicePowerState::DEVICE_POWER_STATE_D1)].restore_latency,
      100);
  ASSERT_TRUE(
      out_dpstates[static_cast<uint8_t>(DevicePowerState::DEVICE_POWER_STATE_D3COLD)].is_supported);
  ASSERT_EQ(out_dpstates[static_cast<uint8_t>(DevicePowerState::DEVICE_POWER_STATE_D3COLD)]
                .restore_latency,
            1000);
}

TEST_F(PowerTestCase, Suspend_Success) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);
  zx_status_t call_status;

  auto suspend_result = Controller::Call::Suspend(zx::unowned(child2_device_handle),
                                                  DevicePowerState::DEVICE_POWER_STATE_D3COLD);
  ASSERT_OK(suspend_result.status());
  const auto &suspend_response = suspend_result.value();
  ASSERT_OK(suspend_response.status);
  ASSERT_EQ(suspend_response.out_state, DevicePowerState::DEVICE_POWER_STATE_D3COLD);

  auto response2 =
      TestDevice::Call::GetCurrentDevicePowerState(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  call_status = ZX_OK;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_OK(call_status);
  ASSERT_EQ(response2->result.response().cur_state, DevicePowerState::DEVICE_POWER_STATE_D3COLD);
}

TEST_F(PowerTestCase, Resume_Success) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);
  zx_status_t call_status;

  auto suspend_result = Controller::Call::Suspend(zx::unowned(child2_device_handle),
                                                  DevicePowerState::DEVICE_POWER_STATE_D3COLD);
  ASSERT_OK(suspend_result.status());
  const auto &suspend_response = suspend_result.value();
  ASSERT_OK(suspend_response.status);
  ASSERT_EQ(suspend_response.out_state, DevicePowerState::DEVICE_POWER_STATE_D3COLD);

  auto response2 =
      TestDevice::Call::GetCurrentDevicePowerState(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  call_status = ZX_OK;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_OK(call_status);
  ASSERT_EQ(response2->result.response().cur_state, DevicePowerState::DEVICE_POWER_STATE_D3COLD);

  auto resume_result = Controller::Call::Resume(zx::unowned(child2_device_handle),
                                                DevicePowerState::DEVICE_POWER_STATE_D0);
  ASSERT_OK(resume_result.status());

  call_status = ZX_OK;
  if (resume_result->result.is_err()) {
    call_status = resume_result->result.err();
  }
  ASSERT_OK(call_status);
  ASSERT_EQ(resume_result->result.response().out_state, DevicePowerState::DEVICE_POWER_STATE_D0);

  auto response3 =
      TestDevice::Call::GetCurrentDevicePowerState(zx::unowned(child2_device_handle));
  ASSERT_OK(response3.status());
  call_status = ZX_OK;
  if (response3->result.is_err()) {
    call_status = response3->result.err();
  }
  ASSERT_OK(call_status);
  ASSERT_EQ(response3->result.response().cur_state, DevicePowerState::DEVICE_POWER_STATE_D0);
}

TEST_F(PowerTestCase, DefaultSystemPowerStatesMapping) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);

  const SystemPowerStateInfo *states_mapping;
  auto response2 = Controller::Call::GetPowerStateMapping(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  zx_status_t call_status = ZX_OK;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_STATUS(call_status, ZX_OK);
  states_mapping = &response2->result.response().mapping[0];

  // Test Default mapping
  ASSERT_EQ(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].dev_state,
      DevicePowerState::DEVICE_POWER_STATE_D3COLD);
  ASSERT_FALSE(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].wakeup_enable);
}

TEST_F(PowerTestCase, UpdatePowerStatesMapping_UnsupportedDevicestate) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);

  ::fidl::Array<SystemPowerStateInfo, fuchsia_device_manager_MAX_SYSTEM_POWER_STATES> mapping{};
  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    mapping[i].dev_state = DevicePowerState::DEVICE_POWER_STATE_D2;
    mapping[i].wakeup_enable = false;
  }

  auto update_result = 
    Controller::Call::UpdatePowerStateMapping(zx::unowned(child2_device_handle),
                                              mapping);
  ASSERT_OK(update_result.status());
  zx_status_t call_status = ZX_OK;
  if (update_result->result.is_err()) {
    call_status = update_result->result.err();
  }
  ASSERT_EQ(call_status, ZX_ERR_INVALID_ARGS);

  const SystemPowerStateInfo *states_mapping;
  auto response2 = Controller::Call::GetPowerStateMapping(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  call_status = ZX_OK;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_STATUS(call_status, ZX_OK);
  states_mapping = &response2->result.response().mapping[0];

  ASSERT_EQ(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].dev_state,
      DevicePowerState::DEVICE_POWER_STATE_D3COLD);
  ASSERT_FALSE(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].wakeup_enable);
}

TEST_F(PowerTestCase, UpdatePowerStatesMapping_UnsupportedWakeConfig) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[1].wakeup_capable = false;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);

  ::fidl::Array<SystemPowerStateInfo, fuchsia_device_manager_MAX_SYSTEM_POWER_STATES> mapping{};
  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    mapping[i].dev_state = DevicePowerState::DEVICE_POWER_STATE_D1;
    mapping[i].wakeup_enable = true;
  }

  auto update_result = 
    Controller::Call::UpdatePowerStateMapping(zx::unowned(child2_device_handle),
                                              mapping);
  ASSERT_OK(update_result.status());
  zx_status_t call_status = ZX_OK;
  if (update_result->result.is_err()) {
    call_status = update_result->result.err();
  }
  ASSERT_EQ(call_status, ZX_ERR_INVALID_ARGS);

  const SystemPowerStateInfo *states_mapping;
  auto response2 = Controller::Call::GetPowerStateMapping(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  call_status = ZX_OK;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_STATUS(call_status, ZX_OK);
  states_mapping = &response2->result.response().mapping[0];

  ASSERT_EQ(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].dev_state,
      DevicePowerState::DEVICE_POWER_STATE_D3COLD);
  ASSERT_FALSE(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].wakeup_enable);
}

TEST_F(PowerTestCase, UpdatePowerStatesMapping_success) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);

  ::fidl::Array<SystemPowerStateInfo, fuchsia_device_manager_MAX_SYSTEM_POWER_STATES> mapping{};
  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    mapping[i].dev_state = DevicePowerState::DEVICE_POWER_STATE_D1;
    mapping[i].wakeup_enable = false;
  }

  auto update_result = 
    Controller::Call::UpdatePowerStateMapping(zx::unowned(child2_device_handle),
                                              mapping);
  ASSERT_OK(update_result.status());
  zx_status_t call_status = ZX_OK;
  if (update_result->result.is_err()) {
    call_status = update_result->result.err();
  }
  ASSERT_OK(call_status);

  const SystemPowerStateInfo *states_mapping;
  auto response2 = Controller::Call::GetPowerStateMapping(zx::unowned(child2_device_handle));
  ASSERT_OK(response2.status());
  call_status = ZX_OK;
  if (response2->result.is_err()) {
    call_status = response2->result.err();
  }
  ASSERT_STATUS(call_status, ZX_OK);
  states_mapping = &response2->result.response().mapping[0];

  ASSERT_EQ(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].dev_state,
      DevicePowerState::DEVICE_POWER_STATE_D1);
  ASSERT_FALSE(states_mapping[fuchsia_device_manager_SystemPowerState_SYSTEM_POWER_STATE_REBOOT].wakeup_enable);
}*/

TEST_F(PowerTestCase, SystemSuspend) {
  // Add Capabilities
  DevicePowerStateInfo states[3];
  states[0].state_id = DevicePowerState::DEVICE_POWER_STATE_D0;
  states[0].is_supported = true;
  states[0].restore_latency = 0;
  states[1].state_id = DevicePowerState::DEVICE_POWER_STATE_D1;
  states[1].is_supported = true;
  states[1].restore_latency = 100;
  states[2].state_id = DevicePowerState::DEVICE_POWER_STATE_D3COLD;
  states[2].is_supported = true;
  states[2].restore_latency = 1000;
  AddChildWithPowerArgs(states, 3);

  ::fidl::Array<SystemPowerStateInfo, fuchsia_device_manager_MAX_SYSTEM_POWER_STATES> mapping{};
  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    mapping[i].dev_state = DevicePowerState::DEVICE_POWER_STATE_D1;
    mapping[i].wakeup_enable = false;
  }

  auto update_result = 
    Controller::Call::UpdatePowerStateMapping(zx::unowned(child2_device_handle),
                                              mapping);
  ASSERT_OK(update_result.status());
  zx_status_t call_status = ZX_OK;
  if (update_result->result.is_err()) {
    call_status = update_result->result.err();
  }
  ASSERT_OK(call_status);


  // MY SUSPEND
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  ASSERT_OK(status);

  status = fdio_service_connect("/svc/fuchsia.device.manager.Administrator", remote.release());
  ASSERT_OK(status);

  call_status = ZX_OK;
  status = fuchsia_device_manager_AdministratorSuspend(
      local.get(), fuchsia_device_manager_SUSPEND_FLAG_SUSPEND_RAM, &call_status);
  ASSERT_OK(status);
}
