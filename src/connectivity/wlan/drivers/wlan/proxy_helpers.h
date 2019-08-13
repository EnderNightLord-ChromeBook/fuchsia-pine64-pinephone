// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_WLAN_PROXY_HELPERS_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_WLAN_PROXY_HELPERS_H_

#include <ddk/protocol/ethernet.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlan/protocol/mac.h>
#include <zircon/types.h>

namespace wlan {

// Helper class for use with wlanmac devices
class WlanmacProxy {
 public:
  WlanmacProxy(wlanmac_protocol_t proto) : proto_(proto) {}

  zx_status_t Query(uint32_t options, wlanmac_info_t* info) {
    return proto_.ops->query(proto_.ctx, options, info);
  }

  zx_status_t Start(wlanmac_ifc_t* ifc, zx_handle_t* sme_channel, void* cookie) {
    return proto_.ops->start(proto_.ctx, ifc, sme_channel, cookie);
  }

  void Stop() { proto_.ops->stop(proto_.ctx); }

  zx_status_t QueueTx(uint32_t options, wlan_tx_packet_t* pkt) {
    return proto_.ops->queue_tx(proto_.ctx, options, pkt);
  }

  zx_status_t SetChannel(uint32_t options, wlan_channel_t* chan) {
    return proto_.ops->set_channel(proto_.ctx, options, chan);
  }

  zx_status_t ConfigureBss(uint32_t options, wlan_bss_config_t* config) {
    return proto_.ops->configure_bss(proto_.ctx, options, config);
  }

  zx_status_t EnableBeaconing(uint32_t options, wlan_bcn_config_t* bcn_cfg) {
    return proto_.ops->enable_beaconing(proto_.ctx, options, bcn_cfg);
  }

  zx_status_t ConfigureBeacon(uint32_t options, wlan_tx_packet_t* pkt) {
    return proto_.ops->configure_beacon(proto_.ctx, options, pkt);
  }

  zx_status_t SetKey(uint32_t options, wlan_key_config_t* key_config) {
    return proto_.ops->set_key(proto_.ctx, options, key_config);
  }

  zx_status_t ConfigureAssoc(uint32_t options, wlan_assoc_ctx_t* assoc_ctx) {
    return proto_.ops->configure_assoc(proto_.ctx, options, assoc_ctx);
  }

  zx_status_t ClearAssoc(uint32_t options, const uint8_t* mac) {
    return proto_.ops->clear_assoc(proto_.ctx, options, mac);
  }

  zx_status_t StartHwScan(const wlan_hw_scan_config_t* scan_config) {
    return proto_.ops->start_hw_scan(proto_.ctx, scan_config);
  }

 private:
  wlanmac_protocol_t proto_;
};

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_WLAN_PROXY_HELPERS_H_
