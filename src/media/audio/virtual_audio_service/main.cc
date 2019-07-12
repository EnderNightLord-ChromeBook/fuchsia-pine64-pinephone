// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>

#include "src/media/audio/virtual_audio_service/virtual_audio_service_impl.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);

  virtual_audio::VirtualAudioServiceImpl impl(sys::ComponentContext::Create());

  if (impl.Init() != ZX_OK) {
    return -1;
  }

  loop.Run();
  return 0;
}
