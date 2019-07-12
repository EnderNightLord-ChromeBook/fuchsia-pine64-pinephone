// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/sys/cpp/component_context.h>

#include "src/connectivity/network/mdns/service/mdns_service_impl.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);

  std::unique_ptr<sys::ComponentContext> component_context = sys::ComponentContext::Create();

  mdns::MdnsServiceImpl impl(component_context.get());

  loop.Run();
  return 0;
}
