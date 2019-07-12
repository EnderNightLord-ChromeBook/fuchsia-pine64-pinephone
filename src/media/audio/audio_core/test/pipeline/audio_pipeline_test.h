// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TEST_PIPELINE_AUDIO_PIPELINE_TEST_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TEST_PIPELINE_AUDIO_PIPELINE_TEST_H_

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/zx/vmo.h>

#include "src/media/audio/lib/test/audio_core_test_base.h"

namespace media::audio::test {

// VAD default format values
constexpr uint32_t kDefaultFrameRate = 48000;
constexpr uint32_t kDefaultSampleFormat = 4;  //  16-bit LPCM
constexpr fuchsia::media::AudioSampleFormat kDefaultAudioFormat =
    fuchsia::media::AudioSampleFormat::SIGNED_16;
constexpr uint32_t kDefaultSampleSize = 2;
constexpr uint32_t kDefaultNumChannels = 2;
constexpr zx_duration_t kDefaultExternalDelayNs = 0;
constexpr uint32_t kDefaultFrameSize = kDefaultSampleSize * kDefaultNumChannels;

// Test-specific values
// For shared buffer to renderer, use 10 pkts of 10 ms each
constexpr uint32_t kPacketMs = 10;
constexpr uint32_t kNumPayloads = 10;
constexpr uint32_t kPacketFrames = kDefaultFrameRate / 1000 * kPacketMs;
constexpr uint32_t kPacketBytes = kDefaultFrameSize * kPacketFrames;
constexpr uint32_t kRendererFrames = kPacketFrames * kNumPayloads;
constexpr uint32_t kRendererBytes = kDefaultFrameSize * kRendererFrames;

// Set VAD ring buffer to 300 ms, and 30 notifs per buffer
constexpr uint32_t kSectionMs = 10;
constexpr uint32_t kNumRingSections = 30;
constexpr uint32_t kSectionFrames = kDefaultFrameRate / 1000 * kSectionMs;
constexpr uint32_t kSectionBytes = kDefaultFrameSize * kSectionFrames;
constexpr uint32_t kRingFrames = kNumRingSections * kSectionFrames;
constexpr uint32_t kRingBytes = kDefaultFrameSize * kRingFrames;

// AudioPipelineTest definition
class AudioPipelineTest : public AudioCoreTestBase {
 public:
  // Set up once when binary loaded; this is used at start/end of each suite.
  static void SetControl(fuchsia::virtualaudio::ControlSyncPtr control_sync);
  static void ResetVirtualDevices();
  static void DisableVirtualDevices();

  // "Regional" per-test-suite set-up. Called before first test in this suite.
  //    static void SetUpTestSuite();
  // Per-test-suite tear-down. Called after last test in this suite.
  static void TearDownTestSuite();

 protected:
  void SetUp() override;
  void TearDown() override;

  void AddVirtualOutput();
  void SetVirtualAudioEvents();
  void ResetVirtualAudioEvents();

  void SetAudioDeviceEvents();
  void ResetAudioDeviceEvents();

  void SetUpRenderer();
  void SetAudioRendererEvents();
  void ResetAudioRendererEvents();

  void SetUpBuffers();

  uint64_t RingBufferSize() const { return kDefaultFrameSize * num_rb_frames_; }
  uint8_t* RingBufferStart() const { return reinterpret_cast<uint8_t*>(ring_buffer_.start()); }
  void SnapshotRingBuffer();
  uint32_t FirstSnapshotFrameSilence();
  bool RemainingSnapshotIsSilence(uint32_t frame_num);

  void MapAndAddRendererBuffer(uint32_t buffer_id);

  void CreateAndSendPackets(uint32_t num_packets, int64_t initial_pts, int16_t data_val);
  void WaitForPacket(uint32_t packet_num);

  void SynchronizedPlay();

  //
  // virtualaudio-related members
  static fuchsia::virtualaudio::ControlSyncPtr control_sync_;
  fuchsia::virtualaudio::OutputPtr output_;
  fuchsia::virtualaudio::InputPtr input_;
  uint64_t output_token_ = 0;

  bool received_set_format_ = false;
  bool received_set_gain_ = false;
  float gain_db_ = fuchsia::media::audio::MUTED_GAIN_DB;
  bool received_ring_buffer_ = false;
  zx::vmo rb_vmo_;
  uint32_t num_rb_frames_ = 0;
  fzl::VmoMapper ring_buffer_;
  bool received_start_ = false;
  zx_time_t start_time_ = 0;
  bool received_stop_ = false;
  zx_time_t stop_time_ = 0;
  uint32_t stop_pos_ = 0;
  bool received_discard_all_ = false;
  uint32_t ring_pos_ = 0;
  uint64_t running_ring_pos_ = 0;
  zx_time_t latest_pos_notify_time_ = 0;

  // Snapshot of ring buffer, for comparison
  std::unique_ptr<uint8_t[]> compare_buff_;

  //
  // AudioDeviceEnum-related members
  fuchsia::media::AudioDeviceEnumeratorPtr audio_dev_enum_;
  bool received_add_device_ = false;
  uint64_t received_add_device_token_ = 0;
  float received_gain_db_ = fuchsia::media::audio::MUTED_GAIN_DB;
  bool received_mute_ = true;
  bool received_remove_device_ = false;
  bool received_gain_changed_ = false;
  bool received_default_device_changed_ = false;
  uint64_t received_default_device_token_ = 0;

  //
  // AudioRenderer-related members
  fuchsia::media::AudioRendererPtr audio_renderer_;

  bool received_min_lead_time_ = false;
  int64_t min_lead_time_ = -1;

  fzl::VmoMapper payload_buffer_;

  bool received_play_ = false;
  int64_t received_play_ref_time = 0;
  int64_t received_play_media_time_ = -1;

  bool received_pause_ = false;
  int64_t received_pause_ref_time_ = 0;
  int64_t received_pause_media_time_ = -1;

  bool received_packet_completion_ = false;
  uint32_t received_packet_num_ = 0;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TEST_PIPELINE_AUDIO_PIPELINE_TEST_H_
