// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_PACKET_REF_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_PACKET_REF_H_

#include <fbl/intrusive_double_list.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fuchsia/media/cpp/fidl.h>
#include <stdint.h>

#include <memory>

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio {

class AudioCoreImpl;

// TODO(johngro): Consider moving instances of this class to a slab allocation
// pattern.  They are the most frequently allocated object in the mixer (easily
// 100s per second) and they do not live very long at all (300-400mSec at most),
// so they could easily be causing heap fragmentation issues.
class AudioPacketRef : public fbl::RefCounted<AudioPacketRef>,
                       public fbl::Recyclable<AudioPacketRef>,
                       public fbl::DoublyLinkedListable<std::unique_ptr<AudioPacketRef>> {
 public:
  using ReleaseHandler = fit::inline_function<void(std::unique_ptr<AudioPacketRef>), sizeof(void*)>;

  AudioPacketRef(fbl::RefPtr<RefCountedVmoMapper> vmo_ref,
                 fuchsia::media::AudioRenderer::SendPacketCallback callback,
                 fuchsia::media::StreamPacket packet, ReleaseHandler release_handler,
                 uint32_t frac_frame_len, int64_t start_pts);

  // Accessors for starting and ending presentation time stamps expressed in
  // units of audio frames (note, not media time), as signed 50.13 fixed point
  // integers (see kPtsFractionalBits).  At 192KHz, this allows for ~186.3
  // years of usable range when starting from a media time of 0.
  //
  // AudioPackets consumed by the AudioCore are all expected to have
  // explicit presentation time stamps.  If packets sent by the user are
  // missing timestamps, appropriate timestamps will be synthesized at this
  // point in the pipeline.
  //
  // Note, the start pts is the time at which the first frame of audio in the
  // packet should be presented.  The end_pts is the time at which the frame
  // after the final frame in the packet would be presented.
  //
  // TODO(johngro): Reconsider this.  It may be best to keep things expressed
  // simply in media time instead of converting to fractional units of renderer
  // frames.  If/when outputs move away from a single fixed step size for output
  // sampling, it will probably be best to just convert this back to media time.
  int64_t start_pts() const { return start_pts_; }
  int64_t end_pts() const { return end_pts_; }
  uint32_t frac_frame_len() const { return frac_frame_len_; }

  void Cleanup() {
    FXL_DCHECK(callback_ != nullptr);
    callback_();
  }
  void* payload() {
    auto start = reinterpret_cast<uint8_t*>(vmo_ref_->start());
    return (start + packet_.payload_offset);
  }
  uint32_t flags() const { return packet_.flags; }
  uint32_t payload_buffer_id() const { return packet_.payload_buffer_id; }

 protected:
  friend class fbl::RefPtr<AudioPacketRef>;
  friend class fbl::Recyclable<AudioPacketRef>;
  friend class std::default_delete<AudioPacketRef>;

  ~AudioPacketRef() = default;

  // Check to see if this packet has a valid callback.  If so, when it gets
  // recycled for the first time, it needs to be kept alive and posted to the
  // service's cleanup queue so that the user's callback gets called on the main
  // service dispatcher thread.
  bool NeedsCleanup() { return callback_ != nullptr; }

  fbl::RefPtr<RefCountedVmoMapper> vmo_ref_;
  fuchsia::media::AudioRenderer::SendPacketCallback callback_;
  fuchsia::media::StreamPacket packet_;

  uint32_t frac_frame_len_;
  int64_t start_pts_;
  int64_t end_pts_;
  bool was_recycled_ = false;

 private:
  void fbl_recycle();
  ReleaseHandler release_handler_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_PACKET_REF_H_
