// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_DEVELOPER_FEEDBACK_AGENT_SCENIC_PTR_H_
#define SRC_DEVELOPER_FEEDBACK_AGENT_SCENIC_PTR_H_

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fit/bridge.h>
#include <lib/fit/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/zx/time.h>

#include <memory>

#include "src/lib/fxl/functional/cancelable_callback.h"
#include "src/lib/fxl/macros.h"

namespace fuchsia {
namespace feedback {

// Wraps around fuchsia::ui::scenic::ScenicPtr to handle establishing the connection, losing the
// connection, waiting for the callback, enforcing a timeout, etc.
class Scenic {
 public:
  Scenic(async_dispatcher_t* dispatcher, std::shared_ptr<::sys::ServiceDirectory> services);

  fit::promise<fuchsia::ui::scenic::ScreenshotData> TakeScreenshot(zx::duration timeout);

 private:
  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<::sys::ServiceDirectory> services_;

  fuchsia::ui::scenic::ScenicPtr scenic_;
  fit::bridge<fuchsia::ui::scenic::ScreenshotData> done_;
  // We wrap the delayed task we post on the async loop to timeout in a CancelableClosure so we can
  // cancel it if we are done another way.
  fxl::CancelableClosure done_after_timeout_;

  FXL_DISALLOW_COPY_AND_ASSIGN(Scenic);
};

}  // namespace feedback
}  // namespace fuchsia

#endif  // SRC_DEVELOPER_FEEDBACK_AGENT_SCENIC_PTR_H_
