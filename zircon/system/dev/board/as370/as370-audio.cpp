// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/gpio.h>
#include <ddktl/metadata/audio.h>
#include <fbl/algorithm.h>
#include <limits.h>
#include <soc/as370/as370-gpio.h>
#include <soc/as370/as370-i2c.h>

#include "as370.h"

namespace board_as370 {

constexpr zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};
static const zx_bind_inst_t ref_out_i2c_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    BI_ABORT_IF(NE, BIND_I2C_BUS_ID, 0),
    BI_MATCH_IF(EQ, BIND_I2C_ADDRESS, 0x31),
};
static const zx_bind_inst_t ref_out_codec_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_CODEC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_MAXIM),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_MAXIM_MAX98373),
};
static const device_component_part_t ref_out_i2c_component[] = {
    {fbl::count_of(root_match), root_match},
    {fbl::count_of(ref_out_i2c_match), ref_out_i2c_match},
};
static const device_component_part_t ref_out_codec_component[] = {
    {fbl::count_of(root_match), root_match},
    {fbl::count_of(ref_out_codec_match), ref_out_codec_match},
};

static const zx_bind_inst_t ref_out_enable_gpio_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
    BI_MATCH_IF(EQ, BIND_GPIO_PIN, 17),
};
static const device_component_part_t ref_out_enable_gpio_component[] = {
    {countof(root_match), root_match},
    {countof(ref_out_enable_gpio_match), ref_out_enable_gpio_match},
};

static const device_component_t codec_components[] = {
    {countof(ref_out_i2c_component), ref_out_i2c_component},
    {countof(ref_out_enable_gpio_component), ref_out_enable_gpio_component},
};
static const device_component_t controller_components[] = {
    {countof(ref_out_codec_component), ref_out_codec_component},
};

zx_status_t As370::AudioInit() {

    pbus_dev_t controller_out = {};
    controller_out.name = "as370-audio-out";
    controller_out.vid = PDEV_VID_SYNAPTICS;
    controller_out.pid = PDEV_PID_SYNAPTICS_AS370;
    controller_out.did = PDEV_DID_SYNAPTICS_AUDIO_OUT;

    // Output pin assignments.
    // AMP_EN.
    gpio_impl_.SetAltFunction(17, 0); // mode 0 to set as GPIO.
    gpio_impl_.ConfigOut(17, 0);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(5)));
    gpio_impl_.ConfigOut(17, 1);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(3)));

    gpio_impl_.SetAltFunction(6, 1); // mode 1 to set as I2S1_MCLK.
    gpio_impl_.SetAltFunction(0, 1); // mode 1 to set as I2S1_BCLKIO (TDM_BCLK).
    gpio_impl_.SetAltFunction(1, 1); // mode 1 to set as I2S1_LRLKIO (TDM_FSYNC).
    gpio_impl_.SetAltFunction(2, 1); // mode 1 to set as I2S1_DO[0] (TDM_MOSI).

    // Output devices.
    constexpr zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_MAXIM},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_MAXIM_MAX98373}
    };
    auto status = DdkAddComposite("audio-tas5772", props, countof(props), codec_components,
                                  countof(codec_components), UINT32_MAX);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: DdkAddComposite failed %d\n", __FUNCTION__, status);
        return status;
    }

    status = pbus_.CompositeDeviceAdd(&controller_out, controller_components,
                                      countof(controller_components), UINT32_MAX);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: CompositeDeviceAdd failed %d\n", __FUNCTION__, status);
        return status;
    }

    return ZX_OK;
}

} // namespace board_mt8167
