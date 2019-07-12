/*
 * Copyright (c) 2019 The Fuchsia Authors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <tuple>

#include <ddk/device.h>
#include <ddk/protocol/gpio.h>
#include <ddk/protocol/sdio.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <mock/ddktl/protocol/gpio.h>
#include <mock/ddktl/protocol/sdio.h>
#include <wifi/wifi-config.h>
#include <zircon/types.h>
#include <zxtest/zxtest.h>

#include "../bus.h"
#include "../sdio.h"

// This is required to use ddk::MockSdio.
bool operator==(const sdio_rw_txn_t& lhs, const sdio_rw_txn_t& rhs) {
    return (lhs.addr == rhs.addr &&
            lhs.data_size == rhs.data_size &&
            lhs.incr == rhs.incr &&
            lhs.write == rhs.write &&
            lhs.buf_offset == rhs.buf_offset);
}

// Stub out the firmware loading from the devhost API.
zx_status_t load_firmware(zx_device_t* dev, const char* path, zx_handle_t* fw,
                          size_t* size) {
  *fw = ZX_HANDLE_INVALID;
  *size = 0;
  return ZX_ERR_NOT_SUPPORTED;
}

namespace {

constexpr sdio_rw_txn MakeSdioTxn(uint32_t addr, uint32_t data_size, bool incr, bool write) {
    return {
        .addr = addr,
        .data_size = data_size,
        .incr = incr,
        .write = write,
        .use_dma = false,
        .dma_vmo = ZX_HANDLE_INVALID,
        .virt_buffer = nullptr,
        .virt_size = 0,
        .buf_offset = 0
    };
}

class MockSdio : public ddk::MockSdio {
public:
    zx_status_t SdioDoVendorControlRwByte(bool write, uint8_t addr, uint8_t write_byte,
                                          uint8_t* out_read_byte) override {
        auto ret = mock_do_vendor_control_rw_byte_.Call(write, addr, write_byte);
        if (out_read_byte != nullptr) {
            *out_read_byte = std::get<1>(ret);
        }

        return std::get<0>(ret);
    }
};

TEST(Sdio, IntrRegister) {
    fake_ddk::Bind ddk;

    wifi_config_t config = {ZX_INTERRUPT_MODE_LEVEL_LOW};
    ddk.SetMetadata(&config, sizeof(config));

    brcmf_sdio_dev dev = {};
    sdio_func func1 = {};
    MockSdio sdio1;
    MockSdio sdio2;
    ddk::MockGpio gpio;
    brcmf_bus bus_if = {};
    brcmf_mp_device settings = {};

    dev.func1 = &func1;
    dev.gpios[WIFI_OOB_IRQ_GPIO_INDEX] = *gpio.GetProto();
    dev.sdio_proto_fn1 = *sdio1.GetProto();
    dev.sdio_proto_fn2 = *sdio2.GetProto();
    dev.bus_if = &bus_if;
    dev.settings = &settings;

    gpio.ExpectConfigIn(ZX_OK, GPIO_NO_PULL)
        .ExpectGetInterrupt(ZX_OK, ZX_INTERRUPT_MODE_LEVEL_LOW, zx::interrupt(ZX_HANDLE_INVALID));
    sdio1.ExpectEnableFnIntr(ZX_OK)
         .ExpectDoVendorControlRwByte(ZX_OK, true, SDIO_CCCR_BRCM_SEPINT,
                                      SDIO_CCCR_BRCM_SEPINT_MASK | SDIO_CCCR_BRCM_SEPINT_OE, 0);
    sdio2.ExpectEnableFnIntr(ZX_OK);


    EXPECT_OK(brcmf_sdiod_intr_register(&dev));

    gpio.VerifyAndClear();
    sdio1.VerifyAndClear();
    sdio2.VerifyAndClear();
}

TEST(Sdio, IntrUnregister) {
    brcmf_sdio_dev dev = {};
    sdio_func func1 = {};

    MockSdio sdio1;
    MockSdio sdio2;
    dev.func1 = &func1;
    dev.sdio_proto_fn1 = *sdio1.GetProto();
    dev.sdio_proto_fn2 = *sdio2.GetProto();
    dev.oob_irq_requested = true;

    sdio1.ExpectDoVendorControlRwByte(ZX_OK, true, 0xf2, 0, 0)
         .ExpectDisableFnIntr(ZX_OK);
    sdio2.ExpectDisableFnIntr(ZX_OK);

    brcmf_sdiod_intr_unregister(&dev);

    sdio1.VerifyAndClear();
    sdio2.VerifyAndClear();

    dev = {};
    func1 = {};

    dev.func1 = &func1;
    dev.sdio_proto_fn1 = *sdio1.GetProto();
    dev.sdio_proto_fn2 = *sdio2.GetProto();
    dev.sd_irq_requested = true;

    sdio1.ExpectDisableFnIntr(ZX_OK);
    sdio2.ExpectDisableFnIntr(ZX_OK);

    brcmf_sdiod_intr_unregister(&dev);

    sdio1.VerifyAndClear();
    sdio2.VerifyAndClear();
}

TEST(Sdio, VendorControl) {
    brcmf_sdio_dev dev = {};

    MockSdio sdio1;
    dev.sdio_proto_fn1 = *sdio1.GetProto();

    sdio1.ExpectDoVendorControlRwByte(ZX_ERR_IO, false, 0xf0, 0, 0xab)
         .ExpectDoVendorControlRwByte(ZX_OK, false, 0xf3, 0, 0x12)
         .ExpectDoVendorControlRwByte(ZX_ERR_BAD_STATE, true, 0xff, 0x55, 0)
         .ExpectDoVendorControlRwByte(ZX_ERR_TIMED_OUT, true, 0xfd, 0x79, 0);

    zx_status_t status;

    EXPECT_EQ(brcmf_sdiod_vendor_control_rb(&dev, 0xf0, &status), 0xab);
    EXPECT_EQ(status, ZX_ERR_IO);
    EXPECT_EQ(brcmf_sdiod_vendor_control_rb(&dev, 0xf3, nullptr), 0x12);

    brcmf_sdiod_vendor_control_wb(&dev, 0xff, 0x55, nullptr);
    brcmf_sdiod_vendor_control_wb(&dev, 0xfd, 0x79, &status);
    EXPECT_EQ(status, ZX_ERR_TIMED_OUT);

    sdio1.VerifyAndClear();
}

TEST(Sdio, Transfer) {
    brcmf_sdio_dev dev = {};

    MockSdio sdio1;
    MockSdio sdio2;
    dev.sdio_proto_fn1 = *sdio1.GetProto();
    dev.sdio_proto_fn2 = *sdio2.GetProto();

    sdio1.ExpectDoRwTxn(ZX_OK, MakeSdioTxn(0x458ef43b, 0xd25d48bb, true, true));
    sdio2.ExpectDoRwTxn(ZX_OK, MakeSdioTxn(0x216977b9, 0x9a1d98ed, true, true))
         .ExpectDoRwTxn(ZX_OK, MakeSdioTxn(0x9da7a590, 0xdc8290a3, true, true))
         .ExpectDoRwTxn(ZX_OK, MakeSdioTxn(0xecf0a024, 0x57d91422, true, true));

    EXPECT_OK(brcmf_sdiod_write(&dev, SDIO_FN_1, 0x458ef43b, nullptr, 0xd25d48bb));
    EXPECT_OK(brcmf_sdiod_write(&dev, SDIO_FN_2, 0x216977b9, nullptr, 0x9a1d98ed));
    EXPECT_OK(brcmf_sdiod_write(&dev, 0, 0x9da7a590, nullptr, 0xdc8290a3));
    EXPECT_OK(brcmf_sdiod_write(&dev, 200, 0xecf0a024, nullptr, 0x57d91422));

    sdio1.VerifyAndClear();
    sdio2.VerifyAndClear();
}

TEST(Sdio, IoAbort) {
    brcmf_sdio_dev dev = {};

    MockSdio sdio1;
    MockSdio sdio2;
    dev.sdio_proto_fn1 = *sdio1.GetProto();
    dev.sdio_proto_fn2 = *sdio2.GetProto();

    sdio1.ExpectIoAbort(ZX_OK);
    sdio2.ExpectIoAbort(ZX_OK).ExpectIoAbort(ZX_OK).ExpectIoAbort(ZX_OK);

    EXPECT_OK(brcmf_sdiod_abort(&dev, 1));
    EXPECT_OK(brcmf_sdiod_abort(&dev, 2));
    EXPECT_OK(brcmf_sdiod_abort(&dev, 0));
    EXPECT_OK(brcmf_sdiod_abort(&dev, 200));

    sdio1.VerifyAndClear();
    sdio2.VerifyAndClear();
}

}  // namespace
