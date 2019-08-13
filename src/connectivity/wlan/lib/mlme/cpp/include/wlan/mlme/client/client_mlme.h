// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CLIENT_MLME_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CLIENT_MLME_H_

#include <optional>

#include <fuchsia/wlan/stats/cpp/fidl.h>
#include <wlan/mlme/client/channel_scheduler.h>
#include <wlan/mlme/client/client_interface.h>
#include <wlan/mlme/client/join_context.h>
#include <wlan/mlme/mlme.h>
#include <wlan/mlme/service.h>
#include <wlan/protocol/mac.h>
#include <zircon/types.h>

namespace wlan {

class DeviceInterface;
class Packet;
class BaseMlmeMsg;
class Scanner;
class Station;

// ClientMlme is a MLME which operates in non-AP role. It is not thread-safe.
class ClientMlme : public Mlme {
 public:
  explicit ClientMlme(DeviceInterface* device);
  ~ClientMlme();

  // Mlme interface methods.
  zx_status_t Init() override;
  zx_status_t HandleMlmeMsg(const BaseMlmeMsg& msg) override;
  zx_status_t HandleFramePacket(fbl::unique_ptr<Packet> pkt) override;
  zx_status_t HandleTimeout(const ObjectId id) override;
  void HwScanComplete(uint8_t code) override final;
  ::fuchsia::wlan::stats::MlmeStats GetMlmeStats() const override final;
  void ResetMlmeStats() override final;

  // Visible for tests only
  bool OnChannel();

 private:
  struct OnChannelHandlerImpl : OnChannelHandler {
    ClientMlme* mlme_;

    explicit OnChannelHandlerImpl(ClientMlme* mlme) : mlme_(mlme) {}

    virtual void HandleOnChannelFrame(fbl::unique_ptr<Packet>) override;
    virtual void PreSwitchOffChannel() override;
    virtual void ReturnedOnChannel() override;
  };

  zx_status_t HandleMlmeJoinReq(const MlmeMsg<::fuchsia::wlan::mlme::JoinRequest>& msg);
  zx_status_t SpawnStation();

  void Unjoin();

  DeviceInterface* const device_;
  OnChannelHandlerImpl on_channel_handler_;
  fbl::unique_ptr<ChannelScheduler> chan_sched_;
  fbl::unique_ptr<Scanner> scanner_;
  // TODO(tkilbourn): track other STAs
  fbl::unique_ptr<ClientInterface> sta_;
  // The BSS the MLME synchronized with.
  // The MLME must synchronize to a BSS before it can start the association
  // flow.
  std::optional<JoinContext> join_ctx_;
};

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CLIENT_MLME_H_
