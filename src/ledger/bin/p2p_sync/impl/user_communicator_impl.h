// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_P2P_SYNC_IMPL_USER_COMMUNICATOR_IMPL_H_
#define SRC_LEDGER_BIN_P2P_SYNC_IMPL_USER_COMMUNICATOR_IMPL_H_

#include <lib/callback/auto_cleanable.h>
#include <lib/component/cpp/service_provider_impl.h>

#include <memory>
#include <set>
#include <string>

#include "peridot/lib/convert/convert.h"
#include "src/ledger/bin/p2p_provider/public/p2p_provider.h"
#include "src/ledger/bin/p2p_provider/public/types.h"
#include "src/ledger/bin/p2p_provider/public/user_id_provider.h"
#include "src/ledger/bin/p2p_sync/impl/device_mesh.h"
#include "src/ledger/bin/p2p_sync/public/user_communicator.h"
#include "src/ledger/lib/coroutine/coroutine.h"

namespace p2p_sync {
class LedgerCommunicatorImpl;

// TODO(LE-768): Document the contract of this class in relationship with
// p2p_provider::P2PProvider
class UserCommunicatorImpl : public UserCommunicator,
                             public DeviceMesh,
                             public p2p_provider::P2PProvider::Client {
 public:
  explicit UserCommunicatorImpl(std::unique_ptr<p2p_provider::P2PProvider> provider,
                                coroutine::CoroutineService* coroutine_service);
  ~UserCommunicatorImpl() override;

  // UserCommunicator:
  void Start() override;
  std::unique_ptr<LedgerCommunicator> GetLedgerCommunicator(std::string namespace_id) override;

  // DeviceMesh:
  DeviceSet GetDeviceList() override;
  void Send(const p2p_provider::P2PClientId& device_name,
            convert::ExtendedStringView data) override;

 private:
  // P2PProvider::Client
  void OnDeviceChange(const p2p_provider::P2PClientId& remote_device,
                      p2p_provider::DeviceChangeType change_type) override;
  void OnNewMessage(const p2p_provider::P2PClientId& source, fxl::StringView data) override;

  // Set of active ledgers.
  std::map<std::string, LedgerCommunicatorImpl*, convert::StringViewComparator> ledgers_;
  std::set<p2p_provider::P2PClientId> devices_;

  bool started_ = false;

  std::unique_ptr<p2p_provider::P2PProvider> p2p_provider_;
  coroutine::CoroutineService* const coroutine_service_;
};

}  // namespace p2p_sync

#endif  // SRC_LEDGER_BIN_P2P_SYNC_IMPL_USER_COMMUNICATOR_IMPL_H_
