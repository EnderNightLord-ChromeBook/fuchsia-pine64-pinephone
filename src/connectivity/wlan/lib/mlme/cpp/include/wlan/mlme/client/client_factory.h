// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CLIENT_FACTORY_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CLIENT_FACTORY_H_

#include <fbl/unique_ptr.h>
#include <wlan/mlme/client/channel_scheduler.h>
#include <wlan/mlme/client/join_context.h>
#include <wlan/mlme/client/station.h>
#include <wlan/mlme/device_interface.h>

namespace wlan {

fbl::unique_ptr<ClientInterface> CreateDefaultClient(DeviceInterface* device, JoinContext* join_ctx,
                                                     ChannelScheduler* chan_scheduler);

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_CLIENT_CLIENT_FACTORY_H_
