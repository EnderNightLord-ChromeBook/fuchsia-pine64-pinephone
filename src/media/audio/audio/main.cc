// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>

#include "src/media/audio/audio/audio_core_client.h"

int main(int argc, const char** argv) {
  syslog::SetTags({"audio_core_client"});

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  // ComponentContext is safe to initialize early as we publish all implemented interfaces before we
  // run the event loop.
  auto component_context = sys::ComponentContext::CreateAndServeOutgoingDirectory();

  auto closer = [&loop]() { async::PostTask(loop.dispatcher(), [&loop]() { loop.Quit(); }); };
  media::audio::AudioCoreClient audio_core(component_context.get(), closer);

  loop.Run();
  return 0;
}
