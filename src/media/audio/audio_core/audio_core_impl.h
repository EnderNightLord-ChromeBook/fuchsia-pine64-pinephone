// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CORE_IMPL_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CORE_IMPL_H_

#include <fbl/intrusive_double_list.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/media/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fzl/vmar-manager.h>
#include <lib/sys/cpp/component_context.h>

#include <mutex>

#include "lib/fidl/cpp/binding_set.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/audio_packet_ref.h"
#include "src/media/audio/audio_core/fwd_decls.h"
#include "src/media/audio/audio_core/pending_flush_token.h"

namespace component {
class Services;
}

namespace media::audio {

class AudioCoreImpl : public fuchsia::media::AudioCore {
 public:
  AudioCoreImpl(std::unique_ptr<sys::ComponentContext> startup_context);
  ~AudioCoreImpl() override;

  // Audio implementation.
  void CreateAudioRenderer(
      fidl::InterfaceRequest<fuchsia::media::AudioRenderer> audio_renderer_request) final;

  void CreateAudioCapturer(
      bool loopback,
      fidl::InterfaceRequest<fuchsia::media::AudioCapturer> audio_capturer_request) final;

  void SetSystemGain(float gain_db) final;
  void SetSystemMute(bool muted) final;

  void SetRoutingPolicy(fuchsia::media::AudioOutputRoutingPolicy policy) final;

  void EnableDeviceSettings(bool enabled) final;

  // Called (indirectly) by AudioOutputs to schedule the callback for a
  // packet was queued to an AudioRenderer.
  //
  // TODO(johngro): This bouncing through thread contexts is inefficient and
  // will increase the latency requirements for clients (its going to take them
  // some extra time to discover that their media has been completely consumed).
  // When fidl exposes a way to safely invoke interface method callbacks from
  // threads other than the thread which executed the method itself, we will
  // want to switch to creating the callback message directly, instead of
  // indirecting through the service.
  void SchedulePacketCleanup(std::unique_ptr<AudioPacketRef> packet);
  void ScheduleFlushCleanup(std::unique_ptr<PendingFlushToken> token);

  // Schedule a closure to run on the service's main message loop.
  void ScheduleMainThreadTask(fit::closure task) {
    FXL_DCHECK(dispatcher_);
    async::PostTask(dispatcher_, std::move(task));
  }

  // Direct access to the service's async_t
  async_dispatcher_t* dispatcher() const { return dispatcher_; }

  // Accessor for our encapsulated device manager.
  AudioDeviceManager& GetDeviceManager() { return device_manager_; }

  float system_gain_db() const { return system_gain_db_; }
  bool system_muted() const { return system_muted_; }

  fbl::RefPtr<fzl::VmarManager> vmar() const { return vmar_manager_; }

  void SetRenderUsageGain(fuchsia::media::AudioRenderUsage usage, float gain_db) final;
  void SetCaptureUsageGain(fuchsia::media::AudioCaptureUsage usage, float gain_db) final;

  float GetRenderUsageGain(fuchsia::media::AudioRenderUsage usage);
  float GetCaptureUsageGain(fuchsia::media::AudioCaptureUsage usage);

 private:
  static constexpr float kDefaultSystemGainDb = -12.0f;
  static constexpr bool kDefaultSystemMuted = false;
  static constexpr float kMaxSystemAudioGainDb = Gain::kUnityGainDb;

  void NotifyGainMuteChanged();
  void PublishServices();
  void Shutdown();
  void DoPacketCleanup();

  fidl::BindingSet<fuchsia::media::AudioCore> bindings_;

  // A reference to our thread's dispatcher object.  Allows us to post events to
  // be handled by our main application thread from things like the output
  // manager's thread pool.
  async_dispatcher_t* dispatcher_;

  // State for dealing with devices.
  AudioDeviceManager device_manager_;

  std::unique_ptr<sys::ComponentContext> ctx_;

  // State for dealing with cleanup tasks.
  std::mutex cleanup_queue_mutex_;
  fbl::DoublyLinkedList<std::unique_ptr<AudioPacketRef>> packet_cleanup_queue_
      FXL_GUARDED_BY(cleanup_queue_mutex_);
  fbl::DoublyLinkedList<std::unique_ptr<PendingFlushToken>> flush_cleanup_queue_
      FXL_GUARDED_BY(cleanup_queue_mutex_);
  bool cleanup_scheduled_ FXL_GUARDED_BY(cleanup_queue_mutex_) = false;
  bool shutting_down_ = false;

  // TODO(johngro): remove this state.  Migrate users to AudioDeviceEnumerator,
  // to control gain on a per-input/output basis.
  // Either way, Gain and Mute should remain fully independent.
  float system_gain_db_ = kDefaultSystemGainDb;
  bool system_muted_ = kDefaultSystemMuted;

  // We allocate a sub-vmar to hold the audio renderer buffers. Keeping these
  // in a sub-vmar allows us to take advantage of ASLR while minimizing page
  // table fragmentation.
  fbl::RefPtr<fzl::VmarManager> vmar_manager_;

  FXL_DISALLOW_COPY_AND_ASSIGN(AudioCoreImpl);
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_CORE_IMPL_H_
