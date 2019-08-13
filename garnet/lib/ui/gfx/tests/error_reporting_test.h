// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_LIB_UI_GFX_TESTS_ERROR_REPORTING_TEST_H_
#define GARNET_LIB_UI_GFX_TESTS_ERROR_REPORTING_TEST_H_

#include <string>
#include <vector>

#include "garnet/lib/ui/scenic/event_reporter.h"
#include "garnet/lib/ui/scenic/util/error_reporter.h"
#include "gtest/gtest.h"
#include "lib/gtest/test_loop_fixture.h"
#include "src/lib/fxl/logging.h"

namespace scenic_impl {
namespace gfx {
namespace test {

// Use of this macro allows us to remain consistent with gtest syntax, aiding
// readability.
#define EXPECT_ERROR_COUNT(n) ExpectErrorCount((n))

class TestErrorReporter : public ErrorReporter {
 public:
  const std::vector<std::string>& errors() const { return reported_errors_; }

 private:
  // |ErrorReporter|
  void ReportError(fxl::LogSeverity severity, std::string error_string) override;

  std::vector<std::string> reported_errors_;
};

class TestEventReporter : public EventReporter {
 public:
  TestEventReporter() : weak_factory_(this){};

  const std::vector<fuchsia::ui::scenic::Event>& events() const { return events_; }

  // |EventReporter|
  EventReporterWeakPtr GetWeakPtr() override { return weak_factory_.GetWeakPtr(); }

 private:
  // |EventReporter|
  void EnqueueEvent(fuchsia::ui::gfx::Event event) override;
  void EnqueueEvent(fuchsia::ui::input::InputEvent event) override;
  void EnqueueEvent(fuchsia::ui::scenic::Command unhandled) override;

  std::vector<fuchsia::ui::scenic::Event> events_;

  fxl::WeakPtrFactory<TestEventReporter> weak_factory_;  // must be last
};

class ErrorReportingTest : public ::gtest::TestLoopFixture {
 protected:
  ErrorReportingTest();
  virtual ~ErrorReportingTest();

  ErrorReporter* error_reporter() const;
  EventReporter* event_reporter() const;
  std::shared_ptr<ErrorReporter> shared_error_reporter() const;
  std::shared_ptr<EventReporter> shared_event_reporter() const;

  // Return the events that were enqueued on the EventReporter returned by |event_reporter()|.
  const std::vector<fuchsia::ui::scenic::Event>& events() const;

  // Verify that the expected number of errors were reported.
  void ExpectErrorCount(size_t errors_expected) {
    EXPECT_EQ(errors_expected, error_reporter_->errors().size());
  }

  // Verify that the error at position |pos| in the list is as expected.  If
  // |pos| is >= the number of errors, then the verification will fail.  If no
  // error is expected, use nullptr as |expected_error_string|.
  void ExpectErrorAt(size_t pos, const char* expected_error_string);

  // Verify that the last reported error is as expected.  If no error is
  // expected, use nullptr as |expected_error_string|.
  void ExpectLastReportedError(const char* expected_error_string);

  // | ::testing::Test |
  void SetUp() override;
  // | ::testing::Test |
  void TearDown() override;

 private:
  std::shared_ptr<TestErrorReporter> error_reporter_;
  std::shared_ptr<TestEventReporter> event_reporter_;

  // Help subclasses remember to call SetUp() and TearDown() on superclass.
  bool setup_called_ = false;
  bool teardown_called_ = false;
};

}  // namespace test
}  // namespace gfx
}  // namespace scenic_impl

#endif  // GARNET_LIB_UI_GFX_TESTS_ERROR_REPORTING_TEST_H_
