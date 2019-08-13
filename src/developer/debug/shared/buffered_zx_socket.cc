// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/shared/buffered_zx_socket.h"

#include <zircon/status.h>

#include "src/developer/debug/shared/logging/logging.h"
#include "src/developer/debug/shared/message_loop_target.h"
#include "src/developer/debug/shared/zx_status.h"
#include "src/lib/fxl/logging.h"

namespace debug_ipc {

BufferedZxSocket::BufferedZxSocket() = default;
BufferedZxSocket::~BufferedZxSocket() = default;

zx_status_t BufferedZxSocket::Init(zx::socket socket) {
  if (!socket.is_valid())
    return ZX_ERR_INVALID_ARGS;

  FXL_DCHECK(!socket_.is_valid());  // Can't be initialized more than once.
  socket_ = std::move(socket);
  stream_.set_writer(this);

  return ZX_OK;
}

zx_status_t BufferedZxSocket::Start() {
  if (!valid())
    return ZX_ERR_BAD_STATE;

  // Register for socket updates from the message loop.
  // We assume the socket is writable and look for that event when we get evidence it's not.
  return MessageLoopTarget::Current()->WatchSocket(MessageLoop::WatchMode::kRead, socket_.get(),
                                                   this, &watch_handle_);
}

zx_status_t BufferedZxSocket::Stop() {
  if (!valid() || watch_handle_.watching())
    return ZX_ERR_BAD_STATE;
  watch_handle_ = MessageLoop::WatchHandle();
  return ZX_OK;
}

void BufferedZxSocket::Reset() {
  socket_.reset();

  callback_ = DataAvailableCallback();
  error_callback_ = ErrorCallback();
}

void BufferedZxSocket::OnSocketReadable(zx_handle_t) {
  // Messages from the client to the agent are typically small so we don't need
  // a very large buffer.
  constexpr size_t kBufSize = 1024;

  // Add all available data to the socket buffer.
  size_t msg_bytes = 0;
  while (true) {
    std::vector<char> buffer;
    buffer.resize(kBufSize);

    size_t num_read = 0;
    zx_status_t status = socket_.read(0, &buffer[0], kBufSize, &num_read);
    if (status == ZX_OK) {
      msg_bytes += num_read;
      buffer.resize(num_read);
      stream_.AddReadData(std::move(buffer));
    } else {
      break;
    }
    // TODO(brettw) it would be nice to yield here after reading "a bunch" of
    // data so this pipe doesn't starve the entire app.
  }

  // Some readable events don't have any bytes in them. Don't trigger the
  // callback on those cases.
  if (msg_bytes == 0)
    return;

  if (callback_)
    callback_();
}

void BufferedZxSocket::OnSocketWritable(zx_handle_t) {
  // Now that the system told us it's ok to write, we go back to assuming it's always writable
  // until proven otherwise.
  watch_handle_ = {};
  MessageLoopTarget::Current()->WatchSocket(MessageLoop::WatchMode::kRead, socket_.get(), this,
                                            &watch_handle_);
  stream_.SetWritable();
}

void BufferedZxSocket::OnSocketError(zx_handle_t) {
  if (error_callback_)
    error_callback_();
}

size_t BufferedZxSocket::ConsumeStreamBufferData(const char* data, size_t len) {
  size_t written = 0;
  zx_status_t status = socket_.write(0, data, len, &written);
  if (status != ZX_OK && status != ZX_ERR_SHOULD_WAIT) {
    DEBUG_LOG(MessageLoop) << "Could not write to socket: " << zx_status_get_string(status);
    if (error_callback_)
      error_callback_();
    return 0;
  }

  // If we couldn't write some of the message, means the socket is full and we want the system to
  // tell us when it's ok to write again.
  if (written < len) {
    watch_handle_ = {};
    MessageLoopTarget::Current()->WatchSocket(MessageLoop::WatchMode::kReadWrite, socket_.get(),
                                              this, &watch_handle_);
  }
  return written;
}

}  // namespace debug_ipc
