// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/zx/channel.h>
#include <test/sysmgr/cpp/fidl.h>

namespace sysmgr {
namespace test {
namespace {

class Service : public ::test::sysmgr::Interface {
 public:
  Service() : context_(sys::ComponentContext::Create()) {
    context_->outgoing()->AddPublicService(bindings_.GetHandler(this));
  }

  ~Service() = default;

  void Ping(PingCallback callback) override { callback("test_sysmgr_service_startup"); }

 private:
  std::unique_ptr<sys::ComponentContext> context_;
  fidl::BindingSet<::test::sysmgr::Interface> bindings_;
};

}  // namespace
}  // namespace test
}  // namespace sysmgr

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);

  sysmgr::test::Service service;
  loop.Run();
  return 0;
}
