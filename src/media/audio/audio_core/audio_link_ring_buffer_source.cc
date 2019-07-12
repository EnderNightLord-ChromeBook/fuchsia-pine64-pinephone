// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_link_ring_buffer_source.h"

#include "src/media/audio/audio_core/audio_device.h"

namespace media::audio {

// static
fbl::RefPtr<AudioLinkRingBufferSource> AudioLinkRingBufferSource::Create(
    fbl::RefPtr<AudioDevice> source, fbl::RefPtr<AudioObject> dest) {
  return fbl::AdoptRef(new AudioLinkRingBufferSource(std::move(source), std::move(dest)));
}

AudioLinkRingBufferSource::AudioLinkRingBufferSource(fbl::RefPtr<AudioDevice> source,
                                                     fbl::RefPtr<AudioObject> dest)
    : AudioLink(SourceType::RingBuffer, std::move(source), std::move(dest)) {}

}  // namespace media::audio
