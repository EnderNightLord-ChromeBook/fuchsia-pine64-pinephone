// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vpu.h"
#include "vpu-regs.h"
#include "hhi-regs.h"
#include "vpp-regs.h"
#include <ddk/debug.h>
#include <ddktl/device.h>

namespace astro_display {

namespace {
constexpr uint32_t kVpuMux = 0;
constexpr uint32_t kVpuDiv = 3;

constexpr int16_t RGB709_to_YUV709l_coeff[24] = {
    0x0000, 0x0000, 0x0000,
    0x00bb, 0x0275, 0x003f,
    0x1f99, 0x1ea6, 0x01c2,
    0x01c2, 0x1e67, 0x1fd7,
    0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000,
    0x0040, 0x0200, 0x0200,
    0x0000, 0x0000, 0x0000,
};

constexpr int16_t YUV709l_to_RGB709_coeff12[24] = {
    -256, -2048, -2048,
    4788, 0, 7372,
    4788, -876, -2190,
    4788, 8686, 0,
    0, 0, 0,
    0, 0, 0,
    0, 0, 0,
    0, 0, 0,
};
} // namespace

// AOBUS Register
#define AOBUS_GEN_PWR_SLEEP0            (0x03a << 2)

// CBUS Reset Register
#define RESET0_LEVEL                    (0x0420 << 2)
#define RESET1_LEVEL                    (0x0421 << 2)
#define RESET2_LEVEL                    (0x0422 << 2)
#define RESET4_LEVEL                    (0x0424 << 2)
#define RESET7_LEVEL                    (0x0427 << 2)

#define READ32_VPU_REG(a)               vpu_mmio_->Read32(a)
#define WRITE32_VPU_REG(a, v)           vpu_mmio_->Write32(v, a)

#define READ32_HHI_REG(a)               hhi_mmio_->Read32(a)
#define WRITE32_HHI_REG(a, v)           hhi_mmio_->Write32(v, a)

#define READ32_AOBUS_REG(a)             aobus_mmio_->Read32(a)
#define WRITE32_AOBUS_REG(a, v)         aobus_mmio_->Write32(v, a)

#define READ32_CBUS_REG(a)              cbus_mmio_->Read32(a)
#define WRITE32_CBUS_REG(a, v)          cbus_mmio_->Write32(v, a)

zx_status_t Vpu::Init(zx_device_t* parent) {
    if (initialized_) {
        return ZX_OK;
    }
    zx_status_t status = device_get_protocol(parent, ZX_PROTOCOL_PDEV, &pdev_);
    if (status != ZX_OK) {
        return status;
    }

    // Map VPU registers
    mmio_buffer_t mmio;
    status = pdev_map_mmio_buffer(&pdev_, MMIO_VPU, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                  &mmio);
    if (status != ZX_OK) {
        DISP_ERROR("vpu: Could not map VPU mmio\n");
        return status;
    }
    vpu_mmio_ = ddk::MmioBuffer(mmio);

    // Map HHI registers
    status = pdev_map_mmio_buffer(&pdev_, MMIO_HHI, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                  &mmio);
    if (status != ZX_OK) {
        DISP_ERROR("vpu: Could not map HHI mmio\n");
        return status;
    }
    hhi_mmio_ = ddk::MmioBuffer(mmio);

    // Map AOBUS registers
    status = pdev_map_mmio_buffer(&pdev_, MMIO_AOBUS, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                  &mmio);
    if (status != ZX_OK) {
        DISP_ERROR("vpu: Could not map AOBUS mmio\n");
        return status;
    }
    aobus_mmio_ = ddk::MmioBuffer(mmio);

    // Map CBUS registers
    status = pdev_map_mmio_buffer(&pdev_, MMIO_CBUS, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                  &mmio);
    if (status != ZX_OK) {
        DISP_ERROR("vpu: Could not map CBUS mmio\n");
        return status;
    }
    cbus_mmio_ = ddk::MmioBuffer(mmio);

    // VPU object is ready to be used
    initialized_ = true;
    return ZX_OK;
}

void Vpu::VppInit() {
    ZX_DEBUG_ASSERT(initialized_);

    // init vpu fifo control register
    SET_BIT32(VPU, VPP_OFIFO_SIZE, 0xFFF, 0, 12);
    WRITE32_REG(VPU, VPP_HOLD_LINES, 0x08080808);
    // default probe_sel, for highlight en
    SET_BIT32(VPU, VPP_MATRIX_CTRL, 0x7, 12, 3);

    // setting up os1 for rgb -> yuv limit
    const int16_t* m = RGB709_to_YUV709l_coeff;

    // VPP WRAP OSD1 matrix
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_PRE_OFFSET0_1,
        ((m[0] & 0xfff) << 16) | (m[1] & 0xfff));
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_PRE_OFFSET2,
        m[2] & 0xfff);
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_COEF00_01,
        ((m[3] & 0x1fff) << 16) | (m[4] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_COEF02_10,
        ((m[5]  & 0x1fff) << 16) | (m[6] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_COEF11_12,
        ((m[7] & 0x1fff) << 16) | (m[8] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_COEF20_21,
        ((m[9] & 0x1fff) << 16) | (m[10] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_COEF22,
        m[11] & 0x1fff);
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_OFFSET0_1,
        ((m[18] & 0xfff) << 16) | (m[19] & 0xfff));
    WRITE32_REG(VPU, VPP_WRAP_OSD1_MATRIX_OFFSET2,
        m[20] & 0xfff);
    SET_BIT32(VPU, VPP_WRAP_OSD1_MATRIX_EN_CTRL, 1, 0, 1);

    // VPP WRAP OSD2 matrix
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_PRE_OFFSET0_1,
        ((m[0] & 0xfff) << 16) | (m[1] & 0xfff));
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_PRE_OFFSET2,
        m[2] & 0xfff);
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_COEF00_01,
        ((m[3] & 0x1fff) << 16) | (m[4] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_COEF02_10,
        ((m[5]  & 0x1fff) << 16) | (m[6] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_COEF11_12,
        ((m[7] & 0x1fff) << 16) | (m[8] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_COEF20_21,
        ((m[9] & 0x1fff) << 16) | (m[10] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_COEF22,
        m[11] & 0x1fff);
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_OFFSET0_1,
        ((m[18] & 0xfff) << 16) | (m[19] & 0xfff));
    WRITE32_REG(VPU, VPP_WRAP_OSD2_MATRIX_OFFSET2,
        m[20] & 0xfff);
    SET_BIT32(VPU, VPP_WRAP_OSD2_MATRIX_EN_CTRL, 1, 0, 1);

    // VPP WRAP OSD3 matrix
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_PRE_OFFSET0_1,
        ((m[0] & 0xfff) << 16) | (m[1] & 0xfff));
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_PRE_OFFSET2,
        m[2] & 0xfff);
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_COEF00_01,
        ((m[3] & 0x1fff) << 16) | (m[4] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_COEF02_10,
        ((m[5]  & 0x1fff) << 16) | (m[6] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_COEF11_12,
        ((m[7] & 0x1fff) << 16) | (m[8] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_COEF20_21,
        ((m[9] & 0x1fff) << 16) | (m[10] & 0x1fff));
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_COEF22,
        m[11] & 0x1fff);
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_OFFSET0_1,
        ((m[18] & 0xfff) << 16) | (m[19] & 0xfff));
    WRITE32_REG(VPU, VPP_WRAP_OSD3_MATRIX_OFFSET2,
        m[20] & 0xfff);
    SET_BIT32(VPU, VPP_WRAP_OSD3_MATRIX_EN_CTRL, 1, 0, 1);

    WRITE32_REG(VPU, DOLBY_PATH_CTRL, 0xf);

    // POST2 matrix: YUV limit -> RGB  default is 12bit
    m = YUV709l_to_RGB709_coeff12;

    // VPP WRAP POST2 matrix
    WRITE32_REG(VPU, VPP_POST2_MATRIX_PRE_OFFSET0_1,
        (((m[0] >> 2) & 0xfff) << 16) | ((m[1] >> 2) & 0xfff));
    WRITE32_REG(VPU, VPP_POST2_MATRIX_PRE_OFFSET2,
        (m[2] >> 2) & 0xfff);
    WRITE32_REG(VPU, VPP_POST2_MATRIX_COEF00_01,
        (((m[3] >> 2) & 0x1fff) << 16) | ((m[4] >> 2) & 0x1fff));
    WRITE32_REG(VPU, VPP_POST2_MATRIX_COEF02_10,
        (((m[5] >> 2) & 0x1fff) << 16) | ((m[6] >> 2) & 0x1fff));
    WRITE32_REG(VPU, VPP_POST2_MATRIX_COEF11_12,
        (((m[7] >> 2) & 0x1fff) << 16) | ((m[8] >> 2) & 0x1fff));
    WRITE32_REG(VPU, VPP_POST2_MATRIX_COEF20_21,
        (((m[9] >> 2) & 0x1fff) << 16) | ((m[10] >> 2) & 0x1fff));
    WRITE32_REG(VPU, VPP_POST2_MATRIX_COEF22,
        (m[11] >> 2) & 0x1fff);
    WRITE32_REG(VPU, VPP_POST2_MATRIX_OFFSET0_1,
        (((m[18] >> 2) & 0xfff) << 16) | ((m[19] >> 2) & 0xfff));
    WRITE32_REG(VPU, VPP_POST2_MATRIX_OFFSET2,
        (m[20] >> 2) & 0xfff);
    SET_BIT32(VPU, VPP_POST2_MATRIX_EN_CTRL, 1, 0, 1);


    SET_BIT32(VPU, VPP_MATRIX_CTRL, 1, 0, 1);
    SET_BIT32(VPU, VPP_MATRIX_CTRL, 0, 8, 3);

    // 709L to RGB
    WRITE32_REG(VPU, VPP_MATRIX_PRE_OFFSET0_1, 0x0FC00E00);
    WRITE32_REG(VPU, VPP_MATRIX_PRE_OFFSET2, 0x00000E00);
    // ycbcr limit range, 709 to RGB
    // -16      1.164  0      1.793  0
    // -128     1.164 -0.213 -0.534  0
    // -128     1.164  2.115  0      0
    WRITE32_REG(VPU, VPP_MATRIX_COEF00_01, 0x04A80000);
    WRITE32_REG(VPU, VPP_MATRIX_COEF02_10, 0x072C04A8);
    WRITE32_REG(VPU, VPP_MATRIX_COEF11_12, 0x1F261DDD);
    WRITE32_REG(VPU, VPP_MATRIX_COEF20_21, 0x04A80876);
    WRITE32_REG(VPU, VPP_MATRIX_COEF22, 0x0);
    WRITE32_REG(VPU, VPP_MATRIX_OFFSET0_1, 0x0);
    WRITE32_REG(VPU, VPP_MATRIX_OFFSET2, 0x0);

    SET_BIT32(VPU, VPP_MATRIX_CLIP, 0, 5, 3);
}

void Vpu::ConfigureClock() {
    ZX_DEBUG_ASSERT(initialized_);
    // vpu clock
    WRITE32_REG(HHI, HHI_VPU_CLK_CNTL, ((kVpuMux << 9) | (kVpuDiv << 0)));
    SET_BIT32(HHI, HHI_VPU_CLK_CNTL, 1, 8, 1);

    // vpu clkb
    // bit 0 is set since kVpuClkFrequency > clkB max frequency (350MHz)
    WRITE32_REG(HHI, HHI_VPU_CLKB_CNTL, ((1 << 8) | (1 << 0)));

    // vapb clk
    // turn on ge2d clock since kVpuClkFrequency > 250MHz
    WRITE32_REG(HHI, HHI_VAPBCLK_CNTL, (1 << 30) | (0 << 9) | (1 << 0));

    SET_BIT32(HHI, HHI_VAPBCLK_CNTL, 1, 8, 1);

    SET_BIT32(HHI, HHI_VID_CLK_CNTL2, 0, 0, 8);

    // dmc_arb_config
    WRITE32_REG(VPU, VPU_RDARB_MODE_L1C1, 0x0);
    WRITE32_REG(VPU, VPU_RDARB_MODE_L1C2, 0x10000);
    WRITE32_REG(VPU, VPU_RDARB_MODE_L2C1, 0x900000);
    WRITE32_REG(VPU, VPU_WRARB_MODE_L2C1, 0x20000);
}

void Vpu::PowerOn() {
    ZX_DEBUG_ASSERT(initialized_);
    SET_BIT32(AOBUS, AOBUS_GEN_PWR_SLEEP0, 0, 8, 1); // [8] power on

    // power up memories
    for (int i = 0; i < 32; i+=2) {
        SET_BIT32(HHI, HHI_VPU_MEM_PD_REG0, 0, i, 2);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    for (int i = 0; i < 32; i+=2) {
        SET_BIT32(HHI, HHI_VPU_MEM_PD_REG1, 0, i, 2);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    SET_BIT32(HHI, HHI_VPU_MEM_PD_REG2, 0, 0, 2);
    zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    for (int i = 4; i < 18; i+=2) {
        SET_BIT32(HHI, HHI_VPU_MEM_PD_REG2, 0, i, 2);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    SET_BIT32(HHI, HHI_VPU_MEM_PD_REG2, 0, 30, 2);
    zx_nanosleep(zx_deadline_after(ZX_USEC(5)));

    for (int i = 8; i < 16; i++) {
        SET_BIT32(HHI, HHI_MEM_PD_REG0, 0, i, 1);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    zx_nanosleep(zx_deadline_after(ZX_USEC(20)));

    // Reset VIU + VENC
    // Reset VENCI + VENCP + VADC + VENCL
    // Reset HDMI-APB + HDMI-SYS + HDMI-TX + HDMI-CEC
    CLEAR_MASK32(CBUS, RESET0_LEVEL, ((1<<5) | (1<<10) | (1<<19) | (1<<13)));
    CLEAR_MASK32(CBUS, RESET1_LEVEL, (1<<5));
    CLEAR_MASK32(CBUS, RESET2_LEVEL, (1<<15));
    CLEAR_MASK32(CBUS, RESET4_LEVEL,
                 ((1<<6) | (1<<7) | (1<<13) | (1<<5) | (1<<9) | (1<<4) | (1<<12)));
    CLEAR_MASK32(CBUS, RESET7_LEVEL, (1<<7));

    // Remove VPU_HDMI ISO
    SET_BIT32(AOBUS, AOBUS_GEN_PWR_SLEEP0, 0, 9, 1); // [9] VPU_HDMI

    // release Reset
    SET_MASK32(CBUS, RESET0_LEVEL, ((1 << 5) | (1<<10) | (1<<19) | (1<<13)));
    SET_MASK32(CBUS, RESET1_LEVEL, (1<<5));
    SET_MASK32(CBUS, RESET2_LEVEL, (1<<15));
    SET_MASK32(CBUS, RESET4_LEVEL,
               ((1<<6) | (1<<7) | (1<<13) | (1<<5) | (1<<9) | (1<<4) | (1<<12)));
    SET_MASK32(CBUS, RESET7_LEVEL, (1<<7));

    ConfigureClock();
}

void Vpu::PowerOff() {
    ZX_DEBUG_ASSERT(initialized_);
    // Power down VPU_HDMI
    // Enable Isolation
    SET_BIT32(AOBUS, AOBUS_GEN_PWR_SLEEP0, 1, 9, 1); // ISO
    zx_nanosleep(zx_deadline_after(ZX_USEC(20)));

    // power down memories
    for (int i = 0; i < 32; i+=2) {
        SET_BIT32(HHI, HHI_VPU_MEM_PD_REG0, 0x3, i, 2);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    for (int i = 0; i < 32; i+=2) {
        SET_BIT32(HHI, HHI_VPU_MEM_PD_REG1, 0x3, i, 2);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    SET_BIT32(HHI, HHI_VPU_MEM_PD_REG2, 0x3, 0, 2);
    zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    for (int i = 4; i < 18; i+=2) {
        SET_BIT32(HHI, HHI_VPU_MEM_PD_REG2, 0x3, i, 2);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    SET_BIT32(HHI, HHI_VPU_MEM_PD_REG2, 0x3, 30, 2);
    zx_nanosleep(zx_deadline_after(ZX_USEC(5)));

    for (int i = 8; i < 16; i++) {
        SET_BIT32(HHI, HHI_MEM_PD_REG0, 0x1, i, 1);
        zx_nanosleep(zx_deadline_after(ZX_USEC(5)));
    }
    zx_nanosleep(zx_deadline_after(ZX_USEC(20)));

    // Power down VPU domain
    SET_BIT32(AOBUS, AOBUS_GEN_PWR_SLEEP0, 1, 8, 1); // PDN

    SET_BIT32(HHI, HHI_VAPBCLK_CNTL, 0, 8, 1);
    SET_BIT32(HHI, HHI_VPU_CLK_CNTL, 0, 8, 1);
}

void Vpu::PrintCaptureRegisters() {
    DISP_INFO("** Display Loopback Register Dump **\n\n");
    DISP_INFO("VdInIfMuxCtrlReg = 0x%x\n",
              VdInIfMuxCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInComCtrl0Reg = 0x%x\n",
              VdInComCtrl0Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInComStatus0Reg = 0x%x\n",
              VdInComStatus0Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInAFifoCtrl3Reg = 0x%x\n",
              VdInAFifoCtrl3Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInMatrixCtrlReg = 0x%x\n",
              VdInMatrixCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInWrCtrlReg = 0x%x\n",
              VdInWrCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInWrHStartEndReg = 0x%x\n",
              VdInWrHStartEndReg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdInWrVStartEndReg = 0x%x\n",
              VdInWrVStartEndReg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinCoef00_01Reg = 0x%x\n",
              VdinCoef00_01Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinCoef02_10Reg = 0x%x\n",
              VdinCoef02_10Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinCoef11_12Reg = 0x%x\n",
              VdinCoef11_12Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinCoef20_21Reg = 0x%x\n",
              VdinCoef20_21Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinCoef22Reg = 0x%x\n",
              VdinCoef22Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinOffset0_1Reg = 0x%x\n",
              VdinOffset0_1Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinOffset2Reg = 0x%x\n",
              VdinOffset2Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinPreOffset0_1Reg = 0x%x\n",
              VdinPreOffset0_1Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
    DISP_INFO("VdinPreOffset2Reg = 0x%x\n",
              VdinPreOffset2Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value());
}

zx_status_t Vpu::Capture(uint8_t canvas_idx, uint32_t height, uint32_t stride) {
    ZX_DEBUG_ASSERT(initialized_);

    // setup VPU path
    VdInIfMuxCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_vpu_path_1(4)
        .set_vpu_path_0(4)
        .WriteTo(&(*vpu_mmio_));

    // setup hold lines and vdin selection to internal loopback
    VdInComCtrl0Reg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_hold_lines(0)
        .set_vdin_selection(7)
        .WriteTo(&(*vpu_mmio_));

    VdinLFifoCtrlReg::Get()
        .FromValue(0)
        .set_fifo_buf_size(0xf00)
        .WriteTo(&(*vpu_mmio_));

    // Setup Async Fifo
    VdInAFifoCtrl3Reg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_data_valid_en(1)
        .set_go_field_en(1)
        .set_go_line_en(1)
        .set_vsync_pol_set(1)
        .set_hsync_pol_set(0)
        .set_vsync_sync_reset_en(1)
        .set_fifo_overflow_clr(0)
        .set_soft_reset_en(0)
        .WriteTo(&(*vpu_mmio_));

    // setup vdin input dimensions
    VdinIntfWidthM1Reg::Get()
        .FromValue(stride - 1)
        .WriteTo(&(*vpu_mmio_));

    // Configure memory size
    VdInWrHStartEndReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_start(0)
        .set_end(stride - 1)
        .WriteTo(&(*vpu_mmio_));
    VdInWrVStartEndReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_start(0)
        .set_end(height - 1)
        .WriteTo(&(*vpu_mmio_));

    // Write output canvas index, 128 bit endian, eol with width, enable 4:4:4 RGB888 mode
    VdInWrCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_eol_sel(0)
        .set_word_swap(1)
        .set_memory_format(1)
        .set_canvas_idx(canvas_idx)
        .WriteTo(&(*vpu_mmio_));

    // enable vdin memory power
    SET_BIT32(HHI, HHI_VPU_MEM_PD_REG0, 0, 18, 2);

    // Now that loopback mode is configured, start capture
    // pause write output
    VdInWrCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_write_ctrl(0)
        .WriteTo(&(*vpu_mmio_));

    // disable vdin path
    VdInComCtrl0Reg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_enable_vdin(0)
        .WriteTo(&(*vpu_mmio_));

    // reset mif
    VdInMiscCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_mif_reset(1)
        .WriteTo(&(*vpu_mmio_));
    zx_nanosleep(zx_deadline_after(ZX_USEC(1)));
    VdInMiscCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_mif_reset(0)
        .WriteTo(&(*vpu_mmio_));

    // resume write output
    VdInWrCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_write_ctrl(1)
        .WriteTo(&(*vpu_mmio_));

    // wait until resets finishes
    zx_nanosleep(zx_deadline_after(ZX_MSEC(20)));

    // Clear status bit
    VdInWrCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_done_status_clear_bit(1)
        .WriteTo(&(*vpu_mmio_));

    // Set as urgent
    VdInWrCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_write_req_urgent(1)
        .WriteTo(&(*vpu_mmio_));

    // Enable loopback
    VdInWrCtrlReg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_write_mem_enable(1)
        .WriteTo(&(*vpu_mmio_));

    // enable vdin path
    VdInComCtrl0Reg::Get()
        .ReadFrom(&(*vpu_mmio_))
        .set_enable_vdin(1)
        .WriteTo(&(*vpu_mmio_));

    // Wait for done
    int timeout = 1000;
    while (VdInComStatus0Reg::Get().ReadFrom(&(*vpu_mmio_)).done() == 0 && timeout--) {
        zx_nanosleep(zx_deadline_after(ZX_MSEC(8)));
    }

    if (timeout <= 0) {
        DISP_ERROR("Time out! Loopback did not succeed\n");
        PrintCaptureRegisters();
        return ZX_ERR_TIMED_OUT;

    }

    return ZX_OK;
}

} // namespace astro_display
