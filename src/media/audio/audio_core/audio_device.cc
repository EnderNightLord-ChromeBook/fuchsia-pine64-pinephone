// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_device.h"

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/audio_link.h"
#include "src/media/audio/audio_core/audio_output.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio {

namespace {
std::string AudioDeviceUniqueIdToString(const audio_stream_unique_id_t& id) {
  static_assert(sizeof(id.data) == 16, "Unexpected unique ID size");
  char buf[(sizeof(id.data) * 2) + 1];

  const auto& d = id.data;
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13],
           d[14], d[15]);
  return std::string(buf, sizeof(buf) - 1);
}
}  // namespace

AudioDevice::AudioDevice(AudioObject::Type type, AudioDeviceManager* manager)
    : AudioObject(type), manager_(manager), driver_(new AudioDriver(this)) {
  FXL_DCHECK(manager_);
  FXL_DCHECK((type == Type::Input) || (type == Type::Output));
}

AudioDevice::~AudioDevice() {
  FXL_DCHECK(is_shutting_down());
  FXL_DCHECK(!device_settings_ || !device_settings_->InContainer());
}

void AudioDevice::Wakeup() {
  FXL_DCHECK(mix_wakeup_ != nullptr);
  mix_wakeup_->Signal();
}

uint64_t AudioDevice::token() const {
  return driver_ ? driver_->stream_channel_koid() : ZX_KOID_INVALID;
}

// Change a device's gain, propagating the change to the affected links.
void AudioDevice::SetGainInfo(const fuchsia::media::AudioGainInfo& info, uint32_t set_flags) {
  // Limit the request to what the hardware can support
  fuchsia::media::AudioGainInfo limited = info;
  ApplyGainLimits(&limited, set_flags);

  // For outputs, change the gain of all links where it is the destination.
  if (is_output()) {
    fbl::AutoLock links_lock(&links_lock_);
    for (auto& link : source_links_) {
      if (link.GetSource()->type() == AudioObject::Type::AudioRenderer) {
        link.bookkeeping()->gain.SetDestMute(limited.flags &
                                             fuchsia::media::AudioGainInfoFlag_Mute);
        link.bookkeeping()->gain.SetDestGain(limited.gain_db);
      }
    }
  } else {
    // For inputs, change the gain of all links where it is the source.
    FXL_DCHECK(is_input());
    fbl::AutoLock links_lock(&links_lock_);
    for (auto& link : dest_links_) {
      if (link.GetDest()->type() == AudioObject::Type::AudioCapturer) {
        link.bookkeeping()->gain.SetSourceMute(limited.flags &
                                               fuchsia::media::AudioGainInfoFlag_Mute);
        link.bookkeeping()->gain.SetSourceGain(limited.gain_db);
      }
    }
  }

  FXL_DCHECK(device_settings_ != nullptr);
  if (device_settings_->SetGainInfo(limited, set_flags)) {
    Wakeup();
  }
}

zx_status_t AudioDevice::Init() {
  // TODO(johngro) : See ZX-940.  Eliminate this priority boost as soon as we
  // have a more official way of meeting real-time latency requirements.

  zx::profile profile;
  zx_status_t res = AcquireHighPriorityProfile(&profile);
  if (res != ZX_OK) {
    return res;
  }

  mix_domain_ = dispatcher::ExecutionDomain::Create(std::move(profile));
  mix_wakeup_ = dispatcher::WakeupEvent::Create();

  if ((mix_domain_ == nullptr) || (mix_wakeup_ == nullptr)) {
    return ZX_ERR_NO_MEMORY;
  }

  dispatcher::WakeupEvent::ProcessHandler process_handler(
      [output = fbl::WrapRefPtr(this)](dispatcher::WakeupEvent* event) -> zx_status_t {
        OBTAIN_EXECUTION_DOMAIN_TOKEN(token, output->mix_domain_);
        output->OnWakeup();
        return ZX_OK;
      });

  res = mix_wakeup_->Activate(mix_domain_, std::move(process_handler));
  if (res != ZX_OK) {
    FXL_PLOG(ERROR, res) << "Failed to activate wakeup event for AudioDevice";
    return res;
  }

  return ZX_OK;
}

void AudioDevice::Cleanup() {
  // ThrottleOutput devices have no driver, so check for that.
  if (driver_ != nullptr) {
    // Instruct the driver to release all its resources (channels, timer).
    driver_->Cleanup();
  }
}

void AudioDevice::ActivateSelf() {
  // If we aren't shutting down, tell DeviceManager we are ready for work.
  if (!is_shutting_down()) {
    // Create default settings. The device manager will restore these settings
    // from persistent storage for us when it gets our activation message.
    FXL_DCHECK(device_settings_ == nullptr);
    FXL_DCHECK(driver() != nullptr);
    device_settings_ = AudioDeviceSettings::Create(*driver(), is_input());

    // Now poke our manager.
    FXL_DCHECK(manager_);
    manager_->ScheduleMainThreadTask([manager = manager_, self = fbl::WrapRefPtr(this)]() {
      manager->ActivateDevice(std::move(self));
    });
  }
}

void AudioDevice::ShutdownSelf() {
  // If we are not already in the process of shutting down, send a message to
  // the main message loop telling it to complete the shutdown process.
  if (!is_shutting_down()) {
    // TODO(mpuryear): Considering eliminating this; it may not be needed.
    PreventNewLinks();

    FXL_DCHECK(mix_domain_);
    mix_domain_->DeactivateFromWithinDomain();

    FXL_DCHECK(manager_);
    manager_->ScheduleMainThreadTask(
        [manager = manager_, self = fbl::WrapRefPtr(this)]() { manager->RemoveDevice(self); });
  }
}

void AudioDevice::DeactivateDomain() {
  if (mix_domain_ != nullptr) {
    mix_domain_->Deactivate();
  }
}

zx_status_t AudioDevice::Startup() {
  // If our derived class failed to initialize, Just get out.  We are being
  // called by the output manager, and they will remove us from the set of
  // active outputs as a result of us failing to initialize.
  zx_status_t res = Init();
  if (res != ZX_OK) {
    DeactivateDomain();
    return res;
  }

  // Poke the output once so it gets a chance to actually start running.
  Wakeup();

  return ZX_OK;
}

void AudioDevice::Shutdown() {
  if (shut_down_) {
    return;
  }

  // Make sure no new callbacks can be generated, and that pending callbacks
  // have been nerfed.
  DeactivateDomain();

  // Unlink ourselves from everything we are currently attached to.
  Unlink();

  // Give our derived class, and our driver, a chance to clean up resources.
  Cleanup();

  // We are now completely shut down.  The only reason we have this flag is to
  // make sure that Shutdown is idempotent.
  shut_down_ = true;
}

bool AudioDevice::UpdatePlugState(bool plugged, zx_time_t plug_time) {
  if ((plugged != plugged_) && (plug_time >= plug_time_)) {
    plugged_ = plugged;
    plug_time_ = plug_time;
    return true;
  }

  return false;
}

const fbl::RefPtr<DriverRingBuffer>& AudioDevice::driver_ring_buffer() const {
  return driver_->ring_buffer();
};

const TimelineFunction& AudioDevice::driver_clock_mono_to_ring_pos_bytes() const {
  return driver_->clock_mono_to_ring_pos_bytes();
};

void AudioDevice::GetDeviceInfo(fuchsia::media::AudioDeviceInfo* out_info) const {
  const auto& drv = *driver();
  out_info->name = drv.manufacturer_name() + ' ' + drv.product_name();
  out_info->unique_id = AudioDeviceUniqueIdToString(drv.persistent_unique_id());
  out_info->token_id = token();
  out_info->is_input = is_input();
  out_info->is_default = false;

  FXL_DCHECK(device_settings_);
  device_settings_->GetGainInfo(&out_info->gain_info);
}

}  // namespace media::audio
