// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_TESTS_STUB_CRASH_SERVER_H_
#define SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_TESTS_STUB_CRASH_SERVER_H_

#include <string>

#include "src/developer/feedback/crashpad_agent/crash_server.h"
#include "third_party/crashpad/util/net/http_body.h"
#include "third_party/crashpad/util/net/http_headers.h"

namespace fuchsia {
namespace crash {

extern const char kStubCrashServerUrl[];
extern const char kStubServerReportId[];

class StubCrashServer : public CrashServer {
 public:
  StubCrashServer(bool request_return_value)
      : CrashServer(kStubCrashServerUrl), request_return_value_(request_return_value) {}

  bool MakeRequest(const crashpad::HTTPHeaders& headers,
                   std::unique_ptr<crashpad::HTTPBodyStream> stream,
                   std::string* server_report_id) override;

 private:
  const bool request_return_value_;
};

}  // namespace crash
}  // namespace fuchsia

#endif  // SRC_DEVELOPER_FEEDBACK_CRASHPAD_AGENT_TESTS_STUB_CRASH_SERVER_H_
