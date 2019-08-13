// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/developer/feedback/crashpad_agent/feedback_data_provider_ptr.h"

#include <lib/async/cpp/task.h>
#include <lib/syslog/cpp/logger.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/lib/fxl/logging.h"

namespace fuchsia {
namespace crash {
namespace {

using fuchsia::feedback::Data;

}  // namespace

fit::promise<Data> GetFeedbackData(async_dispatcher_t* dispatcher,
                                   std::shared_ptr<::sys::ServiceDirectory> services,
                                   zx::duration timeout) {
  std::unique_ptr<FeedbackDataProvider> feedback_data_provider =
      std::make_unique<FeedbackDataProvider>(dispatcher, services);

  // We move |feedback_data_provider| in a subsequent chained promise to guarantee its lifetime.
  return feedback_data_provider->GetData(timeout).then(
      [feedback_data_provider = std::move(feedback_data_provider)](fit::result<Data>& result) {
        return std::move(result);
      });
}

FeedbackDataProvider::FeedbackDataProvider(async_dispatcher_t* dispatcher,
                                           std::shared_ptr<::sys::ServiceDirectory> services)
    : dispatcher_(dispatcher), services_(services) {}

fit::promise<Data> FeedbackDataProvider::GetData(zx::duration timeout) {
  FXL_CHECK(!has_called_get_data_) << "GetData() is not intended to be called twice";
  has_called_get_data_ = true;

  data_provider_ = services_->Connect<fuchsia::feedback::DataProvider>();

  // fit::promise does not have the notion of a timeout. So we post a delayed task that will call
  // the completer after the timeout and return an error.
  //
  // We wrap the delayed task in a CancelableClosure so we can cancel it when the fit::bridge is
  // completed another way.
  //
  // It is safe to pass "this" to the fit::function as the callback won't be callable when the
  // CancelableClosure goes out of scope, which is before "this".
  done_after_timeout_.Reset([this] {
    if (!done_.completer) {
      return;
    }

    FX_LOGS(ERROR) << "Feedback data collection timed out";
    done_.completer.complete_error();
  });
  const zx_status_t post_status = async::PostDelayedTask(
      dispatcher_, [cb = done_after_timeout_.callback()] { cb(); }, timeout);
  if (post_status != ZX_OK) {
    FX_PLOGS(ERROR, post_status) << "Failed to post delayed task";
    FX_LOGS(ERROR) << "Skipping Feedback data collection as it is not safe without a timeout";
    return fit::make_result_promise<Data>(fit::error());
  }

  data_provider_.set_error_handler([this](zx_status_t status) {
    if (!done_.completer) {
      return;
    }

    FX_PLOGS(ERROR, status) << "Lost connection to fuchsia.feedback.DataProvider";
    done_.completer.complete_error();
  });

  data_provider_->GetData([this](fuchsia::feedback::DataProvider_GetData_Result out_result) {
    if (!done_.completer) {
      return;
    }

    if (out_result.is_err()) {
      FX_PLOGS(WARNING, out_result.err()) << "Failed to fetch feedback data";
      done_.completer.complete_error();
    } else {
      done_.completer.complete_ok(std::move(out_result.response().data));
    }
  });

  return done_.consumer.promise_or(fit::error()).then([this](fit::result<Data>& result) {
    done_after_timeout_.Cancel();
    return std::move(result);
  });
}

}  // namespace crash
}  // namespace fuchsia
