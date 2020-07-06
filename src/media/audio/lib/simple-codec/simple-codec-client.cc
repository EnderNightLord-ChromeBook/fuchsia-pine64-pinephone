// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/simple-codec/simple-codec-client.h>

#include <ddk/debug.h>

namespace audio {

zx_status_t SimpleCodecClient::SetProtocol(ddk::CodecProtocolClient proto_client) {
  proto_client_ = proto_client;
  if (!proto_client_.is_valid()) {
    return ZX_ERR_NO_RESOURCES;
  }
  return ZX_OK;
}

void SimpleCodecClient::SetTimeout(int64_t nsecs) { timeout_nsecs_ = nsecs; }

bool SimpleCodecClient::IsDaiFormatSupported(const DaiFormat& format,
                                             std::vector<DaiSupportedFormats>& supported) {
  for (size_t i = 0; i < supported.size(); ++i) {
    if (IsDaiFormatSupported(format, supported[i])) {
      return true;
    }
  }
  return false;
}

bool SimpleCodecClient::IsDaiFormatSupported(const DaiFormat& format,
                                             DaiSupportedFormats& supported) {
  const sample_format_t sample_format = format.sample_format;
  const justify_format_t justify_format = format.justify_format;
  const uint32_t frame_rate = format.frame_rate;
  const uint8_t bits_per_sample = format.bits_per_sample;
  const uint8_t bits_per_channel = format.bits_per_channel;
  size_t i = 0;
  for (i = 0; i < supported.sample_formats.size() && supported.sample_formats[i] != sample_format;
       ++i) {
  }
  if (i == supported.sample_formats.size()) {
    zxlogf(DEBUG, "SimpleCodec did not find wanted sample format");
    return false;
  }

  for (i = 0;
       i < supported.justify_formats.size() && supported.justify_formats[i] != justify_format;
       ++i) {
  }
  if (i == supported.justify_formats.size()) {
    zxlogf(DEBUG, "SimpleCodec did not find wanted justify format");
    return false;
  }

  for (i = 0; i < supported.frame_rates.size() && supported.frame_rates[i] != frame_rate; ++i) {
  }
  if (i == supported.frame_rates.size()) {
    zxlogf(DEBUG, "SimpleCodec did not find wanted sample rate");
    return false;
  }

  for (i = 0;
       i < supported.bits_per_sample.size() && supported.bits_per_sample[i] != bits_per_sample;
       ++i) {
  }
  if (i == supported.bits_per_sample.size()) {
    zxlogf(DEBUG, "SimpleCodec did not find wanted bits per sample");
    return false;
  }

  for (i = 0;
       i < supported.bits_per_channel.size() && supported.bits_per_channel[i] != bits_per_channel;
       ++i) {
  }
  if (i == supported.bits_per_channel.size()) {
    zxlogf(DEBUG, "SimpleCodec did not find wanted bits per channel");
    return false;
  }

  return true;
}

zx_status_t SimpleCodecClient::Reset() {
  AsyncOut out = {};
  proto_client_.Reset(
      [](void* ctx, zx_status_t status) {
        auto* out = reinterpret_cast<AsyncOut*>(ctx);
        out->status = status;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return status;
  }
  if (out.status != ZX_OK) {
    return out.status;
  }
  return ZX_OK;
}

zx_status_t SimpleCodecClient::Stop() {
  AsyncOut out = {};
  proto_client_.Stop(
      [](void* ctx, zx_status_t status) {
        auto* out = reinterpret_cast<AsyncOut*>(ctx);
        out->status = status;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return status;
  }
  if (out.status != ZX_OK) {
    return out.status;
  }
  return ZX_OK;
}

zx_status_t SimpleCodecClient::Start() {
  AsyncOut out = {};
  proto_client_.Start(
      [](void* ctx, zx_status_t status) {
        auto* out = reinterpret_cast<AsyncOut*>(ctx);
        out->status = status;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return status;
  }
  if (out.status != ZX_OK) {
    return out.status;
  }
  return ZX_OK;
}

zx::status<Info> SimpleCodecClient::GetInfo() {
  AsyncOutData<Info> out = {};
  proto_client_.GetInfo(
      [](void* ctx, const info_t* info) {
        auto* out = reinterpret_cast<AsyncOutData<Info>*>(ctx);
        out->data.unique_id.assign(info->unique_id);
        out->data.product_name.assign(info->product_name);
        out->data.manufacturer.assign(info->manufacturer);
        sync_completion_signal(&out->completion);
      },
      &out);
  if (zx_status_t status = sync_completion_wait(&out.completion, timeout_nsecs_) != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(out.data);
}

zx::status<bool> SimpleCodecClient::IsBridgeable() {
  AsyncOutData<bool> out = {};
  proto_client_.IsBridgeable(
      [](void* ctx, bool supports_bridged_mode) {
        auto* out = reinterpret_cast<AsyncOutData<bool>*>(ctx);
        out->data = supports_bridged_mode;
        sync_completion_signal(&out->completion);
      },
      &out);
  if (auto status = sync_completion_wait(&out.completion, timeout_nsecs_) != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(out.data);
}

zx_status_t SimpleCodecClient::SetBridgedMode(bool bridged) {
  AsyncOut out = {};
  proto_client_.SetBridgedMode(
      bridged,
      [](void* ctx) {
        auto* out = reinterpret_cast<AsyncOut*>(ctx);
        sync_completion_signal(&out->completion);
      },
      &out);
  return sync_completion_wait(&out.completion, timeout_nsecs_);
}

zx::status<std::vector<DaiSupportedFormats>> SimpleCodecClient::GetDaiFormats() {
  AsyncOutData<std::vector<DaiSupportedFormats>> out;
  proto_client_.GetDaiFormats(
      [](void* ctx, zx_status_t s, const dai_supported_formats_t* formats_list,
         size_t formats_count) {
        auto* out = reinterpret_cast<AsyncOutData<std::vector<DaiSupportedFormats>>*>(ctx);
        out->status = s;
        if (out->status == ZX_OK) {
          for (size_t i = 0; i < formats_count; ++i) {
            std::vector<uint32_t> number_of_channels;
            std::vector<sample_format_t> sample_formats;
            std::vector<justify_format_t> justify_formats;
            std::vector<uint32_t> frame_rates;
            std::vector<uint8_t> bits_per_channel;
            std::vector<uint8_t> bits_per_sample;
            for (size_t j = 0; j < formats_list[i].number_of_channels_count; ++j) {
              number_of_channels.push_back(formats_list[i].number_of_channels_list[j]);
            }
            for (size_t j = 0; j < formats_list[i].sample_formats_count; ++j) {
              sample_formats.push_back(formats_list[i].sample_formats_list[j]);
            }
            for (size_t j = 0; j < formats_list[i].justify_formats_count; ++j) {
              justify_formats.push_back(formats_list[i].justify_formats_list[j]);
            }
            for (size_t j = 0; j < formats_list[i].frame_rates_count; ++j) {
              frame_rates.push_back(formats_list[i].frame_rates_list[j]);
            }
            for (size_t j = 0; j < formats_list[i].bits_per_channel_count; ++j) {
              bits_per_channel.push_back(formats_list[i].bits_per_channel_list[j]);
            }
            for (size_t j = 0; j < formats_list[i].bits_per_sample_count; ++j) {
              bits_per_sample.push_back(formats_list[i].bits_per_sample_list[j]);
            }
            DaiSupportedFormats formats = {.number_of_channels = number_of_channels,
                                           .sample_formats = sample_formats,
                                           .justify_formats = justify_formats,
                                           .frame_rates = frame_rates,
                                           .bits_per_channel = bits_per_channel,
                                           .bits_per_sample = bits_per_sample};
            out->data.push_back(std::move(formats));
          }
        }
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  if (out.status != ZX_OK) {
    return zx::error(out.status);
  }
  return zx::ok(out.data);
}

zx_status_t SimpleCodecClient::SetDaiFormat(DaiFormat format) {
  AsyncOut out = {};
  dai_format_t f;
  f.number_of_channels = format.number_of_channels;
  f.channels_to_use_count = format.channels_to_use.size();
  auto channels_to_use = std::make_unique<uint32_t[]>(f.channels_to_use_count);
  for (size_t j = 0; j < f.channels_to_use_count; ++j) {
    channels_to_use[j] = format.channels_to_use[j];
  }
  f.channels_to_use_list = channels_to_use.get();
  f.sample_format = format.sample_format;
  f.justify_format = format.justify_format;
  f.frame_rate = format.frame_rate;
  f.bits_per_channel = format.bits_per_channel;
  f.bits_per_sample = format.bits_per_sample;
  proto_client_.SetDaiFormat(
      &f,
      [](void* ctx, zx_status_t s) {
        auto* out = reinterpret_cast<AsyncOut*>(ctx);
        out->status = s;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return status;
  }
  if (out.status != ZX_OK) {
    return out.status;
  }
  return status;
}

zx::status<GainFormat> SimpleCodecClient::GetGainFormat() {
  AsyncOutData<GainFormat> out = {};
  proto_client_.GetGainFormat(
      [](void* ctx, const gain_format_t* format) {
        auto* out = reinterpret_cast<AsyncOutData<GainFormat>*>(ctx);
        out->data.min_gain_db = format->min_gain;
        out->data.max_gain_db = format->max_gain;
        out->data.gain_step_db = format->gain_step;
        out->data.can_mute = format->can_mute;
        out->data.can_agc = format->can_agc;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(out.data);
}

zx::status<GainState> SimpleCodecClient::GetGainState() {
  AsyncOutData<GainState> out = {};
  proto_client_.GetGainState(
      [](void* ctx, const gain_state_t* state) {
        auto* out = reinterpret_cast<AsyncOutData<GainState>*>(ctx);
        out->data.gain_db = state->gain;
        out->data.muted = state->muted;
        out->data.agc_enable = state->agc_enable;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(out.data);
}

void SimpleCodecClient::SetGainState(GainState state) {
  gain_state_t state2 = {
      .gain = state.gain_db, .muted = state.muted, .agc_enable = state.agc_enable};
  proto_client_.SetGainState(
      &state2, [](void* ctx) {}, nullptr);
}

zx::status<PlugState> SimpleCodecClient::GetPlugState() {
  AsyncOutData<PlugState> out = {};
  proto_client_.GetPlugState(
      [](void* ctx, const plug_state_t* state) {
        auto* out = reinterpret_cast<AsyncOutData<PlugState>*>(ctx);
        out->data.hardwired = state->hardwired;
        out->data.plugged = state->plugged;
        sync_completion_signal(&out->completion);
      },
      &out);
  auto status = sync_completion_wait(&out.completion, timeout_nsecs_);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(out.data);
}

}  // namespace audio
