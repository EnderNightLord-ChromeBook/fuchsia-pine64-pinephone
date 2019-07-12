// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_INTEGRATION_TESTING_CPP_TESTING_H_
#define LIB_INTEGRATION_TESTING_CPP_TESTING_H_

#include <fuchsia/testing/runner/cpp/fidl.h>
#include <lib/async/cpp/future.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>

#include <string>

namespace modular {
namespace testing {

// TestStore key used to signal termination of the integration test suite to the
// DevBaseShell, causing it to call Shutdown().
constexpr char kTestShutdown[] = "test_shutdown";

// Integration tests that run under DevBaseShell are cut off after this
// timeout.
constexpr int kTestTimeoutMilliseconds = 30000;

// Connects to the TestRunner service in the caller's Environment.
// This function must be invoked first before calling any of the ones below. A
// test is expected to call either Done() or Teardown() before terminating
// itself in order for the TestRunner service to know that a test process did
// not crash, or that the test has completed and should be torn down.
void Init(sys::ComponentContext* context, const std::string& identity);

// Marks the test a failure with the given |log_msg| message, but does not tear
// it down; the test may continue running. Once the test signals teardown by
// calling Teardown(), the test is finished as a failure.
void Fail(const std::string& log_msg);

// A test must call Done() before it dies, to let the TestRunner service (which
// has a channel connected to this application) know that this test process has
// not crashed, otherwise it must call Teardown() to signal the TestRunner that
// the test has finished altogether. If Done() is not called and the connection
// to the TestService is broken, the test is declared as failed and is torn
// down. If Done() is called, it is not possible to call Teardown().
//
// The calling test component should defer its own exit until test runner has
// acknowledged the receipt of the message using the ack callback. Otherwise
// there is a race between the teardown request and the close of the connection
// to the application controller. If the close of the application controller is
// noticed first by the test runner, it terminates the test as failed.
void Done(fit::function<void()> ack);

// A test may call Teardown() to finish the test run and tear down the service.
// Unless Fail() is called, the TestRunner will consider the test run as having
// passed successfully.
//
// The calling test component should defer its own exit until test runner has
// acknowledged the receipt of the message using the ack callback. Otherwise
// there is a race between the teardown request and the close of the connection
// to the application controller. If the close of the application controller is
// noticed first by the test runner, it terminates the test as failed.
void Teardown(fit::function<void()> ack);

// Returns the TestRunnerStore interface from the caller's
// Environment. Init() must be called before GetStore().
fuchsia::testing::runner::TestRunnerStore* GetStore();

// Creates a new |Future|, adds it to the given vector (to wait for all
// futures in the vector later), and returns a |Completer| callback to be
// passed to a function that requires a callback.
template <typename... Results>
fit::function<void(Results...)> AddBarrierFuture(
    std::vector<FuturePtr<Results...>> futures) {
  auto f = Future<Results...>::Create("some barrier future");
  futures.push_back(f);
  return f->Completer();
}

// Defined for convenience of using GetStore().
void Put(const fidl::StringPtr& key, const fidl::StringPtr& value);

// Defined for convenience of using GetStore(). Listens for the |key|; the value
// is passed to the |callback| function.
void Get(const fidl::StringPtr& key,
         fit::function<void(fidl::StringPtr)> callback);

// Defined for convenience of using GetStore(). The |condition| is used as both
// the key and the value. When listening for the key using Get(), the value is
// used by the receiver to display what key it was waiting on. That way the same
// receiver function can be used to wait for multiple keys. Otherwise, Await()
// can be used to listen for the key.
void Signal(const fidl::StringPtr& condition);

// Defined for convenience of using GetStore(). Waits for |condition| to be
// present as a key in the TestRunnerStore before calling |cont|. The value on
// that key is not exposed to the receiver function |cont|.
void Await(const fidl::StringPtr& condition, fit::function<void()> cont);

// Registers a test point that should pass for a test to be considered
// successful.
void RegisterTestPoint(const std::string& label);

// Signals that a test point has been passed.
void PassTestPoint(const std::string& label);

}  // namespace testing
}  // namespace modular

#endif  // LIB_INTEGRATION_TESTING_CPP_TESTING_H_
