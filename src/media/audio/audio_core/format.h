// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_FORMAT_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_FORMAT_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/media/cpp/timeline_rate.h>
#include <stdint.h>

namespace media::audio {

class Format {
 public:
  static fit::result<Format> Create(fuchsia::media::AudioStreamType stream_type);

  Format(const Format&) = default;
  Format& operator=(const Format&) = default;

  bool operator==(const Format& other) const;
  bool operator!=(const Format& other) const { return !(*this == other); }

  const fuchsia::media::AudioStreamType& stream_type() const { return stream_type_; }
  uint32_t channels() const { return stream_type_.channels; }
  uint32_t frames_per_second() const { return stream_type_.frames_per_second; }
  fuchsia::media::AudioSampleFormat sample_format() const { return stream_type_.sample_format; }

  const TimelineRate& frames_per_ns() const { return frames_per_ns_; }
  const TimelineRate& frame_to_media_ratio() const { return frame_to_media_ratio_; }
  uint32_t bytes_per_frame() const { return bytes_per_frame_; }
  uint32_t valid_bits_per_channel() const { return valid_bits_per_channel_; }

 private:
  Format(fuchsia::media::AudioStreamType stream_type, TimelineRate frames_per_ns,
         TimelineRate frame_to_media_ratio, uint32_t bytes_per_frame,
         uint32_t valid_bits_per_channel);

  fuchsia::media::AudioStreamType stream_type_;
  TimelineRate frames_per_ns_;
  TimelineRate frame_to_media_ratio_;
  uint32_t bytes_per_frame_;
  uint32_t valid_bits_per_channel_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_FORMAT_H_
