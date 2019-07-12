// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/p2p_sync/impl/user_communicator_impl.h"

#include "peridot/lib/ledger_client/constants.h"
#include "src/ledger/bin/p2p_sync/impl/encoding.h"
#include "src/ledger/bin/p2p_sync/impl/flatbuffer_message_factory.h"
#include "src/ledger/bin/p2p_sync/impl/ledger_communicator_impl.h"
#include "src/ledger/bin/p2p_sync/impl/message_generated.h"
#include "src/ledger/bin/p2p_sync/impl/message_holder.h"
#include "src/lib/files/file.h"
#include "src/lib/fxl/logging.h"

namespace p2p_sync {

UserCommunicatorImpl::UserCommunicatorImpl(std::unique_ptr<p2p_provider::P2PProvider> provider,
                                           coroutine::CoroutineService* coroutine_service)
    : p2p_provider_(std::move(provider)), coroutine_service_(coroutine_service) {}

UserCommunicatorImpl::~UserCommunicatorImpl() { FXL_DCHECK(ledgers_.empty()); }

void UserCommunicatorImpl::Start() {
  FXL_DCHECK(!started_);
  started_ = true;
  p2p_provider_->Start(this);
}

std::unique_ptr<LedgerCommunicator> UserCommunicatorImpl::GetLedgerCommunicator(
    std::string namespace_id) {
  FXL_DCHECK(started_);
  FXL_DCHECK(ledgers_.find(namespace_id) == ledgers_.end())
      << "UserCommunicatorImpl::GetLedgerCommunicator should be called once "
         "per active namespace: "
      << namespace_id;

  std::unique_ptr<LedgerCommunicatorImpl> ledger =
      std::make_unique<LedgerCommunicatorImpl>(coroutine_service_, namespace_id, this);
  LedgerCommunicatorImpl* ledger_ptr = ledger.get();
  ledger->set_on_delete([this, namespace_id]() mutable { ledgers_.erase(namespace_id); });
  ledgers_[std::move(namespace_id)] = ledger_ptr;
  return ledger;
}

void UserCommunicatorImpl::OnNewMessage(const p2p_provider::P2PClientId& source,
                                        fxl::StringView data) {
  std::optional<MessageHolder<Message>> message = CreateMessageHolder<Message>(data, &ParseMessage);
  if (!message) {
    // Wrong serialization, abort.
    FXL_LOG(ERROR) << "The message received is malformed.";
    return;
  };

  switch ((*message)->message_type()) {
    case MessageUnion_NONE:
      FXL_LOG(ERROR) << "The message received is unexpected at this point.";
      return;
      break;

    case MessageUnion_Request: {
      MessageHolder<Request> request = std::move(*message).TakeAndMap<Request>(
          [](const Message* message) { return static_cast<const Request*>(message->message()); });
      const NamespacePageId* namespace_page_id = request->namespace_page();

      std::string namespace_id(namespace_page_id->namespace_id()->begin(),
                               namespace_page_id->namespace_id()->end());
      std::string page_id(namespace_page_id->page_id()->begin(),
                          namespace_page_id->page_id()->end());

      const auto& it = ledgers_.find(namespace_id);
      if (it == ledgers_.end()) {
        flatbuffers::FlatBufferBuilder buffer;
        CreateUnknownResponseMessage(&buffer, namespace_id, page_id,
                                     ResponseStatus_UNKNOWN_NAMESPACE);
        p2p_provider_->SendMessage(source, convert::ExtendedStringView(buffer));
        return;
      }
      it->second->OnNewRequest(source, page_id, std::move(request));
      break;
    }

    case MessageUnion_Response: {
      MessageHolder<Response> response = std::move(*message).TakeAndMap<Response>(
          [](const Message* message) { return static_cast<const Response*>(message->message()); });
      const NamespacePageId* namespace_page_id = response->namespace_page();
      std::string namespace_id(namespace_page_id->namespace_id()->begin(),
                               namespace_page_id->namespace_id()->end());
      std::string page_id(namespace_page_id->page_id()->begin(),
                          namespace_page_id->page_id()->end());

      const auto& it = ledgers_.find(namespace_id);
      if (it == ledgers_.end()) {
        // We are receiving a response for a ledger that no longer exists.
        // This can happen in normal operation, and we cannot do anything with
        // this message: we can't send it to a ledger, and we don't send
        // responses to responses. So we just drop it here.
        return;
      }
      it->second->OnNewResponse(source, page_id, std::move(response));
      break;
    }
  }
}

void UserCommunicatorImpl::OnDeviceChange(const p2p_provider::P2PClientId& remote_device,
                                          p2p_provider::DeviceChangeType change_type) {
  switch (change_type) {
    case p2p_provider::DeviceChangeType::NEW: {
      devices_.insert(remote_device);
      break;
    }
    case p2p_provider::DeviceChangeType::DELETED: {
      auto it = devices_.find(remote_device);
      FXL_DCHECK(it != devices_.end());
      devices_.erase(it);
      break;
    }
  }
  for (const auto& it : ledgers_) {
    it.second->OnDeviceChange(remote_device, change_type);
  }
}

DeviceMesh::DeviceSet UserCommunicatorImpl::GetDeviceList() { return devices_; }

void UserCommunicatorImpl::Send(const p2p_provider::P2PClientId& device_name,
                                convert::ExtendedStringView data) {
  p2p_provider_->SendMessage(device_name, data);
}

}  // namespace p2p_sync
