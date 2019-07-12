// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_HW_ACCEL_GDC_GDC_H_
#define SRC_CAMERA_DRIVERS_HW_ACCEL_GDC_GDC_H_

#include <ddk/platform-defs.h>
#ifndef _ALL_SOURCE
#define _ALL_SOURCE  // Enables thrd_create_with_name in <threads.h>.
#include <threads.h>
#endif
#include <ddk/protocol/platform/bus.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/gdc.h>
#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/mutex.h>
#include <fbl/unique_ptr.h>
#include <hw/reg.h>
#include <lib/device-protocol/pdev.h>
#include <lib/device-protocol/platform-device.h>
#include <lib/fidl-utils/bind.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/event.h>
#include <lib/zx/interrupt.h>
#include <zircon/fidl.h>

#include <atomic>
#include <deque>
#include <unordered_map>

#include "task.h"

namespace gdc {
// |GdcDevice| is spawned by the driver in |gdc.cc|
namespace {

constexpr uint64_t kPortKeyIrqMsg = 0x00;
constexpr uint64_t kPortKeyDebugFakeInterrupt = 0x01;

}  // namespace
// This provides ZX_PROTOCOL_GDC.
class GdcDevice;
using GdcDeviceType = ddk::Device<GdcDevice, ddk::Unbindable>;

class GdcDevice : public GdcDeviceType, public ddk::GdcProtocol<GdcDevice, ddk::base_protocol> {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(GdcDevice);
  explicit GdcDevice(zx_device_t* parent, ddk ::MmioBuffer clk_mmio, ddk ::MmioBuffer gdc_mmio,
                     zx::interrupt gdc_irq, zx::bti bti, zx::port port)
      : GdcDeviceType(parent),
        port_(std::move(port)),
        clock_mmio_(std::move(clk_mmio)),
        gdc_mmio_(std::move(gdc_mmio)),
        gdc_irq_(std::move(gdc_irq)),
        bti_(std::move(bti)) {}

  ~GdcDevice() = default;

  // Setup() is used to create an instance of GdcDevice.
  // It sets up the pdev & brings the GDC out of reset.
  static zx_status_t Setup(void* ctx, zx_device_t* parent, std::unique_ptr<GdcDevice>* out);

  // Methods required by the ddk.
  void DdkRelease();
  void DdkUnbind();

  // ZX_PROTOCOL_GDC (Refer to gdc.banjo for documentation).
  zx_status_t GdcInitTask(const buffer_collection_info_t* input_buffer_collection,
                          const buffer_collection_info_t* output_buffer_collection,
                          zx::vmo config_vmo, const gdc_callback_t* callback,
                          uint32_t* out_task_index);
  zx_status_t GdcProcessFrame(uint32_t task_index, uint32_t input_buffer_index);
  void GdcRemoveTask(uint32_t task_index);
  void GdcReleaseFrame(uint32_t task_index, uint32_t buffer_index);

  // Used for unit tests.
  const ddk::MmioBuffer* gdc_mmio() const { return &gdc_mmio_; }
  zx_status_t StartThread();
  zx_status_t StopThread();

 protected:
  struct TaskInfo {
    Task* task;
    uint32_t input_buffer_index;
  };

  zx::port port_;

 private:
  friend class GdcDeviceTester;

  // All necessary clean up is done here in ShutDown().
  void ShutDown();
  void InitClocks();
  int FrameProcessingThread();
  int JoinThread() { return thrd_join(processing_thread_, nullptr); }
  void Start();
  void Stop();

  void ProcessTask(TaskInfo& info);
  zx_status_t WaitForInterrupt(zx_port_packet_t* packet);

  // Used to access the processing queue.
  fbl::Mutex lock_;
  fbl::Mutex output_vmo_pool_lock_;

  // HHI register block has the clock registers
  ddk::MmioBuffer clock_mmio_;
  ddk::MmioBuffer gdc_mmio_;
  zx::interrupt gdc_irq_;
  zx::bti bti_;
  uint32_t next_task_index_ = 0;
  std::unordered_map<uint32_t, std::unique_ptr<Task>> task_map_;
  std::deque<TaskInfo> processing_queue_ __TA_GUARDED(lock_);
  thrd_t processing_thread_;
  fbl::ConditionVariable frame_processing_signal_ __TA_GUARDED(lock_);
  bool shutdown_ __TA_GUARDED(lock_) = false;
};

}  // namespace gdc

#endif  // SRC_CAMERA_DRIVERS_HW_ACCEL_GDC_GDC_H_
