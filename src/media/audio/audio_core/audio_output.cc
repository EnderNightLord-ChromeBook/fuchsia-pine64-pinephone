// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/media/audio/audio_core/audio_output.h"

#include <lib/fit/defer.h>
#include <lib/zx/clock.h>

#include <limits>

#include <trace/event.h>

#include "src/media/audio/audio_core/audio_renderer_impl.h"
#include "src/media/audio/audio_core/mixer/mixer.h"
#include "src/media/audio/audio_core/mixer/no_op.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {

static constexpr zx::duration kMaxTrimPeriod = zx::msec(10);

AudioOutput::AudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry)
    : AudioDevice(Type::Output, threading_model, registry) {
  next_sched_time_ = async::Now(mix_domain().dispatcher());
  next_sched_time_known_ = true;
  source_link_refs_.reserve(16u);
}

void AudioOutput::Process() {
  auto now = async::Now(mix_domain().dispatcher());
  bool needs_trim = true;

  int64_t trace_wake_delta = next_sched_time_known_ ?  (now - next_sched_time_).get() : 0;
  TRACE_DURATION("audio", "AudioOutput::Process", "wake delta", TA_INT64(trace_wake_delta));

  // At this point, we should always know when our implementation would like to be called to do some
  // mixing work next. If we do not know, then we should have already shut down.
  //
  // If the next sched time has not arrived yet, don't attempt to mix anything. Just trim the queues
  // and move on.
  FX_DCHECK(next_sched_time_known_);
  if (now >= next_sched_time_) {
    // Clear the flag. If the implementation does not set it during the cycle by calling
    // SetNextSchedTime, we consider it an error and shut down.
    next_sched_time_known_ = false;

    // As long as our implementation wants to mix more and has not run into a problem trying to
    // finish the mix job, mix some more.
    memset(&cur_mix_job_, 0, sizeof(cur_mix_job_));

    auto mix_frames = StartMixJob(&cur_mix_job_, now);
    if (mix_frames) {
      // If we have a mix job, then we must have an intermediate buffer allocated, and it must be
      // large enough for the mix job we were given.
      FX_DCHECK(mix_buf_);
      FX_DCHECK(cur_mix_job_.buf_frames <= mix_buf_frames_);
      FX_DCHECK(mix_format_);

      cur_mix_job_.buf = mix_buf_.get();
      cur_mix_job_.buf_frames = mix_frames->length;
      cur_mix_job_.start_pts_of = mix_frames->start;

      // Fill the intermediate buffer with silence.
      size_t bytes_to_zero =
          sizeof(cur_mix_job_.buf[0]) * cur_mix_job_.buf_frames * mix_format_->channels();
      std::memset(cur_mix_job_.buf, 0, bytes_to_zero);

      // If we are not muted, actually do the mix.
      if (!cur_mix_job_.sw_output_muted) {
        ForEachSource(TaskType::Mix, now);
        // If we mix we don't need to trim, since any packets will be released by the mix loop.
        needs_trim = false;
      }

      FinishMixJob(cur_mix_job_);
    }
  }

  if (needs_trim) {
    ForEachSource(TaskType::Trim, now);
  }

  if (!next_sched_time_known_) {
    FX_LOGS(ERROR) << "Output failed to schedule next service time. Shutting down!";
    ShutdownSelf();
    return;
  }

  // Figure out when we should wake up to do more work again. No matter how long our implementation
  // wants to wait, we need to make sure to wake up and periodically trim our input queues.
  auto max_sched_time = now + kMaxTrimPeriod;
  if (next_sched_time_ > max_sched_time) {
    next_sched_time_ = max_sched_time;
  }
  zx_status_t status = mix_timer_.PostForTime(mix_domain().dispatcher(), next_sched_time_);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to schedule mix";
    ShutdownSelf();
  }
}

zx_status_t AudioOutput::InitializeSourceLink(const fbl::RefPtr<AudioLink>& link) {
  TRACE_DURATION("audio", "AudioOutput::InitializeSourceLink");

  // If we have an output, pick a mixer based on the input and output formats. Otherwise, we only
  // need a NoOp mixer (for the time being).
  std::unique_ptr<Mixer> mixer;
  auto stream = link->stream();
  if (mix_format_ && stream) {
    mixer = Mixer::Select(stream->format().stream_type(), mix_format_->stream_type());
  } else {
    mixer = std::make_unique<audio::mixer::NoOp>();
  }

  if (mixer == nullptr) {
    FX_LOGS(ERROR)
        << "*** Audio system mixer cannot convert between formats *** (could not select mixer "
           "while linking to output). Usually, this indicates a 'num_channels' mismatch.";
    return ZX_ERR_NOT_SUPPORTED;
  }

  // The Gain object contains multiple stages. In render, stream gain is "source" gain and device
  // (or system) gain is "dest" gain.
  //
  // The renderer will set this link's source gain once this call returns.
  //
  // Set the dest gain -- device gain retrieved from device settings.
  const auto& settings = device_settings();
  if (settings != nullptr) {
    AudioDeviceSettings::GainState cur_gain_state;
    settings->SnapshotGainState(&cur_gain_state);

    mixer->bookkeeping().gain.SetDestGain(
        cur_gain_state.muted
            ? fuchsia::media::audio::MUTED_GAIN_DB
            : fbl::clamp(cur_gain_state.gain_db, Gain::kMinGainDb, Gain::kMaxGainDb));
  }

  link->set_mixer(std::move(mixer));
  return ZX_OK;
}

// Create our intermediate accumulation buffer.
void AudioOutput::SetupMixBuffer(uint32_t max_mix_frames) {
  TRACE_DURATION("audio", "AudioOutput::SetupMixBuffer");
  FX_DCHECK(mix_format_ && mix_format_->channels() > 0u);
  FX_DCHECK(max_mix_frames > 0u);
  FX_DCHECK(static_cast<uint64_t>(max_mix_frames) * mix_format_->channels() <=
            std::numeric_limits<uint32_t>::max());

  mix_buf_frames_ = max_mix_frames;
  mix_buf_ = std::make_unique<float[]>(mix_buf_frames_ * mix_format_->channels());
}

void AudioOutput::ForEachSource(TaskType task_type, zx::time ref_time) {
  TRACE_DURATION("audio", "AudioOutput::ForEachSource");
  // Make a copy of our currently active set of links so that we don't have to hold onto mutex_ for
  // the entire mix operation.
  {
    std::lock_guard<std::mutex> links_lock(links_lock_);
    ZX_DEBUG_ASSERT(source_link_refs_.empty());
    for (auto& link : source_links_) {
      source_link_refs_.emplace_back(fbl::RefPtr(&link));
    }
  }

  // In all cases, release our temporary references upon leaving this method.
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS { source_link_refs_.clear(); });

  for (const auto& link : source_link_refs_) {
    // Quit early if we should be shutting down.
    if (is_shutting_down()) {
      return;
    }

    // Is the link still valid?  If so, process it.
    if (!link->valid()) {
      continue;
    }

    auto stream = link->stream();
    if (!stream) {
      continue;
    }

    auto mixer = link->mixer();
    FX_DCHECK(mixer != nullptr);
    auto& info = mixer->bookkeeping();

    // Ensure the mapping from source-frame to local-time is up-to-date.
    UpdateSourceTrans(*stream, &info);

    bool setup_done = false;
    std::optional<Stream::Buffer> stream_buffer;

    bool release_buffer;
    while (true) {
      release_buffer = false;
      // Try to grab the packet queue's front.
      stream_buffer = stream->LockBuffer();

      // If the queue is empty, then we are done.
      if (!stream_buffer) {
        break;
      }

      // If the packet is discontinuous, reset our mixer's internal filter state.
      if (!stream_buffer->is_continuous()) {
        mixer->Reset();
      }

      // If we have not set up for this renderer yet, do so. If the setup fails for any reason, stop
      // processing packets for this renderer.
      if (!setup_done) {
        if (task_type == TaskType::Mix) {
          SetupMix(mixer);
        } else {
          SetupTrim(mixer, ref_time);
        }
        setup_done = true;
      }

      // Now process the packet at the front of the renderer's queue. If the packet has been
      // entirely consumed, pop it off the front and proceed to the next. Otherwise, we are done.
      release_buffer = (task_type == TaskType::Mix)
                           ? ProcessMix(stream.get(), mixer, *stream_buffer)
                           : ProcessTrim(*stream_buffer);

      // If we have mixed enough destination frames, we are done with this mix, regardless of what
      // we should now do with the source packet.
      if ((task_type == TaskType::Mix) &&
          (cur_mix_job_.frames_produced == cur_mix_job_.buf_frames)) {
        break;
      }
      // If we still need to produce more destination data, but could not complete this source
      // packet (we're paused, or the packet is in the future), then we are done.
      if (!release_buffer) {
        break;
      }
      // We did consume this entire source packet, and we should keep mixing.
      stream_buffer = std::nullopt;
      stream->UnlockBuffer(release_buffer);
    }

    // Unlock queue (completing packet if needed) and proceed to the next source.
    stream_buffer = std::nullopt;
    stream->UnlockBuffer(release_buffer);

    // Note: there is no point in doing this for Trim tasks, but it doesn't hurt anything, and it's
    // easier than adding another function to ForEachSource to run after each renderer is processed,
    // just to set this flag.
    cur_mix_job_.accumulate = true;
  }
}

void AudioOutput::SetupMix(Mixer* mixer) {
  TRACE_DURATION("audio", "AudioOutput::SetupMix");
  // If we need to recompose our transformation from destination frame space to source fractional
  // frames, do so now.
  FX_DCHECK(mixer);
  UpdateDestTrans(cur_mix_job_, &mixer->bookkeeping());
  cur_mix_job_.frames_produced = 0;
}

bool AudioOutput::ProcessMix(Stream* stream, Mixer* mixer, const Stream::Buffer& source_buffer) {
  TRACE_DURATION("audio", "AudioOutput::ProcessMix");
  // Bookkeeping should contain: the rechannel matrix (eventually).

  // Sanity check our parameters.
  FX_DCHECK(mixer);

  // We had better have a valid job, or why are we here?
  FX_DCHECK(cur_mix_job_.buf_frames);
  FX_DCHECK(cur_mix_job_.frames_produced <= cur_mix_job_.buf_frames);

  auto& info = mixer->bookkeeping();

  // If the renderer is currently paused, subject_delta (not just step_size) is zero. This packet
  // may be relevant eventually, but currently it contributes nothing. Tell ForEachSource we are
  // done, but hold the packet for now.
  if (!info.dest_frames_to_frac_source_frames.subject_delta()) {
    return false;
  }

  // Have we produced enough? If so, hold this packet and move to next renderer.
  if (cur_mix_job_.frames_produced >= cur_mix_job_.buf_frames) {
    return false;
  }

  // At this point we know we need to consume some source data, but we don't yet know how much.
  // Here is how many destination frames we still need to produce, for this mix job.
  uint32_t dest_frames_left = cur_mix_job_.buf_frames - cur_mix_job_.frames_produced;
  float* buf = mix_buf_.get() + (cur_mix_job_.frames_produced * mix_format_->channels());

  // Calculate this job's first and last sampling points, in source sub-frames. Use timestamps for
  // the first and last dest frames we need, translated into the source (frac_frame) timeline.
  FractionalFrames<int64_t> frac_source_for_first_mix_job_frame =
      FractionalFrames<int64_t>::FromRaw(info.dest_frames_to_frac_source_frames(
          cur_mix_job_.start_pts_of + cur_mix_job_.frames_produced));

  // This represents (in the frac_frame source timeline) the time of the LAST dest frame we need.
  // Without the "-1", this would be the first destination frame of the NEXT job.
  FractionalFrames<int64_t> frac_source_for_final_mix_job_frame =
      frac_source_for_first_mix_job_frame +
      FractionalFrames<int64_t>::FromRaw(
          info.dest_frames_to_frac_source_frames.rate().Scale(dest_frames_left - 1));

  // If packet has no frames, there's no need to mix it; it may be skipped.
  if (source_buffer.end() == source_buffer.start()) {
    AUD_VLOG(TRACE) << " skipping an empty packet!";
    return true;
  }

  FX_DCHECK(source_buffer.end() >= source_buffer.start() + 1);

  // The above two calculated values characterize our demand. Now reason about our supply. Calculate
  // the actual first and final frame times in the source packet.
  FractionalFrames<int64_t> frac_source_for_first_packet_frame = source_buffer.start();
  FractionalFrames<int64_t> frac_source_for_final_packet_frame = source_buffer.end() - 1;

  // If this source packet's final audio frame occurs before our filter's negative edge, centered at
  // our first sampling point, then this packet is entirely in the past and may be skipped.
  // Returning true means we're done with the packet (it can be completed) and we would like another
  if (frac_source_for_final_packet_frame <
      (frac_source_for_first_mix_job_frame - mixer->neg_filter_width())) {
    FractionalFrames<int64_t> source_frac_frames_late = frac_source_for_first_mix_job_frame -
                                                        mixer->neg_filter_width() -
                                                        frac_source_for_first_packet_frame;
    auto clock_mono_late = zx::nsec(info.clock_mono_to_frac_source_frames.rate().Inverse().Scale(
        source_frac_frames_late.raw_value()));

    stream->ReportUnderflow(frac_source_for_first_packet_frame, frac_source_for_first_mix_job_frame,
                            clock_mono_late);

    return true;
  }

  // If this source packet's first audio frame occurs after our filter's positive edge, centered at
  // our final sampling point, then this packet is entirely in the future and should be held.
  // Returning false (based on requirement that packets must be presented in timestamp-chronological
  // order) means that we have consumed all of the available packet "supply" as we can at this time.
  if (frac_source_for_first_packet_frame >
      (frac_source_for_final_mix_job_frame + mixer->pos_filter_width())) {
    return false;
  }

  // If neither of the above, then evidently this source packet intersects our mixer's filter.
  // Compute the offset into the dest buffer where our first generated sample should land, and the
  // offset into the source packet where we should start sampling.
  int64_t dest_offset_64 = 0;
  FractionalFrames<int64_t> frac_source_offset_64 =
      frac_source_for_first_mix_job_frame - frac_source_for_first_packet_frame;
  FractionalFrames<int64_t> frac_source_pos_edge_first_mix_frame =
      frac_source_for_first_mix_job_frame + mixer->pos_filter_width();

  // If the packet's first frame comes after the filter window's positive edge,
  // then we should skip some frames in the destination buffer before starting to produce data.
  if (frac_source_for_first_packet_frame > frac_source_pos_edge_first_mix_frame) {
    const TimelineRate& dest_to_src = info.dest_frames_to_frac_source_frames.rate();
    // The dest_buffer offset is based on the distance from mix job start to packet start (measured
    // in frac_frames), converted into frames in the destination timeline. As we scale the
    // frac_frame delta into dest frames, we want to "round up" any subframes that are present; any
    // src subframes should push our dest frame up to the next integer. To do this, we subtract a
    // single subframe (guaranteeing that the zero-fraction src case will truncate down), then scale
    // the src delta to dest frames (which effectively truncates any resultant fraction in the
    // computed dest frame), then add an additional 'round-up' frame (to account for initial
    // subtract). Because we entered this IF in the first place, we have at least some fractional
    // src delta, thus dest_offset_64 is guaranteed to become greater than zero.
    FractionalFrames<int64_t> first_source_mix_point =
        frac_source_for_first_packet_frame - frac_source_pos_edge_first_mix_frame;
    dest_offset_64 = dest_to_src.Inverse().Scale(first_source_mix_point.raw_value() - 1) + 1;
    FX_DCHECK(dest_offset_64 > 0);

    frac_source_offset_64 += FractionalFrames<int64_t>::FromRaw(dest_to_src.Scale(dest_offset_64));

    stream->ReportPartialUnderflow(frac_source_offset_64, dest_offset_64);
  }

  FX_DCHECK(dest_offset_64 >= 0);
  FX_DCHECK(dest_offset_64 < static_cast<int64_t>(dest_frames_left));
  auto dest_offset = static_cast<uint32_t>(dest_offset_64);

  FX_DCHECK(frac_source_offset_64 <= std::numeric_limits<int32_t>::max());
  FX_DCHECK(frac_source_offset_64 >= std::numeric_limits<int32_t>::min());
  auto frac_source_offset = FractionalFrames<int32_t>(frac_source_offset_64);

  // Looks like we are ready to go. Mix.
  FX_DCHECK(source_buffer.length() <= FractionalFrames<uint32_t>(FractionalFrames<int32_t>::Max()));

  FX_DCHECK(frac_source_offset + mixer->pos_filter_width() >= FractionalFrames<uint32_t>(0));
  bool consumed_source = false;
  if (frac_source_offset + mixer->pos_filter_width() < source_buffer.length()) {
    // When calling Mix(), we communicate the resampling rate with three parameters. We augment
    // step_size with rate_modulo and denominator arguments that capture the remaining rate
    // component that cannot be expressed by a 19.13 fixed-point step_size. Note: step_size and
    // frac_source_offset use the same format -- they have the same limitations in what they can and
    // cannot communicate.
    //
    // For perfect position accuracy, just as we track incoming/outgoing fractional source offset,
    // we also need to track the ongoing subframe_position_modulo. This is now added to Mix() and
    // maintained across calls, but not initially set to any value other than zero. For now, we are
    // deferring that work, tracking it with MTWN-128.
    //
    // Q: Why did we solve this issue for Rate but not for initial Position?
    // A: We solved this issue for *rate* because its effect accumulates over time, causing clearly
    // measurable distortion that becomes crippling with larger jobs. For *position*, there is no
    // accumulated magnification over time -- in analyzing the distortion that this should cause,
    // mix job size affects the distortion's frequency but not its amplitude. We expect the effects
    // to be below audible thresholds. Until the effects are measurable and attributable to this
    // jitter, we will defer this work.
    auto prev_dest_offset = dest_offset;
    auto prev_frac_source_offset = frac_source_offset;

    // Check whether we are still ramping
    bool ramping = info.gain.IsRamping();
    if (ramping) {
      info.gain.GetScaleArray(
          info.scale_arr.get(),
          std::min(dest_frames_left - dest_offset, Mixer::Bookkeeping::kScaleArrLen),
          cur_mix_job_.reference_clock_to_destination_frame->rate());
    }

    {
      int32_t raw_source_offset = frac_source_offset.raw_value();
      consumed_source = mixer->Mix(buf, dest_frames_left, &dest_offset, source_buffer.payload(),
                                   source_buffer.length().raw_value(), &raw_source_offset,
                                   cur_mix_job_.accumulate);
      frac_source_offset = FractionalFrames<int32_t>::FromRaw(raw_source_offset);
    }
    FX_DCHECK(dest_offset <= dest_frames_left);
    AUD_VLOG_OBJ(SPEW, this) << " consumed from " << std::hex << std::setw(8)
                             << prev_frac_source_offset.raw_value() << " to " << std::setw(8)
                             << frac_source_offset.raw_value() << ", of " << std::setw(8)
                             << source_buffer.length().raw_value();

    // If src is ramping, advance by delta of dest_offset
    if (ramping) {
      info.gain.Advance(dest_offset - prev_dest_offset,
                        cur_mix_job_.reference_clock_to_destination_frame->rate());
    }
  } else {
    // This packet was initially within our mix window. After realigning our sampling point to the
    // nearest dest frame, it is now entirely in the past. This can only occur when down-sampling
    // and is made more likely if the rate conversion ratio is very high. We've already reported
    // a partial underflow when realigning, so just complete the packet and move on to the next.
    consumed_source = true;
  }

  if (consumed_source) {
    FX_DCHECK(frac_source_offset + mixer->pos_filter_width() >= source_buffer.length());
  }

  cur_mix_job_.frames_produced += dest_offset;

  FX_DCHECK(cur_mix_job_.frames_produced <= cur_mix_job_.buf_frames);
  return consumed_source;
}

void AudioOutput::SetupTrim(Mixer* mixer, zx::time now) {
  TRACE_DURATION("audio", "AudioOutput::SetupTrim");
  // Compute the cutoff time used to decide whether to trim packets. ForEachSource has already
  // updated our transformation, no need for us to do so here.
  FX_DCHECK(mixer);
  int64_t local_now_ticks = (now - zx::time(0)).to_nsecs();

  // RateControlBase guarantees that the transformation into the media timeline is never singular.
  // If a forward transformation fails it must be because of overflow, which should be impossible
  // unless user defined a playback rate where the ratio of media-ticks-to-local-ticks is greater
  // than one.
  trim_threshold_ = FractionalFrames<int64_t>::FromRaw(
      mixer->bookkeeping().clock_mono_to_frac_source_frames(local_now_ticks));
}

bool AudioOutput::ProcessTrim(const Stream::Buffer& buffer) {
  TRACE_DURATION("audio", "AudioOutput::ProcessTrim");

  // If the presentation end of this packet is in the future, stop trimming.
  if (buffer.end() > trim_threshold_) {
    return false;
  }

  return true;
}

void AudioOutput::UpdateSourceTrans(const Stream& stream, Mixer::Bookkeeping* bk) {
  TRACE_DURATION("audio", "AudioOutput::UpdateSourceTrans");

  auto func = stream.ReferenceClockToFractionalFrames();
  bk->clock_mono_to_frac_source_frames = func.first;

  // If local->media transformation hasn't changed since last time, we're done.
  if (bk->source_trans_gen_id == func.second) {
    return;
  }

  // Transformation has changed. Update gen; invalidate dest-to-src generation.
  bk->source_trans_gen_id = func.second;
  bk->dest_trans_gen_id = kInvalidGenerationId;
}

void AudioOutput::UpdateDestTrans(const MixJob& job, Mixer::Bookkeeping* bk) {
  TRACE_DURATION("audio", "AudioOutput::UpdateDestTrans");
  // We should only be here if we have a valid mix job. This means a job which supplies a valid
  // transformation from local time to output frames.
  FX_DCHECK(job.reference_clock_to_destination_frame);
  FX_DCHECK(job.reference_clock_to_destination_frame_gen != kInvalidGenerationId);

  // If generations match, don't re-compute -- just use what we have already.
  if (bk->dest_trans_gen_id == job.reference_clock_to_destination_frame_gen) {
    return;
  }

  // Assert we can map from local time to fractional renderer frames.
  FX_DCHECK(bk->source_trans_gen_id != kInvalidGenerationId);

  // Combine the job-supplied local-to-output transformation, with the renderer-supplied mapping of
  // local-to-input-subframe, to produce a transformation which maps from output frames to
  // fractional input frames.
  TimelineFunction& dest = bk->dest_frames_to_frac_source_frames;
  dest = bk->clock_mono_to_frac_source_frames * job.reference_clock_to_destination_frame->Inverse();

  // Finally, compute the step size in subframes. IOW, every time we move forward one output frame,
  // how many input subframes should we consume. Don't bother doing the multiplications if already
  // we know the numerator is zero.
  FX_DCHECK(dest.rate().reference_delta());
  if (!dest.rate().subject_delta()) {
    bk->step_size = 0;
    bk->denominator = 0;  // shouldn't also need to clear rate_mod and pos_mod
  } else {
    int64_t tmp_step_size = dest.rate().Scale(1);

    FX_DCHECK(tmp_step_size >= 0);
    FX_DCHECK(tmp_step_size <= std::numeric_limits<uint32_t>::max());

    bk->step_size = static_cast<uint32_t>(tmp_step_size);
    bk->denominator = bk->SnapshotDenominatorFromDestTrans();
    bk->rate_modulo = dest.rate().subject_delta() - (bk->denominator * bk->step_size);
  }

  // Done, update our dest_trans generation.
  bk->dest_trans_gen_id = job.reference_clock_to_destination_frame_gen;
}

void AudioOutput::Cleanup() {
  AudioDevice::Cleanup();
  mix_timer_.Cancel();
}

}  // namespace media::audio
