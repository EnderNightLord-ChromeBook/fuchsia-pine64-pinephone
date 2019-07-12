// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_TESTS_SESSION_HANDLER_TEST_H_
#define GARNET_LIB_UI_GFX_TESTS_SESSION_HANDLER_TEST_H_

#include <lib/fit/function.h>

#include "garnet/lib/ui/gfx/displays/display_manager.h"
#include "garnet/lib/ui/gfx/engine/engine.h"
#include "garnet/lib/ui/gfx/engine/session.h"
#include "garnet/lib/ui/gfx/tests/error_reporting_test.h"
#include "garnet/lib/ui/gfx/tests/mocks.h"
#include "garnet/lib/ui/scenic/event_reporter.h"
#include "garnet/lib/ui/scenic/tests/scenic_test.h"
#include "lib/gtest/test_loop_fixture.h"

namespace scenic_impl {
namespace gfx {
namespace test {

// For testing SessionHandler without having to manually provide all the state
// necessary for SessionHandler to run
class SessionHandlerTest : public ErrorReportingTest, public EventReporter, public SessionUpdater {
 public:
  SessionHandlerTest();

 protected:
  // | ::testing::Test |
  void SetUp() override;
  void TearDown() override;

  // |EventReporter|
  void EnqueueEvent(fuchsia::ui::gfx::Event event) override;
  void EnqueueEvent(fuchsia::ui::input::InputEvent event) override;
  void EnqueueEvent(fuchsia::ui::scenic::Command unhandled) override;

  void InitializeScenic();
  void InitializeDisplayManager();
  void InitializeEngine();
  void InitializeSessionHandler();
  void InitializeScenicSession(SessionId session_id);

  SessionHandler* session_handler() {
    FXL_DCHECK(command_dispatcher_);
    return static_cast<SessionHandler*>(command_dispatcher_.get());
  }

  // |SessionUpdater|
  UpdateResults UpdateSessions(std::unordered_set<SessionId> sessions_to_update,
                               zx_time_t presentation_time, uint64_t trace_id) override;
  // |SessionUpdater|
  void PrepareFrame(zx_time_t presentation_time, uint64_t trace_id) override;

  std::unique_ptr<sys::ComponentContext> app_context_;
  std::unique_ptr<Scenic> scenic_;
  std::unique_ptr<escher::impl::CommandBufferSequencer> command_buffer_sequencer_;
  std::unique_ptr<Engine> engine_;
  std::shared_ptr<FrameScheduler> frame_scheduler_;
  std::unique_ptr<DisplayManager> display_manager_;
  std::unique_ptr<scenic_impl::Session> scenic_session_;
  CommandDispatcherUniquePtr command_dispatcher_;
  std::unique_ptr<SessionManagerForTest> session_manager_;

  std::vector<fuchsia::ui::scenic::Event> events_;

  fxl::WeakPtrFactory<SessionHandlerTest> weak_factory_;  // must be last
};

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_TESTS_SESSION_HANDLER_TEST_H_
