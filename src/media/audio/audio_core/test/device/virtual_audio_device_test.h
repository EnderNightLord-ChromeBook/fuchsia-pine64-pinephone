// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TEST_DEVICE_VIRTUAL_AUDIO_DEVICE_TEST_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TEST_DEVICE_VIRTUAL_AUDIO_DEVICE_TEST_H_

#include <unordered_set>

#include "src/media/audio/audio_core/test/device/audio_device_test.h"

namespace media::audio::test {

class AtomicDeviceId {
 public:
  AtomicDeviceId() : id_(kInvalidDeviceId) {}

  uint64_t get() const { return id_.load(); }
  uint64_t Next() {
    uint64_t ret;
    do {
      ret = id_.fetch_add(1);
    } while (ret == kInvalidDeviceId);
    return ret;
  }

 private:
  static constexpr uint64_t kInvalidDeviceId = 0;

  std::atomic<uint64_t> id_;
};

//
// VirtualAudioDeviceTest
//
// This set of tests verifies asynchronous usage of AudioDeviceEnumerator.
class VirtualAudioDeviceTest : public AudioDeviceTest {
 public:
  void EnableVirtualDevices();
  void DisableVirtualDevices();

 protected:
  static void PopulateUniqueIdArr(bool is_input, uint8_t* unique_id_arr);

  static void SetUpTestSuite();
  static void TearDownTestSuite();

  void SetUp() override;
  void TearDown() override;
  void WaitForVirtualDeviceDepartures();
  void ExpectDeviceAdded(const std::array<uint8_t, 16>& unique_id_arr) override;
  void ExpectDeviceRemoved(uint64_t remove_token) override;

  void AddTwoDevices(bool is_input, bool is_plugged = true);

  void TestGetDevicesAfterAdd(bool is_input);
  void TestGetDevicesAfterRemove(bool is_input, bool most_recent);
  void TestGetDevicesAfterUnplug(bool is_input, bool most_recent);
  void TestGetDevicesAfterSetDeviceGain(bool is_input);

  void TestGetDefaultDeviceUsingAddGetDevices(bool is_input);
  void TestGetDefaultDeviceAfterAdd(bool is_input);
  void TestGetDefaultDeviceAfterUnpluggedAdd(bool is_input);
  void TestGetDefaultDeviceAfterRemove(bool is_input, bool most_recent);
  void TestGetDefaultDeviceAfterUnplug(bool is_input, bool most_recent);

  void TestGetDeviceGainAfterAdd(bool is_input);
  void TestGetDeviceGainAfterSetDeviceGain(bool is_input);

  void TestOnDeviceAddedAfterAdd(bool is_input, bool is_plugged);
  void TestOnDeviceAddedAfterPlug(bool is_input);

  void TestOnDeviceRemovedAfterRemove(bool is_input, bool is_plugged);
  void TestOnDeviceRemovedAfterUnplug(bool is_input);

  void TestOnDefaultDeviceChangedAfterAdd(bool is_input);
  void TestOnDefaultDeviceChangedAfterPlug(bool is_input, bool most_recent);
  void TestOnDefaultDeviceChangedAfterRemove(bool is_input, bool most_recent);
  void TestOnDefaultDeviceChangedAfterUnplug(bool is_input, bool most_recent);

  void TestOnDeviceGainChanged(bool is_input);

  static fuchsia::virtualaudio::ControlSyncPtr control_sync_;
  static AtomicDeviceId sequential_devices_;

  fuchsia::virtualaudio::InputPtr input_;
  fuchsia::virtualaudio::InputPtr input_2_;
  fuchsia::virtualaudio::OutputPtr output_;
  fuchsia::virtualaudio::OutputPtr output_2_;

  std::unordered_set<uint64_t> virtual_device_tokens_;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TEST_DEVICE_VIRTUAL_AUDIO_DEVICE_TEST_H_
