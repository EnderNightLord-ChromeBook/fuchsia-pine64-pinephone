// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <fbl/algorithm.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/assert.h>

#include "test.h"
#include "test-resources.h"

namespace board_test {

void TestBoard::DdkRelease() {
    delete this;
}

int TestBoard::Thread() {
    zx_status_t status;

    status = GpioInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: GpioInit failed: %d\n", __func__, status);
    }

    status = I2cInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: I2cInit failed: %d\n", __func__, status);
    }

    status = ClockInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: ClockInit failed: %d\n", __func__, status);
    }

    status = PowerInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: PowerInit failed: %d\n", __func__, status);
    }

    status = TestInit();
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: TestInit failed: %d\n", __func__, status);
    }
    return 0;
}

zx_status_t TestBoard::Start() {
    int rc = thrd_create_with_name(&thread_,
                                   [](void* arg) -> int {
                                       return reinterpret_cast<TestBoard*>(arg)->Thread();
                                   },
                                   this,
                                   "test-board-start-thread");
    if (rc != thrd_success) {
        return ZX_ERR_INTERNAL;
    }
    return ZX_OK;
}

zx_status_t TestBoard::Create(zx_device_t* parent) {
    pbus_protocol_t pbus;
    if (device_get_protocol(parent, ZX_PROTOCOL_PBUS, &pbus) != ZX_OK) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    auto board = std::make_unique<TestBoard>(parent, &pbus);

    zx_status_t status = board->DdkAdd("test-board", DEVICE_ADD_NON_BINDABLE);
    if (status != ZX_OK) {
        zxlogf(ERROR, "TestBoard::Create: DdkAdd failed: %d\n", status);
        return status;
    }

    status = board->Start();
    if (status == ZX_OK) {
      // devmgr is now in charge of the device.
      __UNUSED auto* dummy = board.release();
    }

    // Add a composite device
    const zx_bind_inst_t root_match[] = {
        BI_MATCH(),
    };
    const zx_bind_inst_t pdev_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TEST),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_PBUS_TEST),
        BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_TEST_COMPOSITE),
    };
    const zx_bind_inst_t gpio_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_GPIO),
        BI_MATCH_IF(EQ, BIND_GPIO_PIN, 3),
    };
    const zx_bind_inst_t clock_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_CLOCK),
        BI_MATCH_IF(EQ, BIND_CLOCK_ID, 1),
    };
    const zx_bind_inst_t i2c_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
        BI_ABORT_IF(NE, BIND_I2C_BUS_ID, 1),
        BI_MATCH_IF(EQ, BIND_I2C_ADDRESS, 5),
    };
    const zx_bind_inst_t power_match[] = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_POWER),
        BI_MATCH_IF(EQ, BIND_POWER_DOMAIN, 3),
    };
    device_component_part_t pdev_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(pdev_match), pdev_match },
    };
    device_component_part_t gpio_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(gpio_match), gpio_match },
    };
    device_component_part_t clock_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(clock_match), clock_match },
    };
    device_component_part_t i2c_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(i2c_match), i2c_match },
    };
    device_component_part_t power_component[] = {
        { fbl::count_of(root_match), root_match },
        { fbl::count_of(power_match), power_match },
    };
    device_component_t components[] = {
        { fbl::count_of(pdev_component), pdev_component },
        { fbl::count_of(gpio_component), gpio_component },
        { fbl::count_of(clock_component), clock_component },
        { fbl::count_of(i2c_component), i2c_component },
        { fbl::count_of(power_component), power_component },
    };

    const uint32_t test_metadata_value = 12345;

    const pbus_metadata_t test_metadata[] = {
        {
            .type = DEVICE_METADATA_PRIVATE,
            .data_buffer = &test_metadata_value,
            .data_size = sizeof(test_metadata_value),
        }
    };

    // add platform device to use as a component.
    pbus_dev_t pdev = {};
    pdev.name = "composite-dev";
    pdev.vid = PDEV_VID_TEST;
    pdev.pid = PDEV_PID_PBUS_TEST;
    pdev.did = PDEV_DID_TEST_COMPOSITE;
    pdev.metadata_list = test_metadata;
    pdev.metadata_count = fbl::count_of(test_metadata);
    status = pbus_device_add(&pbus, &pdev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "TestBoard::Create: pbus_device_add failed: %d\n", status);
    }

    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_TEST},
        {BIND_PLATFORM_DEV_PID, 0, PDEV_PID_PBUS_TEST},
        {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_TEST_COMPOSITE},
    };

    // and now composite device, running in devhost of the platform device
    status = device_add_composite(parent, "composite-test", props, fbl::count_of(props),
                                  components, fbl::count_of(components), 0);
    if (status != ZX_OK) {
        zxlogf(ERROR, "TestBoard::Create: pbus_composite_device_add failed: %d\n", status);
    }

    return status;
}

zx_status_t test_bind(void* ctx, zx_device_t* parent) {
    return TestBoard::Create(parent);
}

static zx_driver_ops_t driver_ops = [](){
    zx_driver_ops_t ops;
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = test_bind;
    return ops;
}();

} // namespace board_test

ZIRCON_DRIVER_BEGIN(test_bus, board_test::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PBUS),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TEST),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_PBUS_TEST),
ZIRCON_DRIVER_END(test_bus)
