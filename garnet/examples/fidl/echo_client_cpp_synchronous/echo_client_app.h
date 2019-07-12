// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_EXAMPLES_FIDL_ECHO_CLIENT_CPP_SYNCHRONOUS_ECHO_CLIENT_APP_H_
#define GARNET_EXAMPLES_FIDL_ECHO_CLIENT_CPP_SYNCHRONOUS_ECHO_CLIENT_APP_H_

#include <fidl/examples/echo/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/sys/cpp/component_context.h>

namespace echo {

class EchoClientApp {
 public:
  EchoClientApp();

  fidl::examples::echo::EchoSyncPtr& echo_sync() { return echo_sync_; }

  void Start(std::string server_url);

 private:
  EchoClientApp(const EchoClientApp&) = delete;
  EchoClientApp& operator=(const EchoClientApp&) = delete;

  std::unique_ptr<sys::ComponentContext> context_;
  fuchsia::sys::ComponentControllerPtr controller_;
  fidl::examples::echo::EchoSyncPtr echo_sync_;
};

}  // namespace echo

#endif  // GARNET_EXAMPLES_FIDL_ECHO_CLIENT_CPP_SYNCHRONOUS_ECHO_CLIENT_APP_H_
