// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_link_packet_source.h"

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/audio_object.h"
#include "src/media/audio/audio_core/audio_renderer_format_info.h"
#include "src/media/audio/audio_core/audio_renderer_impl.h"

namespace media::audio {

AudioLinkPacketSource::AudioLinkPacketSource(fbl::RefPtr<AudioObject> source,
                                             fbl::RefPtr<AudioObject> dest,
                                             fbl::RefPtr<AudioRendererFormatInfo> format_info)
    : AudioLink(SourceType::Packet, std::move(source), std::move(dest)),
      format_info_(std::move(format_info)) {}

AudioLinkPacketSource::~AudioLinkPacketSource() {
  pending_flush_packet_queue_.clear();
  pending_packet_queue_.clear();
  pending_flush_token_queue_.clear();
}

// static
fbl::RefPtr<AudioLinkPacketSource> AudioLinkPacketSource::Create(
    fbl::RefPtr<AudioObject> source, fbl::RefPtr<AudioObject> dest,
    fbl::RefPtr<AudioRendererFormatInfo> format) {
  FXL_DCHECK(source);
  FXL_DCHECK(dest);

  // TODO(mpuryear): Relax this when other audio objects can be packet sources.
  if (source->type() != AudioObject::Type::AudioRenderer) {
    FXL_LOG(ERROR) << "Cannot create packet source link; packet sources must "
                      "be AudioRenderers";
    return nullptr;
  }

  return fbl::AdoptRef(
      new AudioLinkPacketSource(std::move(source), std::move(dest), std::move(format)));
}

void AudioLinkPacketSource::PushToPendingQueue(const fbl::RefPtr<AudioPacketRef>& packet) {
  std::lock_guard<std::mutex> locker(pending_mutex_);
  pending_packet_queue_.emplace_back(std::move(packet));
}

void AudioLinkPacketSource::FlushPendingQueue(const fbl::RefPtr<PendingFlushToken>& flush_token) {
  std::deque<fbl::RefPtr<AudioPacketRef>> flushed_packets;

  {
    std::lock_guard<std::mutex> locker(pending_mutex_);

    flushed_ = true;

    if (processing_in_progress_) {
      // Is the sink currently mixing?  If so, the flush cannot complete until
      // the mix operation has finished.  Move the 'waiting to be rendered'
      // packets to the back of the 'waiting to be flushed queue', and append
      // our flush token (if any) to the pending flush token queue.  The sink's
      // thread will take are of releasing these objects back to the service
      // thread for cleanup when it has finished it's current job.
      for (auto& packet : pending_packet_queue_) {
        pending_flush_packet_queue_.emplace_back(std::move(packet));
      }
      pending_packet_queue_.clear();

      if (flush_token != nullptr) {
        pending_flush_token_queue_.emplace_back(std::move(flush_token));
      }

      return;
    } else {
      // If the sink is not currently mixing, then we just swap the contents the
      // pending packet queues with out local queue and release the packets in
      // the proper order once we have left the pending mutex lock.
      FXL_DCHECK(pending_flush_packet_queue_.empty());
      FXL_DCHECK(pending_flush_token_queue_.empty());
      flushed_packets.swap(pending_packet_queue_);
    }
  }

  // Release the packets, front to back.
  for (auto& ptr : flushed_packets) {
    ptr.reset();
  }
}

void AudioLinkPacketSource::CopyPendingQueue(const fbl::RefPtr<AudioLinkPacketSource>& other) {
  FXL_DCHECK(other != nullptr);
  FXL_DCHECK(this != other.get());

  std::lock_guard<std::mutex> source_locker(other->pending_mutex_);
  if (other->pending_packet_queue_.empty())
    return;

  std::lock_guard<std::mutex> locker(pending_mutex_);
  FXL_DCHECK(pending_packet_queue_.empty());
  pending_packet_queue_ = other->pending_packet_queue_;
}

fbl::RefPtr<AudioPacketRef> AudioLinkPacketSource::LockPendingQueueFront(bool* was_flushed) {
  FXL_DCHECK(was_flushed);
  std::lock_guard<std::mutex> locker(pending_mutex_);

  FXL_DCHECK(!processing_in_progress_);
  processing_in_progress_ = true;

  *was_flushed = flushed_;
  flushed_ = false;

  if (pending_packet_queue_.size()) {
    return pending_packet_queue_.front();
  } else {
    return nullptr;
  }
}

void AudioLinkPacketSource::UnlockPendingQueueFront(bool release_packet) {
  {
    std::lock_guard<std::mutex> locker(pending_mutex_);
    FXL_DCHECK(processing_in_progress_);
    processing_in_progress_ = false;

    // Did a flush take place while we were working?  If so release each of the
    // packets waiting to be flushed back to the service thread, then release
    // each of the flush tokens.
    if (!pending_flush_packet_queue_.empty() || !pending_flush_token_queue_.empty()) {
      for (auto& ptr : pending_flush_packet_queue_) {
        ptr.reset();
      }

      for (auto& ptr : pending_flush_token_queue_) {
        ptr.reset();
      }

      pending_flush_packet_queue_.clear();
      pending_flush_token_queue_.clear();

      return;
    }

    // If the sink wants us to release the front of the pending queue, and no
    // flush operation happened while they were processing, then there had
    // better be a packet at the front of the queue to release.

    // Assert that the user either got no packet when they locked the queue
    // (because the queue was empty), or that they got the front of the queue
    // and that the front of the queue has not changed.
    FXL_DCHECK(!release_packet || !pending_packet_queue_.empty());
    if (release_packet) {
      pending_packet_queue_.pop_front();
    }
  }
}

}  // namespace media::audio
