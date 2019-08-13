// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INFLIGHT_LIST_H
#define INFLIGHT_LIST_H

#include <deque>

#include "magma.h"
#include "magma_util/dlog.h"
#include "magma_util/macros.h"
#include "magma_util/status.h"

namespace magma {

// A convenience class for maintaining a list of inflight command buffers,
// by reading completed buffer ids from the notification channel.
// Caution, this approach only works for drivers that report completions
// in this format.
// Note, this class is not threadsafe.
class InflightList {
 public:
  InflightList() {}

  // Deprecated
  InflightList(magma_connection_t connection) : connection_(connection) {}

  void add(uint64_t buffer_id) { buffers_.push_back(buffer_id); }

  void release(uint64_t buffer_id) {
    auto iter = std::find(buffers_.begin(), buffers_.end(), buffer_id);
    DASSERT(iter == buffers_.begin());
    buffers_.erase(iter);
  }

  uint32_t size() { return buffers_.size(); }

  bool is_inflight(uint64_t buffer_id) {
    return std::find(buffers_.begin(), buffers_.end(), buffer_id) != buffers_.end();
  }

  // Deprecated
  bool WaitForCompletion(uint64_t timeout_ms) {
    return magma_wait_notification_channel(connection_, timeout_ms * 1000000ull) == MAGMA_STATUS_OK;
  }

  // Wait for a completion; returns true if a completion was
  // received before |timeout_ms|.
  magma::Status WaitForCompletion(magma_connection_t connection, int64_t timeout_ns) {
    return magma::Status(magma_wait_notification_channel(connection, timeout_ns));
  }

  // Read all outstanding completions and update the inflight list.
  void ServiceCompletions(magma_connection_t connection) {
    uint64_t buffer_ids[8];
    uint64_t bytes_available = 0;
    while (true) {
      magma_status_t status = magma_read_notification_channel(connection, buffer_ids,
                                                              sizeof(buffer_ids), &bytes_available);
      if (status != MAGMA_STATUS_OK) {
        DLOG("magma_read_notification_channel returned %d", status);
        return;
      }
      if (bytes_available == 0)
        return;
      DASSERT(bytes_available % sizeof(uint64_t) == 0);
      for (uint32_t i = 0; i < bytes_available / sizeof(uint64_t); i++) {
        DASSERT(is_inflight(buffer_ids[i]));
        release(buffer_ids[i]);
      }
    }
  }

 private:
  // Deprecated
  magma_connection_t connection_;
  std::deque<uint64_t> buffers_;
};

}  // namespace magma

#endif  // INFLIGHT_LIST_H
