// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/fidl/fidl_audio_renderer.h"

#include "lib/async/default.h"
#include "lib/media/cpp/timeline_rate.h"
#include "src/lib/fxl/logging.h"
#include "src/media/playback/mediaplayer/fidl/fidl_type_conversions.h"
#include "src/media/playback/mediaplayer/graph/formatting.h"

namespace media_player {
namespace {

constexpr int64_t kDefaultMinLeadTime = ZX_MSEC(100);
constexpr int64_t kTargetLeadTimeDeltaNs = ZX_MSEC(10);

}  // namespace

// static
std::shared_ptr<FidlAudioRenderer> FidlAudioRenderer::Create(
    fuchsia::media::AudioRendererPtr audio_renderer) {
  return std::make_shared<FidlAudioRenderer>(std::move(audio_renderer));
}

FidlAudioRenderer::FidlAudioRenderer(fuchsia::media::AudioRendererPtr audio_renderer)
    : audio_renderer_(std::move(audio_renderer)), arrivals_(true), departures_(false) {
  FXL_DCHECK(audio_renderer_);

  // |demand_task_| is used to wake up when demand might transition from
  // negative to positive.
  demand_task_.set_handler([this]() {
    FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
    SignalCurrentDemand();
  });

  min_lead_time_ns_ = kDefaultMinLeadTime;
  target_lead_time_ns_ = min_lead_time_ns_ + kTargetLeadTimeDeltaNs;

  audio_renderer_.events().OnMinLeadTimeChanged = [this](int64_t min_lead_time_ns) {
    FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
    renderer_responding_ = true;

    if (min_lead_time_ns == 0) {
      // Ignore the zero we get during warmup.
      // TODO(dalesat): Remove check when MTWN-244 is fixed.
      return;
    }

    // Target lead time is somewhat greater than minimum lead time, so
    // we stay slightly ahead of the deadline.
    min_lead_time_ns_ = min_lead_time_ns;
    target_lead_time_ns_ = min_lead_time_ns_ + kTargetLeadTimeDeltaNs;
  };
  audio_renderer_->EnableMinLeadTimeEvents(true);

  supported_stream_types_.push_back(AudioStreamTypeSet::Create(
      {StreamType::kAudioEncodingLpcm}, AudioStreamType::SampleFormat::kUnsigned8,
      Range<uint32_t>(fuchsia::media::MIN_PCM_CHANNEL_COUNT, fuchsia::media::MAX_PCM_CHANNEL_COUNT),
      Range<uint32_t>(fuchsia::media::MIN_PCM_FRAMES_PER_SECOND,
                      fuchsia::media::MAX_PCM_FRAMES_PER_SECOND)));

  supported_stream_types_.push_back(AudioStreamTypeSet::Create(
      {StreamType::kAudioEncodingLpcm}, AudioStreamType::SampleFormat::kSigned16,
      Range<uint32_t>(fuchsia::media::MIN_PCM_CHANNEL_COUNT, fuchsia::media::MAX_PCM_CHANNEL_COUNT),
      Range<uint32_t>(fuchsia::media::MIN_PCM_FRAMES_PER_SECOND,
                      fuchsia::media::MAX_PCM_FRAMES_PER_SECOND)));

  supported_stream_types_.push_back(AudioStreamTypeSet::Create(
      {StreamType::kAudioEncodingLpcm}, AudioStreamType::SampleFormat::kFloat,
      Range<uint32_t>(fuchsia::media::MIN_PCM_CHANNEL_COUNT, fuchsia::media::MAX_PCM_CHANNEL_COUNT),
      Range<uint32_t>(fuchsia::media::MIN_PCM_FRAMES_PER_SECOND,
                      fuchsia::media::MAX_PCM_FRAMES_PER_SECOND)));
}

FidlAudioRenderer::~FidlAudioRenderer() { FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_); }

const char* FidlAudioRenderer::label() const { return "audio_renderer"; }

void FidlAudioRenderer::Dump(std::ostream& os) const {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  Renderer::Dump(os);

  os << fostr::Indent;
  os << fostr::NewLine << "priming:               " << !!prime_callback_;
  os << fostr::NewLine << "flushed:               " << flushed_;
  os << fostr::NewLine << "presentation time:     "
     << AsNs(current_timeline_function()(zx::clock::get_monotonic().get()));
  os << fostr::NewLine << "last supplied pts:     " << AsNs(last_supplied_pts_ns_);
  os << fostr::NewLine << "last departed pts:     " << AsNs(last_departed_pts_ns_);
  if (last_supplied_pts_ns_ != Packet::kNoPts && last_departed_pts_ns_ != Packet::kNoPts) {
    os << fostr::NewLine
       << "supplied - departed:   " << AsNs(last_supplied_pts_ns_ - last_departed_pts_ns_);
  }

  os << fostr::NewLine << "minimum lead time:     " << AsNs(min_lead_time_ns_);

  if (arrivals_.count() != 0) {
    os << fostr::NewLine << "packet arrivals: " << fostr::Indent << arrivals_ << fostr::Outdent;
  }

  if (departures_.count() != 0) {
    os << fostr::NewLine << "packet departures: " << fostr::Indent << departures_ << fostr::Outdent;
  }

  os << fostr::Outdent;
}

void FidlAudioRenderer::OnInputConnectionReady(size_t input_index) {
  FXL_DCHECK(input_index == 0);

  auto vmos = UseInputVmos().GetVmos();
  FXL_DCHECK(vmos.size() == 1);
  audio_renderer_->AddPayloadBuffer(
      0, vmos.front()->Duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_READ | ZX_RIGHT_MAP));
}

void FidlAudioRenderer::FlushInput(bool hold_frame_not_used, size_t input_index,
                                   fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(callback);

  flushed_ = true;
  SetEndOfStreamPts(Packet::kNoPts);
  input_packet_request_outstanding_ = false;

  audio_renderer_->DiscardAllPackets([this, callback = std::move(callback)]() {
    FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
    last_supplied_pts_ns_ = Packet::kNoPts;
    last_departed_pts_ns_ = Packet::kNoPts;
    callback();
  });
}

void FidlAudioRenderer::PutInputPacket(PacketPtr packet, size_t input_index) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(packet);
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(bytes_per_frame_ != 0);

  input_packet_request_outstanding_ = false;

  int64_t now = zx::clock::get_monotonic().get();

  if (packet->pts() == Packet::kNoPts) {
    if (!renderer_responding_) {
      // Discard this packet.
      SignalCurrentDemand();
      return;
    }

    // The packet has no PTS. We need to assign one. We prefer to use frame
    // units, so first make sure the PTS rate is set to frames.
    // TODO(dalesat): Remove this code when MTWN-243 is fixed.
    packet->SetPtsRate(pts_rate_);

    int64_t min_pts = from_ns(current_timeline_function()(now) + min_lead_time_ns_);

    if (next_pts_to_assign_ == Packet::kNoPts ||
        (packet->discontinuity() && min_pts > next_pts_to_assign_)) {
      // Set the packet's PTS to meet the deadline.
      packet->SetPts(min_pts);
    } else {
      // Set the packet's PTS to immediately follow the previous packet.
      packet->SetPts(next_pts_to_assign_);
    }
  }

  int64_t start_pts = packet->GetPts(pts_rate_);
  int64_t start_pts_ns = to_ns(start_pts);

  next_pts_to_assign_ = start_pts + packet->size() / bytes_per_frame_;

  last_supplied_pts_ns_ = to_ns(next_pts_to_assign_);
  if (last_departed_pts_ns_ == Packet::kNoPts) {
    last_departed_pts_ns_ = start_pts_ns;
  }

  if (flushed_ || last_supplied_pts_ns_ < min_pts(0) || start_pts_ns > max_pts(0)) {
    // Discard this packet.
    SignalCurrentDemand();
    return;
  }

  arrivals_.AddSample(now, current_timeline_function()(now), start_pts_ns, Progressing());

  if (packet->end_of_stream()) {
    SetEndOfStreamPts(last_supplied_pts_ns_);

    if (prime_callback_) {
      // We won't get any more packets, so we're as primed as we're going to
      // get.
      prime_callback_();
      prime_callback_ = nullptr;
    }
  }

  if (packet->size() == 0) {
    packet.reset();
  } else {
    fuchsia::media::StreamPacket audioPacket;
    audioPacket.pts = start_pts;
    audioPacket.payload_size = packet->size();
    audioPacket.payload_offset = packet->payload_buffer()->offset();
    audioPacket.flags =
        packet->discontinuity() ? fuchsia::media::STREAM_PACKET_FLAG_DISCONTINUITY : 0;

    audio_renderer_->SendPacket(audioPacket, [this, packet]() {
      FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
      int64_t now = zx::clock::get_monotonic().get();

      int64_t start_pts = packet->GetPts(pts_rate_);
      int64_t start_pts_ns = to_ns(start_pts);
      int64_t end_pts_ns = to_ns(start_pts + packet->size() / bytes_per_frame_);

      UpdateLastRenderedPts(end_pts_ns);

      last_departed_pts_ns_ = std::max(end_pts_ns, last_departed_pts_ns_);

      departures_.AddSample(now, current_timeline_function()(now), start_pts_ns, Progressing());

      SignalCurrentDemand();
    });
  }

  if (SignalCurrentDemand()) {
    return;
  }

  if (prime_callback_) {
    // We have all the packets we need and we're priming. Signal that priming
    // is complete.
    prime_callback_();
    prime_callback_ = nullptr;
  }
}

void FidlAudioRenderer::SetStreamType(const StreamType& stream_type) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(stream_type.audio());

  fuchsia::media::AudioStreamType audio_stream_type;
  audio_stream_type.sample_format =
      fidl::To<fuchsia::media::AudioSampleFormat>(stream_type.audio()->sample_format());
  audio_stream_type.channels = stream_type.audio()->channels();
  audio_stream_type.frames_per_second = stream_type.audio()->frames_per_second();

  audio_renderer_->SetPcmStreamType(std::move(audio_stream_type));

  // TODO: What about stream type changes?

  // Configure the input for a single VMO of adequate size.
  size_t size = stream_type.audio()->min_buffer_size(
      stream_type.audio()->frames_per_second());  // TODO How many seconds?

  if (ConfigureInputToUseVmos(size, 0, 0, VmoAllocation::kSingleVmo)) {
    OnInputConnectionReady(0);
  }

  // Tell the renderer that media time is in frames.
  audio_renderer_->SetPtsUnits(stream_type.audio()->frames_per_second(), 1);

  pts_rate_ = media::TimelineRate(stream_type.audio()->frames_per_second(), 1);
  bytes_per_frame_ = stream_type.audio()->bytes_per_frame();
}

void FidlAudioRenderer::Prime(fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (prime_callback_) {
    FXL_LOG(WARNING) << "Prime requested when priming was already in progress.";
    FXL_DCHECK(false);
    prime_callback_();
  }

  flushed_ = false;

  if (!NeedMorePackets() || end_of_stream_pending()) {
    callback();
    return;
  }

  prime_callback_ = std::move(callback);
  SignalCurrentDemand();
}

void FidlAudioRenderer::SetTimelineFunction(media::TimelineFunction timeline_function,
                                            fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  // AudioRenderer only supports 0/1 (paused) or 1/1 (normal playback rate).
  // TODO(dalesat): Remove this DCHECK when AudioRenderer supports other rates,
  // build an SRC into this class, or prohibit other rates entirely.
  FXL_DCHECK(timeline_function.subject_delta() == 0 ||
             (timeline_function.subject_delta() == 1 && timeline_function.reference_delta() == 1));

  Renderer::SetTimelineFunction(timeline_function, std::move(callback));

  if (timeline_function.subject_delta() == 0) {
    audio_renderer_->PauseNoReply();
  } else {
    int64_t presentation_time = from_ns(timeline_function.subject_time());
    audio_renderer_->PlayNoReply(timeline_function.reference_time(), presentation_time);
  }
}

void FidlAudioRenderer::BindGainControl(
    fidl::InterfaceRequest<fuchsia::media::audio::GainControl> gain_control_request) {
  audio_renderer_->BindGainControl(std::move(gain_control_request));
}

void FidlAudioRenderer::OnTimelineTransition() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  SignalCurrentDemand();
}

bool FidlAudioRenderer::NeedMorePackets() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  demand_task_.Cancel();

  if (flushed_ || end_of_stream_pending()) {
    // If we're flushed or we've seen end of stream, we don't need any more
    // packets.
    return false;
  }

  int64_t presentation_time_ns = current_timeline_function()(zx::clock::get_monotonic().get());

  if (last_supplied_pts_ns_ == Packet::kNoPts ||
      presentation_time_ns + target_lead_time_ns_ > last_supplied_pts_ns_) {
    // We need more packets to meet lead time commitments.
    return true;
  }

  if (!current_timeline_function().invertible()) {
    // We don't need packets now, and the timeline isn't progressing, so we
    // won't need packets until the timeline starts progressing.
    return false;
  }

  // We don't need packets now. Predict when we might need the next packet
  // and check then.
  demand_task_.PostForTime(dispatcher(), zx::time(current_timeline_function().ApplyInverse(
                                             last_supplied_pts_ns_ - target_lead_time_ns_)));

  return false;
}

bool FidlAudioRenderer::SignalCurrentDemand() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (input_packet_request_outstanding_) {
    return false;
  }

  if (!NeedMorePackets()) {
    return false;
  }

  input_packet_request_outstanding_ = true;
  RequestInputPacket();
  return true;
}

}  // namespace media_player
