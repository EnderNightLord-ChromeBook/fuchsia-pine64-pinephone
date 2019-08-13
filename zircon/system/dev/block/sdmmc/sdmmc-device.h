// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddktl/protocol/sdmmc.h>

namespace sdmmc {

// SdmmcDevice wraps a ddk::SdmmcProtocolClient to provide helper methods to the SD/MMC and SDIO
// core drivers. It is assumed that the underlying SDMMC protocol driver can handle calls from
// different threads, although care should be taken when calling methods that update the RCA
// (SdSendRelativeAddr and MmcSetRelativeAddr) or change the signal voltage (SdSwitchUhsVoltage).
// These are typically not used outside the probe thread however, so generally no synchronization is
// required.
class SdmmcDevice {
 public:
  SdmmcDevice(const ddk::SdmmcProtocolClient& host, const sdmmc_host_info_t& host_info)
      : host_(host), host_info_(host_info) {}

  SdmmcDevice(const SdmmcDevice& other)
      : host_(other.host_), host_info_(other.host_info_), rca_(other.rca_) {}

  virtual const ddk::SdmmcProtocolClient& host() const { return host_; }
  const sdmmc_host_info_t& host_info() const { return host_info_; }

  bool UseDma() const {
    return (host_info_.caps & (SDMMC_HOST_CAP_ADMA2 | SDMMC_HOST_CAP_SIXTY_FOUR_BIT));
  }

  // Update the current voltage field, e.g. after reading the card status registers.
  void SetCurrentVoltage(sdmmc_voltage_t new_voltage) { signal_voltage_ = new_voltage; }

  virtual zx_status_t SdmmcRequest(sdmmc_req_t* req) { return host_.Request(req); }

  // SD/MMC shared ops
  virtual zx_status_t SdmmcGoIdle();
  virtual zx_status_t SdmmcSendStatus(uint32_t* response);
  virtual zx_status_t SdmmcStopTransmission();

  // SD ops
  virtual zx_status_t SdSendOpCond(uint32_t flags, uint32_t* ocr);
  virtual zx_status_t SdSendIfCond();
  virtual zx_status_t SdSelectCard();
  virtual zx_status_t SdSendScr(uint8_t scr[8]);
  virtual zx_status_t SdSetBusWidth(sdmmc_bus_width_t width);

  // SD/SDIO shared ops
  virtual zx_status_t SdSwitchUhsVoltage(uint32_t ocr);
  virtual zx_status_t SdSendRelativeAddr(uint16_t* card_status);

  // SDIO ops
  virtual zx_status_t SdioSendOpCond(uint32_t ocr, uint32_t* rocr);
  virtual zx_status_t SdioIoRwDirect(bool write, uint32_t fn_idx, uint32_t reg_addr,
                                     uint8_t write_byte, uint8_t* read_byte);
  virtual zx_status_t SdioIoRwExtended(uint32_t caps, bool write, uint32_t fn_idx,
                                       uint32_t reg_addr, bool incr, uint32_t blk_count,
                                       uint32_t blk_size, bool use_dma, uint8_t* buf,
                                       zx_handle_t dma_vmo, uint64_t buf_offset);

  // MMC ops
  virtual zx_status_t MmcSendOpCond(uint32_t ocr, uint32_t* rocr);
  virtual zx_status_t MmcAllSendCid(uint32_t cid[4]);
  virtual zx_status_t MmcSetRelativeAddr(uint16_t rca);
  virtual zx_status_t MmcSendCsd(uint32_t csd[4]);
  virtual zx_status_t MmcSendExtCsd(uint8_t ext_csd[512]);
  virtual zx_status_t MmcSelectCard();
  virtual zx_status_t MmcSwitch(uint8_t index, uint8_t value);

 private:
  zx_status_t SdmmcRequestHelper(sdmmc_req_t* req, uint8_t retries, uint32_t wait_time) const;
  zx_status_t SdSendAppCmd();

  inline uint32_t RcaArg() const { return rca_ << 16; }

  const ddk::SdmmcProtocolClient host_;
  const sdmmc_host_info_t host_info_;
  sdmmc_voltage_t signal_voltage_ = SDMMC_VOLTAGE_V330;
  uint16_t rca_ = 0;  // APP_CMD requires the initial RCA to be zero.
};

}  // namespace sdmmc
