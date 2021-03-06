// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "l2cap.h"

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/task_domain.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/control_packets.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel_manager.h"

namespace bt {
namespace l2cap {

class Impl final : public L2cap {
 public:
  Impl(fxl::WeakPtr<hci::Transport> hci)
      : L2cap(), dispatcher_(async_get_default_dispatcher()), hci_(std::move(hci)) {
    ZX_ASSERT(hci_);
    ZX_ASSERT(hci_->acl_data_channel());
    const auto& acl_buffer_info = hci_->acl_data_channel()->GetBufferInfo();
    const auto& le_buffer_info = hci_->acl_data_channel()->GetLEBufferInfo();

    // LE Buffer Info is always available from ACLDataChannel.
    ZX_ASSERT(acl_buffer_info.IsAvailable());
    auto send_packets =
        fit::bind_member(hci_->acl_data_channel(), &hci::ACLDataChannel::SendPackets);
    auto drop_queued_acl =
        fit::bind_member(hci_->acl_data_channel(), &hci::ACLDataChannel::DropQueuedPackets);
    auto acl_priority = fit::bind_member(this, &Impl::RequestAclPriority);

    channel_manager_ = std::make_unique<ChannelManager>(
        acl_buffer_info.max_data_length(), le_buffer_info.max_data_length(),
        std::move(send_packets), std::move(drop_queued_acl), std::move(acl_priority));
    hci_->acl_data_channel()->SetDataRxHandler(channel_manager_->MakeInboundDataHandler());

    bt_log(DEBUG, "l2cap", "initialized");
  }

  ~Impl() {
    bt_log(DEBUG, "l2cap", "shutting down");

    ZX_ASSERT(hci_->acl_data_channel());
    hci_->acl_data_channel()->SetDataRxHandler(nullptr);

    channel_manager_ = nullptr;
  }

  void AddACLConnection(hci::ConnectionHandle handle, hci::Connection::Role role,
                        LinkErrorCallback link_error_callback,
                        SecurityUpgradeCallback security_callback) override {
    channel_manager_->RegisterACL(handle, role, std::move(link_error_callback),
                                  std::move(security_callback));
  }

  void AttachInspect(inspect::Node& parent, std::string name) override {
    node_ = parent.CreateChild(name);
  }

  LEFixedChannels AddLEConnection(hci::ConnectionHandle handle, hci::Connection::Role role,
                                  LinkErrorCallback link_error_callback,
                                  LEConnectionParameterUpdateCallback conn_param_callback,
                                  SecurityUpgradeCallback security_callback) override {
    channel_manager_->RegisterLE(handle, role, std::move(conn_param_callback),
                                 std::move(link_error_callback), std::move(security_callback));

    auto att = channel_manager_->OpenFixedChannel(handle, kATTChannelId);
    auto smp = channel_manager_->OpenFixedChannel(handle, kLESMPChannelId);
    ZX_ASSERT(att);
    ZX_ASSERT(smp);
    return LEFixedChannels{.att = std::move(att), .smp = std::move(smp)};
  }

  void RemoveConnection(hci::ConnectionHandle handle) override {
    channel_manager_->Unregister(handle);
  }

  void AssignLinkSecurityProperties(hci::ConnectionHandle handle,
                                    sm::SecurityProperties security) override {
    channel_manager_->AssignLinkSecurityProperties(handle, security);
  }

  void RequestConnectionParameterUpdate(
      hci::ConnectionHandle handle, hci::LEPreferredConnectionParameters params,
      ConnectionParameterUpdateRequestCallback request_cb) override {
    channel_manager_->RequestConnectionParameterUpdate(handle, params, std::move(request_cb));
  }

  void OpenL2capChannel(hci::ConnectionHandle handle, PSM psm, ChannelParameters params,
                        ChannelCallback cb) override {
    channel_manager_->OpenChannel(handle, psm, params, std::move(cb));
  }

  void RegisterService(PSM psm, ChannelParameters params, ChannelCallback callback) override {
    const bool result = channel_manager_->RegisterService(psm, params, std::move(callback));
    ZX_DEBUG_ASSERT(result);
  }

  void UnregisterService(PSM psm) override { channel_manager_->UnregisterService(psm); }

 private:
  void RequestAclPriority(AclPriority priority, hci::ConnectionHandle handle,
                          fit::callback<void(fit::result<>)> cb) {
    bt_log(TRACE, "l2cap", "sending ACL priority command");

    auto packet = bt::hci::CommandPacket::New(bt::hci::kBcmSetAclPriority,
                                              sizeof(hci::BcmSetAclPriorityCommandParams));
    auto params = packet->mutable_payload<hci::BcmSetAclPriorityCommandParams>();
    params->handle = htole16(handle);
    params->priority = bt::hci::BcmAclPriority::kHigh;
    params->direction = bt::hci::BcmAclPriorityDirection::kSink;
    if (priority == AclPriority::kSource) {
      params->direction = bt::hci::BcmAclPriorityDirection::kSource;
    } else if (priority == AclPriority::kNormal) {
      params->priority = bt::hci::BcmAclPriority::kNormal;
    }

    hci_->command_channel()->SendCommand(
        std::move(packet),
        [cb = std::move(cb), priority](auto id, const hci::EventPacket& event) mutable {
          if (hci_is_error(event, WARN, "l2cap", "BCM acl priority failed")) {
            cb(fit::error());
            return;
          }

          bt_log(DEBUG, "l2cap", "BCM acl priority updated (priority: %#.8x)",
                 static_cast<uint32_t>(priority));
          cb(fit::ok());
        });
  }

  async_dispatcher_t* dispatcher_;

  // Inspect hierarchy node representing the data domain.
  inspect::Node node_;

  // Handle to the underlying HCI transport.
  fxl::WeakPtr<hci::Transport> hci_;

  std::unique_ptr<ChannelManager> channel_manager_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Impl);
};

// static
fbl::RefPtr<L2cap> L2cap::Create(fxl::WeakPtr<hci::Transport> hci) {
  ZX_DEBUG_ASSERT(hci);
  return AdoptRef(new Impl(hci));
}

}  // namespace l2cap
}  // namespace bt
