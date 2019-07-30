// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_ISP_MALI_009_ARM_ISP_TEST_H_
#define SRC_CAMERA_DRIVERS_ISP_MALI_009_ARM_ISP_TEST_H_

#include <fuchsia/camera/test/c/fidl.h>
#include <lib/fidl-utils/bind.h>
#include <lib/fit/function.h>
#include <zircon/fidl.h>

#include <memory>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/mutex.h>

#include "global_regs.h"
#include "pingpong_regs.h"

namespace camera {
// |ArmIspDeviceTester| is spawned by the driver in |arm-isp.cc|
// This provides the interface provided in fuchsia-camera-test/isp.fidl in
// Zircon.

// Organizes the data from a register dump.
struct ArmIspRegisterDump {
  uint32_t global_config[kGlobalConfigSize];
  uint32_t ping_config[kContextConfigSize];
  uint32_t pong_config[kContextConfigSize];
};

class ArmIspDevice;

class ArmIspDeviceTester;
using IspDeviceTesterType = ddk::Device<ArmIspDeviceTester, ddk::Unbindable, ddk::Messageable>;

class ArmIspDeviceTester : public IspDeviceTesterType,
                           public ddk::EmptyProtocol<ZX_PROTOCOL_ISP_TEST> {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ArmIspDeviceTester);

  explicit ArmIspDeviceTester(ArmIspDevice* isp, zx_device_t* parent)
      : IspDeviceTesterType(parent), isp_(isp) {}

  // On successful creation, |on_isp_unbind| is filled with a pointer to the
  // Disconnect function, so that the ArmIspDevice can notify the
  // ArmIspDeviceTester that it is going away.
  static zx_status_t Create(ArmIspDevice* isp, fit::callback<void()>* on_isp_unbind);

  // Methods required by the ddk.
  void DdkRelease();
  void DdkUnbind();
  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);

 private:
  // DDKMessage Helper Functions.
  zx_status_t RunTests(fidl_txn_t* txn);

  // Disconnects this instance from the ArmIspDevice it is testing.
  // This should only be called when the ArmIspDevice is going away, because
  // it makes this class rather useless.
  void Disconnect();

  static constexpr fuchsia_camera_test_IspTester_ops isp_tester_ops = {
      .RunTests = fidl::Binder<ArmIspDeviceTester>::BindMember<&ArmIspDeviceTester::RunTests>,
  };

  // ISP Tests:
  // Test the GetRegisters interface by writing to a register.
  // |report| is updated with the results of the tests this function performs.
  void TestWriteRegister(fuchsia_camera_test_TestReport& report);

  // The ArmIspDevice is a parent of the ArmIspDeviceTester.  It will call
  // Disconnect() during its DdkUnbind() call, so that isp_ never references an
  // invalid instance. The isp_lock_ ensures that isp_ won't be removed while we
  // are using it.
  fbl::Mutex isp_lock_;
  ArmIspDevice* isp_ __TA_GUARDED(isp_lock_);
};

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_ISP_MALI_009_ARM_ISP_TEST_H_
