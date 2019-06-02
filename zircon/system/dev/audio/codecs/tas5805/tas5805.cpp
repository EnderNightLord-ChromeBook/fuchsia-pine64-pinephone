// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tas5805.h"

#include <algorithm>
#include <memory>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/composite.h>
#include <ddk/protocol/i2c-lib.h>
#include <ddk/protocol/i2c.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

#if 0
#define LERROR(fmt, ...)    zxlogf(ERROR, "[%s %d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define LERROR(fmt, ...)    printf("[%s %d] " fmt, __func__, __LINE__, ##__VA_ARGS__)
#endif

namespace {
// clang-format off
constexpr uint8_t kRegSelectPage  = 0x00;
constexpr uint8_t kRegReset       = 0x01;
constexpr uint8_t kRegDeviceCtrl1 = 0x02;
constexpr uint8_t kRegDeviceCtrl2 = 0x03;
constexpr uint8_t kRegSapCtrl1    = 0x33;
constexpr uint8_t kRegDigitalVol  = 0x4C;

constexpr uint8_t kRegResetBitsCtrl            = 0x11;
constexpr uint8_t kRegDeviceCtrl1BitsPbtlMode  = 0x04;
constexpr uint8_t kRegDeviceCtrl1Bits1SpwMode  = 0x01;
constexpr uint8_t kRegSapCtrl1Bits16bits       = 0x00;
constexpr uint8_t kRegSapCtrl1Bits32bits       = 0x03;
constexpr uint8_t kRegDeviceCtrl2BitsDeepSleep = 0x00;
constexpr uint8_t kRegDeviceCtrl2BitsPlay      = 0x03;
// clang-format on

// TODO(andresoportus): Add handling for the other formats supported by this codec.
static const uint32_t supported_n_channels[] = {2};
static const sample_format_t supported_sample_formats[] = {SAMPLE_FORMAT_PCM_SIGNED};
static const justify_format_t supported_justify_formats[] = {JUSTIFY_FORMAT_JUSTIFY_I2S};
static const uint32_t supported_rates[] = {48000};
static const uint8_t supported_bits_per_sample[] = {16, 32};
static const dai_supported_formats_t kSupportedDaiFormats = {
    .number_of_channels_list = supported_n_channels,
    .number_of_channels_count = countof(supported_n_channels),
    .sample_formats_list = supported_sample_formats,
    .sample_formats_count = countof(supported_sample_formats),
    .justify_formats_list = supported_justify_formats,
    .justify_formats_count = countof(supported_justify_formats),
    .frame_rates_list = supported_rates,
    .frame_rates_count = countof(supported_rates),
    .bits_per_channel_list = supported_bits_per_sample,
    .bits_per_channel_count = countof(supported_bits_per_sample),
    .bits_per_sample_list = supported_bits_per_sample,
    .bits_per_sample_count = countof(supported_bits_per_sample),
};

enum {
    COMPONENT_I2C,
    COMPONENT_COUNT,
};

} // namespace

namespace audio {

zx_status_t Tas5805::Initialize() {
    auto status = Standby();
    if (status != ZX_OK) {
        return status;
    }
    constexpr uint8_t defaults[][2] = {
        {kRegSelectPage, 0x00},
        {kRegDeviceCtrl1, kRegDeviceCtrl1BitsPbtlMode | kRegDeviceCtrl1Bits1SpwMode},
    };
    for (auto& i : defaults) {
        status = WriteReg(i[0], i[1]);
        if (status != ZX_OK) {
            return status;
        }
    }
    return ExitStandby();
}

zx_status_t Tas5805::Bind() {
    auto status = Initialize();
    if (status != ZX_OK) {
        return status;
    }
    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_TI},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_TI_TAS5805},
    };
    return DdkAdd("tas5805", 0, props, countof(props));
}

zx_status_t Tas5805::Create(zx_device_t* parent) {
    zx_status_t status;
    composite_protocol_t composite;

    status = device_get_protocol(parent, ZX_PROTOCOL_COMPOSITE, &composite);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Could not get composite protocol\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    zx_device_t* components[COMPONENT_COUNT] = {};
    size_t actual;
    composite_get_components(&composite, components, countof(components), &actual);
    // Only PDEV and I2C components are required.
    if (actual < 2) {
        zxlogf(ERROR, "could not get components\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    fbl::AllocChecker ac;
    auto dev = std::unique_ptr<Tas5805>(new (&ac) Tas5805(parent, components[COMPONENT_I2C]));
    status = dev->Bind();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the memory for dev.
    dev.release();
    return ZX_OK;
}

bool Tas5805::ValidGain(float gain) const {
    return (gain <= kMaxGain) && (gain >= kMinGain);
}
void Tas5805::CodecReset(codec_reset_callback callback, void* cookie) {
    auto status = WriteReg(kRegReset, kRegResetBitsCtrl);
    if (status != ZX_OK) {
        zxlogf(ERROR, "Could not reset via registers\n");
        callback(cookie, status);
    }
    status = Initialize();
    if (status != ZX_OK) {
        zxlogf(ERROR, "Could not initialize\n");
        callback(cookie, status);
    }
    callback(cookie, ZX_OK);
}

void Tas5805::CodecGetInfo(codec_get_info_callback callback, void* cookie) {
}

void Tas5805::CodecIsBridgeable(codec_is_bridgeable_callback callback, void* cookie) {
}

void Tas5805::CodecSetBridgedMode(bool enable_bridged_mode,
                                  codec_set_bridged_mode_callback callback, void* cookie) {
}

void Tas5805::CodecGetDaiFormats(codec_get_dai_formats_callback callback, void* cookie) {
    printf("%s\n", __PRETTY_FUNCTION__);
    callback(cookie, ZX_OK, &kSupportedDaiFormats, 1);
}

void Tas5805::CodecSetDaiFormat(const dai_format_t* format, codec_set_dai_format_callback callback,
                                void* cookie) {
    printf("%s\n", __PRETTY_FUNCTION__);
    if (format == nullptr) {
        callback(cookie, ZX_ERR_INVALID_ARGS);
        return;
    }

    // Only allow 2 channels.
    if (format->channels_to_use_count != 2 ||
        format->channels_to_use_list == nullptr ||
        format->channels_to_use_list[0] != 0 ||
        format->channels_to_use_list[1] != 1) {
        if (format->channels_to_use_list != nullptr) {
            LERROR("DAI format channels to use not supported %lu %u %u\n",
                   format->channels_to_use_count,
                   format->channels_to_use_list[0],
                   format->channels_to_use_list[1]);
        }
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    // Only I2S.
    if (format->sample_format != SAMPLE_FORMAT_PCM_SIGNED ||
        format->justify_format != JUSTIFY_FORMAT_JUSTIFY_I2S) {
        LERROR("DAI format format not supported\n");
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    // Check rates allowed.
    size_t i = 0;
    for (i = 0; i < kSupportedDaiFormats.frame_rates_count; ++i) {
        if (format->frame_rate == kSupportedDaiFormats.frame_rates_list[i]) {
            break;
        }
    }
    if (i == kSupportedDaiFormats.frame_rates_count) {
        LERROR("DAI format rates not supported\n");
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }

    // Allow 16 or 32 bits.
    if (format->bits_per_sample != 16 && format->bits_per_sample != 32) {
        LERROR("DAI format number of bits not supported\n");
        callback(cookie, ZX_ERR_NOT_SUPPORTED);
        return;
    }
    uint8_t reg_value =
        format->bits_per_sample == 32 ? kRegSapCtrl1Bits32bits : kRegSapCtrl1Bits16bits;
    auto status = WriteReg(kRegSapCtrl1, reg_value);
    if (status != ZX_OK) {
        callback(cookie, ZX_ERR_INTERNAL);
        return;
    }
    callback(cookie, ZX_OK);
}

void Tas5805::CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie) {
    printf("%s\n", __PRETTY_FUNCTION__);
    gain_format_t format;
    format.type = GAIN_TYPE_DECIBELS;
    format.min_gain = kMinGain;
    format.max_gain = kMaxGain;
    format.gain_step = kGainStep;
    callback(cookie, &format);
}

void Tas5805::CodecGetGainState(codec_get_gain_state_callback callback, void* cookie) {
    printf("%s\n", __PRETTY_FUNCTION__);
    gain_state_t gain_state = {};
    gain_state.gain = current_gain_;
    gain_state.muted = false;
    gain_state.agc_enable = false;
    callback(cookie, &gain_state);
}

void Tas5805::CodecSetGainState(const gain_state_t* gain_state, codec_set_gain_state_callback callback, void* cookie) {
    printf("%s\n", __PRETTY_FUNCTION__);
    float gain = std::clamp(gain_state->gain, kMinGain, kMaxGain);
    uint8_t gain_reg = static_cast<uint8_t>(48 - gain * 2);
    zx_status_t status = WriteReg(kRegDigitalVol, gain_reg);
    if (status != ZX_OK) {
        callback(cookie);
        return;
    }
    current_gain_ = gain;
    callback(cookie);
}

void Tas5805::CodecGetPlugState(codec_get_plug_state_callback callback, void* cookie) {
}

zx_status_t Tas5805::Standby() {
    printf("%s\n", __PRETTY_FUNCTION__);
    return WriteReg(kRegDeviceCtrl2, kRegDeviceCtrl2BitsDeepSleep);
}

zx_status_t Tas5805::ExitStandby() {
    printf("%s\n", __PRETTY_FUNCTION__);
    return WriteReg(kRegDeviceCtrl2, kRegDeviceCtrl2BitsPlay);
}

zx_status_t Tas5805::WriteReg(uint8_t reg, uint8_t value) {
    printf("%s\n", __PRETTY_FUNCTION__);
    uint8_t write_buf[2];
    write_buf[0] = reg;
    write_buf[1] = value;
//#define TRACE_I2C
#ifdef TRACE_I2C
    printf("Writing register 0x%02X to value 0x%02X\n", reg, value);
    auto status = i2c_.WriteSync(write_buf, 2);
    if (status != ZX_OK) {
        printf("Could not I2C write %d\n", status);
        return status;
    }
    uint8_t buffer = 0;
    i2c_.ReadSync(reg, &buffer, 1);
    if (status != ZX_OK) {
        printf("Could not I2C read %d\n", status);
        return status;
    }
    printf("Read register just written 0x%02X, value 0x%02X\n", reg, buffer);
    return ZX_OK;
#else
    return i2c_.WriteSync(write_buf, 2);
#endif
}

zx_status_t tas5805_bind(void* ctx, zx_device_t* parent) {
    return Tas5805::Create(parent);
}

static zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = tas5805_bind;
    return ops;
}();

} // namespace audio

// clang-format off
ZIRCON_DRIVER_BEGIN(ti_tas5805, audio::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TI),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TI_TAS5805),
ZIRCON_DRIVER_END(ti_tas5805)
// clang-format on
