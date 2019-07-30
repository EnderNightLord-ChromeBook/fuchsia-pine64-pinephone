// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_DRIVERS_ISP_MODULES_STATS_MGR_H_
#define SRC_CAMERA_DRIVERS_ISP_MODULES_STATS_MGR_H_

#include <lib/mmio/mmio.h>
#include <lib/sync/completion.h>

#include <atomic>

#include <fbl/unique_ptr.h>

#include "sensor.h"
#ifndef _ALL_SOURCE
#define _ALL_SOURCE  // Enables thrd_create_with_name in <threads.h>.
#endif
#include <threads.h>

namespace camera {

// Takes the place of the fsm_mgr.
// Processes an event queue, and maintains ownership of all the modules.
// This class will be broken out into multiple classes based on utility, but
// this will serve as the initial step in porting functionality from the fsm
// architecture.
// Collects statistics from all the modules.
class StatsManager {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(StatsManager);
  StatsManager(fbl::unique_ptr<camera::Sensor> sensor, sync_completion_t frame_processing_signal)
      : sensor_(std::move(sensor)), frame_processing_signal_(frame_processing_signal) {}

  static fbl::unique_ptr<StatsManager> Create(ddk::MmioView isp_mmio, ddk::MmioView isp_mmio_local,
                                              ddk::CameraSensorProtocolClient camera_sensor,
                                              sync_completion_t frame_processing_signal);
  ~StatsManager();

  void SensorStartStreaming() { sensor_->StartStreaming(); }
  void SensorStopStreaming() { sensor_->StopStreaming(); }

 private:
  int FrameProcessingThread();

  fbl::unique_ptr<camera::Sensor> sensor_;
  sync_completion_t frame_processing_signal_;
  thrd_t frame_processing_thread_;
  std::atomic<bool> running_;
};

}  // namespace camera

#endif  // SRC_CAMERA_DRIVERS_ISP_MODULES_STATS_MGR_H_
