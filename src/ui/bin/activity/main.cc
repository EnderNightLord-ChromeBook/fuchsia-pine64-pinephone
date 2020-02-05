// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ui/activity/control/cpp/fidl.h>
#include <fuchsia/ui/activity/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/default.h>
#include <lib/sys/cpp/component_context.h>
#include <trace-provider/provider.h>

#include <memory>

#include "lib/async/dispatcher.h"
#include "lib/trace-provider/provider.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/scudo-trace/hooks.h"
#include "src/ui/bin/activity/activity_app.h"
#include "src/ui/bin/activity/state_machine_driver.h"

int main(void) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  async_set_default_dispatcher(loop.dispatcher());

  trace::TraceProviderWithFdio trace_provider{loop.dispatcher(), "my_trace_provider"};
  async::Task dump_task{[](async_dispatcher_t *dispatcher, async::Task *t, zx_status_t status){
    scudo_trace::ExportScudoStats(false);
    t->PostDelayed(dispatcher, zx::sec(1));
  }};
  dump_task.Post(loop.dispatcher());

  std::unique_ptr<sys::ComponentContext> startup_context = sys::ComponentContext::Create();

  auto driver = std::make_unique<activity::StateMachineDriver>(loop.dispatcher());
  activity::ActivityApp app(std::move(driver), loop.dispatcher());

  startup_context->outgoing()->AddPublicService<fuchsia::ui::activity::control::Control>(
      [&app](fidl::InterfaceRequest<fuchsia::ui::activity::control::Control> request) {
        app.AddControlBinding(std::move(request));
      });
  startup_context->outgoing()->AddPublicService<fuchsia::ui::activity::Tracker>(
      [&app](fidl::InterfaceRequest<fuchsia::ui::activity::Tracker> request) {
        app.AddTrackerBinding(std::move(request));
      });
  startup_context->outgoing()->AddPublicService<fuchsia::ui::activity::Provider>(
      [&app](fidl::InterfaceRequest<fuchsia::ui::activity::Provider> request) {
        app.AddProviderBinding(std::move(request));
      });

  FXL_LOG(INFO) << "activity-service: Starting service";
  loop.Run();
  async_set_default_dispatcher(nullptr);
  return 0;
}
