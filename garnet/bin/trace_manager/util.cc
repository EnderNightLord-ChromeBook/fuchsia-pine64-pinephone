// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/trace_manager/util.h"

#include <src/lib/fxl/logging.h>

namespace tracing {

TransferStatus WriteBufferToSocket(const zx::socket& socket, const void* buffer,
                                   size_t len) {
  auto data = reinterpret_cast<const uint8_t*>(buffer);
  size_t offset = 0;
  while (offset < len) {
    zx_status_t status = ZX_OK;
    size_t actual = 0;
    if ((status = socket.write(0u, data + offset, len - offset, &actual)) < 0) {
      if (status == ZX_ERR_SHOULD_WAIT) {
        zx_signals_t pending = 0;
        status = socket.wait_one(ZX_SOCKET_WRITABLE | ZX_SOCKET_PEER_CLOSED,
                                 zx::time::infinite(), &pending);
        if (status < 0) {
          FXL_LOG(ERROR) << "Wait on socket failed: " << status;
          return TransferStatus::kWriteError;
        }

        if (pending & ZX_SOCKET_WRITABLE)
          continue;

        if (pending & ZX_SOCKET_PEER_CLOSED) {
          FXL_LOG(ERROR) << "Peer closed while writing to socket";
          return TransferStatus::kReceiverDead;
        }
      }

      return TransferStatus::kWriteError;
    }
    offset += actual;
  }

  return TransferStatus::kComplete;
}

}  // namespace tracing
