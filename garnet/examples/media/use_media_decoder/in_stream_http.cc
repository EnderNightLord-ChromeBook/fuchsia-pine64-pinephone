// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "in_stream_http.h"

#include "lib/zx/time.h"
#include "src/lib/fxl/logging.h"
#include "util.h"

#include <lib/media/test/one_shot_event.h>
#include <zircon/errors.h>
#include <zircon/types.h>
#include <queue>
#include <tuple>

namespace {

// To date, likely ignored.  For now the MD5 hashing seems to be the bottleneck,
// with the CPU not idle much, so even if this is ignored, the socket buffering
// seems sufficient to avoid starving the overall pipe.
const uint32_t kResponseBodyBufferSize = 2 * 1024 * 1024;

}  // namespace

InStreamHttp::InStreamHttp(async::Loop* fidl_loop,
              thrd_t fidl_thread,
              component::StartupContext* startup_context,
              std::string url)
  : InStream(fidl_loop, fidl_thread, startup_context),
    url_(url) {
  ZX_DEBUG_ASSERT(thrd_current() != fidl_thread_);
  ZX_DEBUG_ASSERT(!url_.empty());

  // We're not runnign on the fidl_thread_, so we need to post over to the
  // fidl_thread_ for any binding, sending, etc.
  fuchsia::net::oldhttp::HttpServicePtr http_service;
  http_service.set_error_handler([](zx_status_t status){
    Exit("http_service failed - status: %lu", status);
  });
  startup_context_->ConnectToEnvironmentService(
      http_service.NewRequest(fidl_dispatcher_));

  url_loader_.set_error_handler([](zx_status_t status){
    Exit("url_loader_ failed - status: %lu", status);
  });
  PostToFidlSerial([&http_service, url_loader_request = url_loader_.NewRequest(fidl_dispatcher_)]() mutable {
    http_service->CreateURLLoader(std::move(url_loader_request));
  });

  fuchsia::net::oldhttp::URLRequest url_request{};
  url_request.url = url_;
  url_request.response_body_buffer_size = kResponseBodyBufferSize;
  url_request.auto_follow_redirects = true;
  url_request.cache_mode = fuchsia::net::oldhttp::CacheMode::BYPASS_CACHE;

  fuchsia::net::oldhttp::URLResponse response;
  OneShotEvent have_response_event;

  PostToFidlSerial([this, url_request = std::move(url_request), &response, &have_response_event]() mutable {
    url_loader_->Start(std::move(url_request), [&response, &have_response_event](fuchsia::net::oldhttp::URLResponse response_param){
      response = std::move(response_param);
      have_response_event.Signal();
    });
  });
  have_response_event.Wait(zx::deadline_after(zx::sec(30)));

  ZX_ASSERT_MSG(!response.error, "http response has error");
  ZX_ASSERT_MSG(response.body, "http response missing body");
  ZX_ASSERT_MSG(response.body->stream().is_valid(), "http response stream !is_valid()");

  if (response.headers) {
    for (auto& header : response.headers.get()) {
      // TODO(dustingreen): deal with chunked encoding, or switch to a new http
      // client impl that deals with de-chunking before we see the data. For now
      // we rely on the http server to not generate chunked encoding.
      ZX_ASSERT(!(header.name == "transfer-encoding" && header.value == "chunked"));
    }
  }

  socket_ = std::move(response.body->stream());
}

InStreamHttp::~InStreamHttp() {
  ZX_DEBUG_ASSERT(thrd_current() != fidl_thread_);

  // By fencing anything we've previously posted to fidl_thread, we avoid
  // touching "this" too late.
  PostToFidlSerial([this]{
    url_loader_.Unbind();
  });

  // After this call completes, we know the above post has run on fidl_thread_,
  // so no more code re. this instance will be running on fidl_thread_ (partly
  // because we Unbind()/reset() in the lambda above, and partly because we
  // never re-post from fidl_thread_).
  FencePostToFidlSerial();
}

zx_status_t InStreamHttp::ReadBytesInternal(uint32_t max_bytes_to_read,
                                            uint32_t* bytes_read_out,
                                            uint8_t* buffer_out,
                                            zx::time just_fail_deadline) {
  if (eos_position_known_ && cursor_position_ == eos_position_) {
    // Not possible to read more because there isn't any more.  Not a failure.
    *bytes_read_out = 0;
    return ZX_OK;
  }

  zx_signals_t pending{};
  zx_status_t status = socket_.wait_one(ZX_SOCKET_READABLE | ZX_SOCKET_PEER_CLOSED, just_fail_deadline, &pending);
  if (status != ZX_OK) {
    Exit("socket_ wait failed - status: %d", status);
  }

  if (pending & ZX_SOCKET_READABLE) {
    size_t len = max_bytes_to_read;
    size_t actual;
    status = socket_.read(0, static_cast<void*>(buffer_out), len, &actual);
    if (status != ZX_OK) {
      Exit("socket_.read() failed - status: %d", status);
    }
    *bytes_read_out = actual;
    return ZX_OK;
  } else if (pending & ZX_SOCKET_PEER_CLOSED) {
    // Only handle this after ZX_SOCKET_READABLE, because we must assume this
    // means EOS and we don't want to miss any data that was sent before EOS.
    //
    // If both READABLE and PEER_CLOSED are set, we have to assume that more may
    // be readable, so we intentionally only handle PEER_CLOSED when PEER_CLOSED
    // && !READABLE.
    *bytes_read_out = 0;
    // InStream::ReadBytesShort() takes care of seting eos_position_known_ on
    // return from this method, so we don't need to do that here.
    return ZX_OK;
  } else {
    Exit("socket_ wait returned success but neither signal set?");
  }
  FXL_NOTREACHED();
  return ZX_ERR_INTERNAL;
}
