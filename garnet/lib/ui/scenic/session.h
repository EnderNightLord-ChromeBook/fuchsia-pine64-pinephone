// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_SCENIC_SESSION_H_
#define GARNET_LIB_UI_SCENIC_SESSION_H_

#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <lib/fit/function.h>

#include <array>
#include <memory>
#include <string>

#include "garnet/lib/ui/scenic/event_reporter.h"
#include "garnet/lib/ui/scenic/forward_declarations.h"
#include "garnet/lib/ui/scenic/scenic.h"
#include "garnet/lib/ui/scenic/system.h"
#include "garnet/lib/ui/scenic/util/error_reporter.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace scenic_impl {

class CommandDispatcher;
class Scenic;

using SessionId = uint64_t;

class Session final : public fuchsia::ui::scenic::Session {
 public:
  Session(SessionId id, ::fidl::InterfaceHandle<fuchsia::ui::scenic::SessionListener> listener);
  ~Session() override;

  void SetCommandDispatchers(
      std::array<CommandDispatcherUniquePtr, System::TypeId::kMaxSystems> dispatchers);

  // |fuchsia::ui::scenic::Session|
  void Enqueue(::std::vector<fuchsia::ui::scenic::Command> cmds) override;

  // |fuchsia::ui::scenic::Session|
  void Present(uint64_t presentation_time, ::std::vector<zx::event> acquire_fences,
               ::std::vector<zx::event> release_fences, PresentCallback callback) override;

  // |fuchsia::ui::scenic::Session|
  void SetDebugName(std::string debug_name) override;

  SessionId id() const { return id_; }

  std::shared_ptr<ErrorReporter> error_reporter() const { return reporter_; }
  std::shared_ptr<EventReporter> event_reporter() const { return reporter_; }

  // For tests.  See FlushEvents() below.
  void set_event_callback(fit::function<void(fuchsia::ui::scenic::Event)> callback) {
    reporter_->set_event_callback(std::move(callback));
  }

  // For tests.  Called by ReportError().
  void set_error_callback(fit::function<void(std::string)> callback) {
    reporter_->set_error_callback(std::move(callback));
  }

 private:
  // Helper class which manages the reporting of events and errors to Scenic clients.
  // NOTE: this object is not only reffed by the owning Session; it is also reffed by
  // shared_ptr<EventReporter/ErrorReporter> that are obtained via CommandDispatcherContext.
  // Therefore, the owning Session cannot be strongly reffed by this, or else a reference-cycle
  // would result.
  class EventAndErrorReporter : public EventReporter, public ErrorReporter {
   public:
    explicit EventAndErrorReporter(Session* session);
    virtual ~EventAndErrorReporter() = default;

    // |EventReporter|
    // Enqueues the gfx/cmd event and schedules call to FlushEvents().
    void EnqueueEvent(fuchsia::ui::gfx::Event event) override;
    void EnqueueEvent(fuchsia::ui::scenic::Command event) override;

    // |EventReporter|
    // Enqueues the input event and immediately calls FlushEvents().
    void EnqueueEvent(fuchsia::ui::input::InputEvent event) override;

    // |EventReporter|
    EventReporterWeakPtr GetWeakPtr() override { return weak_factory_.GetWeakPtr(); }

    // |ErrorReporter|
    // Customize behavior of ErrorReporter::ReportError().
    void ReportError(fxl::LogSeverity severity, std::string error_string) override;

    // Called when the owning session is destroyed.
    void Reset();

    void set_event_callback(fit::function<void(fuchsia::ui::scenic::Event)> callback) {
      event_callback_ = std::move(callback);
    }

    // For tests.  Called by ReportError().
    void set_error_callback(fit::function<void(std::string)> callback) {
      error_callback_ = std::move(callback);
    }

    void FlushEvents();

    // Post an asynchronous task to call FlushEvents.
    void PostFlushTask();

   private:
    Session* session_ = nullptr;

    // Callbacks for testing.
    fit::function<void(fuchsia::ui::scenic::Event)> event_callback_;
    fit::function<void(std::string)> error_callback_;

    // Holds events from EnqueueEvent() until they are flushed by FlushEvents().
    std::vector<fuchsia::ui::scenic::Event> buffered_events_;

    fxl::WeakPtrFactory<EventAndErrorReporter> weak_factory_;
  };

  // Flush any/all events that were enqueued via EnqueueEvent(), sending them
  // to |listener_|.  If |listener_| is null but |event_callback_| isn't, then
  // invoke the callback for each event.
  void FlushEvents() { reporter_->FlushEvents(); }

  // True until we are in the process of being destroyed.
  bool valid_ = true;

  const SessionId id_;
  ::fidl::InterfacePtr<fuchsia::ui::scenic::SessionListener> listener_;

  std::array<CommandDispatcherUniquePtr, System::TypeId::kMaxSystems> dispatchers_;

  // A flow event trace id for following |Session::Present| calls from client
  // to scenic.  This will be incremented each |Session::Present| call.  By
  // convention, the scenic implementation side will also contain its own
  // trace id that begins at 0, and is incremented each |Session::Present|
  // call.
  uint64_t next_present_trace_id_ = 0;

  std::shared_ptr<EventAndErrorReporter> reporter_;

  fxl::WeakPtrFactory<Session> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Session);
};

}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_SCENIC_SESSION_H_
