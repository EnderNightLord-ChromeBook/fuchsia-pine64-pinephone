// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_AUDIO_AS370_TDM_OUTPUT_CODEC_H_
#define ZIRCON_SYSTEM_DEV_AUDIO_AS370_TDM_OUTPUT_CODEC_H_

#include <ddk/debug.h>
#include <ddktl/protocol/codec.h>
#include <lib/sync/completion.h>
#include <lib/zx/time.h>

namespace audio {
namespace as370 {

constexpr sample_format_t kWantedSampleFormat = SAMPLE_FORMAT_PCM_SIGNED;
constexpr justify_format_t kWantedJustifyFormat = JUSTIFY_FORMAT_JUSTIFY_I2S;
constexpr uint32_t kWantedFrameRate = 48000;
constexpr uint8_t kWantedBitsPerSample = 32;
constexpr uint8_t kWantedBitsPerChannel = 32;

struct Codec {
  static constexpr uint32_t kCodecTimeoutSecs = 1;

  struct AsyncOut {
    sync_completion_t completion;
    zx_status_t status;
  };

  zx_status_t GetInfo();
  zx_status_t Reset();
  zx_status_t SetNotBridged();
  zx_status_t CheckExpectedDaiFormat();
  zx_status_t SetDaiFormat(dai_format_t format);
  zx_status_t GetGainFormat(gain_format_t* format);
  zx_status_t GetGainState(gain_state_t* state);
  zx_status_t SetGainState(gain_state_t* state);

  ddk::CodecProtocolClient proto_client_;
};

}  // namespace as370
}  // namespace audio

#endif  // ZIRCON_SYSTEM_DEV_AUDIO_AS370_TDM_OUTPUT_CODEC_H_
