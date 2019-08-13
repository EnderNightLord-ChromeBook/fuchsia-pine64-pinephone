// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/network/mdns/service/mdns_transceiver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>

#include "src/connectivity/network/mdns/service/mdns_addresses.h"
#include "src/connectivity/network/mdns/service/mdns_fidl_util.h"
#include "src/lib/files/unique_fd.h"
#include "src/lib/fxl/logging.h"

namespace mdns {

MdnsTransceiver::MdnsTransceiver() {}

MdnsTransceiver::~MdnsTransceiver() {}

void MdnsTransceiver::Start(fuchsia::netstack::NetstackPtr netstack, const MdnsAddresses& addresses,
                            fit::closure link_change_callback,
                            InboundMessageCallback inbound_message_callback) {
  FXL_DCHECK(netstack);
  FXL_DCHECK(link_change_callback);
  FXL_DCHECK(inbound_message_callback);

  netstack_ = std::move(netstack);
  addresses_ = &addresses;
  link_change_callback_ = std::move(link_change_callback);
  inbound_message_callback_ = [this, callback = std::move(inbound_message_callback)](
                                  std::unique_ptr<DnsMessage> message,
                                  const ReplyAddress& reply_address) {
    if (interface_transceivers_by_address_.find(reply_address.socket_address().address()) ==
        interface_transceivers_by_address_.end()) {
      callback(std::move(message), reply_address);
    }
  };

  netstack_.events().OnInterfacesChanged =
      fit::bind_member(this, &MdnsTransceiver::InterfacesChanged);
}

void MdnsTransceiver::Stop() {
  netstack_ = nullptr;

  for (auto& [address, interface] : interface_transceivers_by_address_) {
    if (interface) {
      interface->Stop();
    }
  }
}

MdnsInterfaceTransceiver* MdnsTransceiver::GetInterfaceTransceiver(const inet::IpAddress& address) {
  auto iter = interface_transceivers_by_address_.find(address);
  return iter == interface_transceivers_by_address_.end() ? nullptr : iter->second.get();
}

void MdnsTransceiver::SendMessage(DnsMessage* message, const ReplyAddress& reply_address) {
  FXL_DCHECK(message);

  if (reply_address.socket_address() == addresses_->v4_multicast()) {
    for (auto& [address, interface] : interface_transceivers_by_address_) {
      FXL_DCHECK(interface);
      interface->SendMessage(message, reply_address.socket_address());
    }

    return;
  }

  auto interface_transceiver = GetInterfaceTransceiver(reply_address.interface_address());
  if (interface_transceiver != nullptr) {
    interface_transceiver->SendMessage(message, reply_address.socket_address());
  }
}

void MdnsTransceiver::LogTraffic() {
  for (auto& [address, interface] : interface_transceivers_by_address_) {
    FXL_DCHECK(interface);
    interface->LogTraffic();
  }
}

void MdnsTransceiver::InterfacesChanged(std::vector<fuchsia::netstack::NetInterface> interfaces) {
  bool link_change = false;

  std::unordered_map<inet::IpAddress, std::unique_ptr<MdnsInterfaceTransceiver>> prev;

  interface_transceivers_by_address_.swap(prev);

  for (const auto& if_info : interfaces) {
    inet::IpAddress address = MdnsFidlUtil::IpAddressFrom(&if_info.addr);

    if ((if_info.flags & fuchsia::netstack::NetInterfaceFlagUp) == 0 || address.is_loopback()) {
      continue;
    }

    inet::IpAddress alternate_address_for_v6;

    if (address.is_v4() && address != inet::IpAddress(0, 0, 0, 0)) {
      // The NIC has been provisioned with a valid V4 address. That address
      // will be the alternate address for any V6 transceivers we create.
      alternate_address_for_v6 = address;

      inet::IpAddress alternate_address_for_v4;
      if (!if_info.ipv6addrs.empty()) {
        // TODO(dalesat): Is the first V6 address the right one?
        alternate_address_for_v4 = MdnsFidlUtil::IpAddressFrom(&if_info.ipv6addrs.front().addr);
      }

      // Ensure that there's an interface transceiver for the V4 address.
      if (EnsureInterfaceTransceiver(address, alternate_address_for_v4, if_info.id, if_info.name,
                                     &prev)) {
        link_change = true;
      }
    }

    // Ensure that there's an interface transceiver for each valid V6 address.
    // TODO(dalesat): What does it mean if there's more than one of these?
    for (auto& subnet : if_info.ipv6addrs) {
      if (EnsureInterfaceTransceiver(MdnsFidlUtil::IpAddressFrom(&subnet.addr),
                                     alternate_address_for_v6, if_info.id, if_info.name, &prev)) {
        link_change = true;
      }
    }
  }

  for (auto& [address, interface] : prev) {
    FXL_DCHECK(interface);
    interface->Stop();
    interface.reset();
    link_change = true;
  }

  if (link_change && link_change_callback_) {
    link_change_callback_();
  }
}

bool MdnsTransceiver::EnsureInterfaceTransceiver(
    const inet::IpAddress& address, const inet::IpAddress& alternate_address, uint32_t id,
    const std::string& name,
    std::unordered_map<inet::IpAddress, std::unique_ptr<MdnsInterfaceTransceiver>>* prev) {
  FXL_DCHECK(prev);

  if (!address.is_valid()) {
    return false;
  }

  bool result_on_fail = false;

  auto iter = prev->find(address);
  if (iter != prev->end()) {
    FXL_DCHECK(iter->second);
    auto& existing = iter->second;
    FXL_DCHECK(existing->address() == address);

    if (existing->name() == name && existing->index() == id) {
      // An interface transceiver already exists for this address. Move it to
      // |interface_transceivers_by_address_|, and we're done.

      if (alternate_address.is_valid()) {
        existing->SetAlternateAddress(alternate_address);
      }

      interface_transceivers_by_address_.emplace(address, std::move(existing));
      prev->erase(iter);
      return false;
    }

    // We have an interface transceiver for this address, but its name or id
    // don't match. Destroy it and create a new one.
    prev->erase(iter);
    result_on_fail = true;
  }

  auto interface_transceiver = MdnsInterfaceTransceiver::Create(address, name, id);

  if (!interface_transceiver->Start(*addresses_, inbound_message_callback_.share())) {
    // Couldn't start the transceiver.
    return result_on_fail;
  }

  if (alternate_address.is_valid()) {
    interface_transceiver->SetAlternateAddress(alternate_address);
  }

  interface_transceivers_by_address_.emplace(address, std::move(interface_transceiver));

  return true;
}

}  // namespace mdns
