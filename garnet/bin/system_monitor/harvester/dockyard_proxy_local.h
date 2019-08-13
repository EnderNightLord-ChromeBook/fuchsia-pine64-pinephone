// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_SYSTEM_MONITOR_HARVESTER_DOCKYARD_PROXY_LOCAL_H_
#define GARNET_BIN_SYSTEM_MONITOR_HARVESTER_DOCKYARD_PROXY_LOCAL_H_

#include <string>

#include "dockyard_proxy.h"

namespace harvester {

// A local harvester that simply prints to stdout rather than sending messages
// to the Dockyard.
class DockyardProxyLocal : public DockyardProxy {
 public:
  DockyardProxyLocal() {}

  // |DockyardProxy|.
  virtual DockyardProxyStatus Init() override;

  // |DockyardProxy|.
  virtual DockyardProxyStatus SendInspectJson(const std::string& stream_name,
                                              const std::string& json) override;

  // |DockyardProxy|.
  virtual DockyardProxyStatus SendSample(const std::string& stream_name, uint64_t value) override;

  // |DockyardProxy|.
  virtual DockyardProxyStatus SendSampleList(const SampleList list) override;

  // |DockyardProxy|.
  virtual DockyardProxyStatus SendStringSampleList(const StringSampleList list) override;
};

}  // namespace harvester

#endif  // GARNET_BIN_SYSTEM_MONITOR_HARVESTER_DOCKYARD_PROXY_LOCAL_H_
