// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <wlan/protocol/if-impl.h>

namespace wlanif {

uint8_t ConvertBSSType(::fuchsia::wlan::mlme::BSSTypes bss_type);
uint8_t ConvertScanType(::fuchsia::wlan::mlme::ScanTypes scan_type);
uint8_t ConvertCBW(::fuchsia::wlan::common::CBW cbw);
void ConvertWlanChan(wlan_channel_t* wlanif_chan, const ::fuchsia::wlan::common::WlanChan& fidl_chan);
void ConvertWlanChan(::fuchsia::wlan::common::WlanChan* fidl_chan, const wlan_channel_t& wlanif_chan);
void CopySSID(const ::std::vector<uint8_t>& in_ssid, wlanif_ssid_t* out_ssid);
void CopyRSNE(const ::std::vector<uint8_t>& in_rsne, uint8_t* out_rsne, size_t* out_rsne_len);
void ConvertBSSDescription(wlanif_bss_description_t* wlanif_bss_desc,
                           const ::fuchsia::wlan::mlme::BSSDescription& fidl_bss_desc);
void ConvertBSSDescription(::fuchsia::wlan::mlme::BSSDescription* fidl_bss_desc,
                           const wlanif_bss_description_t& wlanif_bss_desc);
uint16_t ConvertCapabilityInfo(::fuchsia::wlan::mlme::CapabilityInfo cap_info);
::fuchsia::wlan::mlme::CapabilityInfo ConvertCapabilityInfo(uint16_t capability);
uint8_t ConvertAuthType(::fuchsia::wlan::mlme::AuthenticationTypes auth_type);
uint16_t ConvertDeauthReasonCode(::fuchsia::wlan::mlme::ReasonCode reason);
uint8_t ConvertKeyType(::fuchsia::wlan::mlme::KeyType key_type);
void ConvertSetKeyDescriptor(set_key_descriptor_t* key_desc,
                             const ::fuchsia::wlan::mlme::SetKeyDescriptor& fidl_key_desc);
void ConvertDeleteKeyDescriptor(delete_key_descriptor_t* key_desc,
                                const ::fuchsia::wlan::mlme::DeleteKeyDescriptor& fidl_key_desc);
::fuchsia::wlan::mlme::BSSTypes ConvertBSSType(uint8_t bss_type);
::fuchsia::wlan::common::CBW ConvertCBW(uint8_t cbw);
::fuchsia::wlan::mlme::AuthenticationTypes ConvertAuthType(uint8_t auth_type);
::fuchsia::wlan::mlme::ReasonCode ConvertDeauthReasonCode(uint16_t reason);
::fuchsia::wlan::mlme::ScanResultCodes ConvertScanResultCode(uint8_t code);
::fuchsia::wlan::mlme::JoinResultCodes ConvertJoinResultCode(uint8_t code);
::fuchsia::wlan::mlme::AuthenticateResultCodes ConvertAuthResultCode(uint8_t code);
uint8_t ConvertAuthResultCode(::fuchsia::wlan::mlme::AuthenticateResultCodes result_code);
::fuchsia::wlan::mlme::AssociateResultCodes ConvertAssocResultCode(uint8_t code);
uint8_t ConvertAssocResultCode(::fuchsia::wlan::mlme::AssociateResultCodes code);
::fuchsia::wlan::mlme::StartResultCodes ConvertStartResultCode(uint8_t code);
::fuchsia::wlan::mlme::StopResultCodes ConvertStopResultCode(uint8_t code);
::fuchsia::wlan::mlme::EapolResultCodes ConvertEapolResultCode(uint8_t code);
::fuchsia::wlan::mlme::MacRole ConvertMacRole(uint8_t role);
void ConvertBandCapabilities(::fuchsia::wlan::mlme::BandCapabilities* fidl_band,
                             const wlanif_band_capabilities_t& band);
void ConvertIfaceStats(::fuchsia::wlan::stats::IfaceStats* fidl_stats, const wlanif_stats_t& stats);
uint32_t ConvertMgmtCaptureFlags(::fuchsia::wlan::mlme::MgmtFrameCaptureFlags fidl_flags);
::fuchsia::wlan::mlme::MgmtFrameCaptureFlags ConvertMgmtCaptureFlags(uint32_t ddk_flags);
void ConvertRateSets(::std::vector<uint8_t>* basic, ::std::vector<uint8_t>* op,
                     const wlanif_bss_description_t& wlanif_desc);

}  // namespace wlanif
