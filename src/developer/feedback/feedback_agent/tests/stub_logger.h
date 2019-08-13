// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_TESTS_STUB_LOGGER_H_
#define SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_TESTS_STUB_LOGGER_H_

#include <fuchsia/logger/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/zx/time.h>

#include <string>
#include <vector>

#include "src/lib/fxl/logging.h"

namespace fuchsia {
namespace feedback {

// Returns a LogMessage with the given severity, message and optional tags.
//
// The process and thread ids are constants. The timestamp is a constant plus the optionally
// provided offset.
fuchsia::logger::LogMessage BuildLogMessage(const int32_t severity, const std::string& text,
                                            const zx::duration timestamp_offset = zx::duration(0),
                                            const std::vector<std::string>& tags = {});

// Stub Log service to return canned responses to Log::DumpLogs().
class StubLogger : public fuchsia::logger::Log {
 public:
  // Returns a request handler for binding to this stub service.
  fidl::InterfaceRequestHandler<fuchsia::logger::Log> GetHandler() {
    return bindings_.GetHandler(this);
  }

  // |fuchsia::logger::Log|.
  void Listen(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
              std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override {
    FXL_NOTIMPLEMENTED();
  }
  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;

  // Stub injection methods.
  void set_messages(const std::vector<fuchsia::logger::LogMessage>& messages) {
    messages_ = messages;
  }

  void CloseAllConnections() { bindings_.CloseAll(); }

 protected:
  fidl::BindingSet<fuchsia::logger::Log> bindings_;
  std::vector<fuchsia::logger::LogMessage> messages_;
};

class StubLoggerClosesConnection : public StubLogger {
 public:
  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;
};

class StubLoggerNeverBindsToLogListener : public StubLogger {
 public:
  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;
};

class StubLoggerUnbindsFromLogListenerAfterOneMessage : public StubLogger {
 public:
  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;
};

class StubLoggerNeverCallsLogManyBeforeDone : public StubLogger {
 public:
  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;
};

class StubLoggerBindsToLogListenerButNeverCalls : public StubLogger {
 public:
  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;

 private:
  // Owns the connection with the log listener so that it doesn't get closed when DumpLogs()
  // returns and we can test the timeout on the log listener side.
  fuchsia::logger::LogListenerPtr log_listener_ptr_;
};

class StubLoggerDelaysAfterOneMessage : public StubLogger {
 public:
  StubLoggerDelaysAfterOneMessage(async_dispatcher_t* dispatcher, zx::duration delay)
      : dispatcher_(dispatcher), delay_(delay) {}

  void DumpLogs(fidl::InterfaceHandle<fuchsia::logger::LogListener> log_listener,
                std::unique_ptr<fuchsia::logger::LogFilterOptions> options) override;

 private:
  async_dispatcher_t* dispatcher_;
  zx::duration delay_;
};

}  // namespace feedback
}  // namespace fuchsia

#endif  // SRC_DEVELOPER_FEEDBACK_FEEDBACK_AGENT_TESTS_STUB_LOGGER_H_
