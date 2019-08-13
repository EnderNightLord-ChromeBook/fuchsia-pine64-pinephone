// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PARSE_BEACON_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PARSE_BEACON_H_

#include <fbl/span.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <wlan/common/element.h>

namespace wlan {

void ParseBeaconElements(fbl::Span<const uint8_t> ies, uint8_t rx_channel,
                         fuchsia::wlan::mlme::BSSDescription* bss_desc);

// The following functions are visible for testing only
void FillRates(fbl::Span<const SupportedRate> supp_rates,
               fbl::Span<const SupportedRate> ext_supp_rates, ::std::vector<uint8_t>* basic,
               ::std::vector<uint8_t>* op);
std::optional<CBW> GetVhtCbw(const fuchsia::wlan::mlme::VhtOperation& vht_op);
wlan_channel_t DeriveChannel(uint8_t rx_channel, std::optional<uint8_t> dsss_chan,
                             const fuchsia::wlan::mlme::HtOperation* ht_op,
                             std::optional<CBW> vht_cbw);

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PARSE_BEACON_H_
