// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/net/oldhttp/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/sys/cpp/component_context.h>

#include <string>

#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/macros.h"

namespace examples {

namespace http = ::fuchsia::net::oldhttp;

class ResponsePrinter {
 public:
  void Run(async::Loop* loop, http::URLResponse response) const {
    if (response.error) {
      printf("Got error: %d (%s)\n", response.error->code,
             response.error->description.value_or("").c_str());
      exit(1);
    } else {
      PrintResponse(response);
      PrintResponseBody(std::move(response.body->stream()));
    }

    loop->Quit();  // All done!
  }

  void PrintResponse(const http::URLResponse& response) const {
    printf(">>> Headers <<< \n");
    printf("  %s\n", response.status_line.value_or("").c_str());
    if (response.headers) {
      for (size_t i = 0; i < response.headers->size(); ++i)
        printf("  %s=%s\n", response.headers->at(i).name.c_str(),
               response.headers->at(i).value.c_str());
    }
  }

  void PrintResponseBody(zx::socket body) const {
    // Read response body in blocking fashion.
    printf(">>> Body <<<\n");

    for (;;) {
      char buf[512];
      size_t num_bytes = sizeof(buf);
      zx_status_t result = body.read(0u, buf, num_bytes, &num_bytes);

      if (result == ZX_ERR_SHOULD_WAIT) {
        body.wait_one(ZX_SOCKET_READABLE | ZX_SOCKET_PEER_CLOSED, zx::time::infinite(), nullptr);
      } else if (result == ZX_ERR_PEER_CLOSED) {
        // not an error
        break;
      } else if (result == ZX_OK) {
        if (fwrite(buf, num_bytes, 1, stdout) != 1) {
          printf("\nUnexpected error writing to file\n");
          break;
        }
      } else {
        printf("\nUnexpected error reading response %d\n", result);
        break;
      }
    }

    printf("\n>>> EOF <<<\n");
  }
};

class WGetApp {
 public:
  WGetApp(async::Loop* loop) : loop_(loop), context_(sys::ComponentContext::Create()) {
    http_service_ = context_->svc()->Connect<http::HttpService>();
    FXL_DCHECK(http_service_);
  }

  bool Start(const std::vector<std::string>& args) {
    if (args.size() == 1) {
      printf("usage: %s url\n", args[0].c_str());
      return false;
    }
    std::string url(args[1]);
    if (url.find("://") == std::string::npos) {
      url.insert(0, "http://");
    }
    printf("Loading: %s\n", url.c_str());

    http_service_->CreateURLLoader(url_loader_.NewRequest());

    http::URLRequest request;
    request.url = url;
    request.method = "GET";
    request.auto_follow_redirects = true;

    url_loader_->Start(std::move(request), [this](http::URLResponse response) {
      ResponsePrinter printer;
      printer.Run(loop_, std::move(response));
    });
    return true;
  }

 private:
  async::Loop* const loop_;
  std::unique_ptr<sys::ComponentContext> context_;

  http::HttpServicePtr http_service_;
  http::URLLoaderPtr url_loader_;
};

}  // namespace examples

int main(int argc, const char** argv) {
  std::vector<std::string> args(argv, argv + argc);
  async::Loop loop(&kAsyncLoopConfigAttachToThread);

  examples::WGetApp app(&loop);
  if (app.Start(args))
    loop.Run();

  return 0;
}
