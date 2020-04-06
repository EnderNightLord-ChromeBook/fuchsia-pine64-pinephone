// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/plug_detector.h"

#include <fcntl.h>
#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <lib/fdio/fdio.h>
#include <lib/fit/defer.h>
#include <lib/zx/channel.h>
#include <zircon/compiler.h>

#include <memory>
#include <vector>

#include <fbl/unique_fd.h>
#include <trace/event.h>

#include "src/lib/fsl/io/device_watcher.h"
#include "src/lib/syslog/cpp/logger.h"
#include "src/media/audio/audio_core/reporter.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {
namespace {

static const struct {
  const char* path;
  bool is_input;
  bool is_legacy;
} AUDIO_DEVNODES[] = {
    {.path = "/dev/class/audio-output", .is_input = false, .is_legacy = true},
    {.path = "/dev/class/audio-input", .is_input = true, .is_legacy = true},
    {.path = "/dev/class/audio-output-2", .is_input = false, .is_legacy = false},
    {.path = "/dev/class/audio-input-2", .is_input = true, .is_legacy = false},
};

class PlugDetectorImpl : public PlugDetector {
 public:
  zx_status_t Start(Observer observer) final {
    TRACE_DURATION("audio", "PlugDetectorImpl::Start");
    // Start should only be called once.
    FX_DCHECK(watchers_.empty());
    FX_DCHECK(!observer_);
    FX_DCHECK(observer);

    observer_ = std::move(observer);

    // If we fail to set up monitoring for any of our target directories,
    // automatically stop monitoring all sources of device nodes.
    auto error_cleanup = fit::defer([this]() { Stop(); });

    // Create our watchers.
    for (const auto& devnode : AUDIO_DEVNODES) {
      auto watcher = fsl::DeviceWatcher::Create(
          devnode.path, [this, is_input = devnode.is_input, is_legacy = devnode.is_legacy](
                            int dir_fd, const std::string& filename) {
            AddAudioDevice(dir_fd, filename, is_input, is_legacy);
          });

      if (watcher == nullptr) {
        AUD_VLOG(TRACE) << "PlugDetectorImpl failed to create DeviceWatcher for \"" << devnode.path
                        << "\".";
      }

      watchers_.emplace_back(std::move(watcher));
    }

    error_cleanup.cancel();

    return ZX_OK;
  }

  void Stop() final {
    TRACE_DURATION("audio", "PlugDetectorImpl::Stop");
    observer_ = nullptr;
    watchers_.clear();
  }

 private:
  void AddAudioDevice(int dir_fd, const std::string& name, bool is_input, bool is_legacy) {
    TRACE_DURATION("audio", "PlugDetectorImpl::AddAudioDevice");
    if (!observer_) {
      return;
    }

    // Open the device node.
    //
    // TODO(35145): Remove blocking 'openat' from the main thread. fdio_open_at is probably what we
    // want, but we'll need a version of DeviceWatcher that operates on fuchsia::io::Directory
    // handles instead of file descriptors.
    fbl::unique_fd dev_node(openat(dir_fd, name.c_str(), O_RDONLY));
    if (!dev_node.is_valid()) {
      REPORT(FailedToOpenDevice(name, is_input, errno));
      FX_LOGS(ERROR) << "PlugDetectorImpl failed to open device node at \"" << name << "\". ("
                     << strerror(errno) << " : " << errno << ")";
      return;
    }

    // Obtain the FDIO device channel, wrap it in a sync proxy, use that to get the stream channel.
    zx_status_t res;
    zx::channel dev_channel;
    res = fdio_get_service_handle(dev_node.release(), dev_channel.reset_and_get_address());
    if (res != ZX_OK) {
      REPORT(FailedToObtainFdioServiceChannel(name, is_input, res));
      FX_PLOGS(ERROR, res) << "Failed to obtain FDIO service channel to audio "
                           << (is_input ? "input" : "output");
      return;
    }

    // Obtain the stream channel
    auto device =
        fidl::InterfaceHandle<fuchsia::hardware::audio::Device>(std::move(dev_channel)).Bind();
    device.set_error_handler([name, is_input](zx_status_t res) {
      REPORT(FailedToObtainStreamChannel(name, is_input, res));
      FX_PLOGS(ERROR, res) << "Failed to open channel to audio " << (is_input ? "input" : "output");
    });
    device->GetChannel([d = std::move(device), this, is_input, name](
                           ::fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig> intf) {
      observer_(intf.TakeChannel(), name, is_input);
    });
  }
  Observer observer_;
  std::vector<std::unique_ptr<fsl::DeviceWatcher>> watchers_;
};

}  // namespace

std::unique_ptr<PlugDetector> PlugDetector::Create() {
  return std::make_unique<PlugDetectorImpl>();
}

}  // namespace media::audio
