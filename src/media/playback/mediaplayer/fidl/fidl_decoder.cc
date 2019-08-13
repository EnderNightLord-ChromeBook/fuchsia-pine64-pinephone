// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/fidl/fidl_decoder.h"

#include <vector>

#include "lib/fidl/cpp/clone.h"
#include "lib/fidl/cpp/optional.h"
#include "src/media/playback/mediaplayer/fidl/fidl_type_conversions.h"
#include "src/media/playback/mediaplayer/graph/formatting.h"
#include "src/media/playback/mediaplayer/graph/types/audio_stream_type.h"

namespace media_player {

// static
void FidlDecoder::Create(const StreamType& stream_type,
                         fuchsia::media::FormatDetails input_format_details,
                         fuchsia::media::StreamProcessorPtr decoder,
                         fit::function<void(std::shared_ptr<Decoder>)> callback) {
  auto fidl_decoder = std::make_shared<FidlDecoder>(stream_type, std::move(input_format_details));
  fidl_decoder->Init(std::move(decoder),
                     [fidl_decoder, callback = std::move(callback)](bool succeeded) {
                       callback(succeeded ? fidl_decoder : nullptr);
                     });
}

FidlDecoder::FidlDecoder(const StreamType& stream_type,
                         fuchsia::media::FormatDetails input_format_details)
    : medium_(stream_type.medium()), input_format_details_(std::move(input_format_details)) {
  FXL_DCHECK(input_format_details_.has_mime_type());

  switch (medium_) {
    case StreamType::Medium::kAudio:
      output_stream_type_ = AudioStreamType::Create(StreamType::kAudioEncodingLpcm, nullptr,
                                                    AudioStreamType::SampleFormat::kNone, 1, 1);
      break;
    case StreamType::Medium::kVideo:
      output_stream_type_ = VideoStreamType::Create(
          StreamType::kVideoEncodingUncompressed, nullptr, VideoStreamType::PixelFormat::kUnknown,
          VideoStreamType::ColorSpace::kUnknown, 0, 0, 0, 0, 1, 1, 0);
      break;
    case StreamType::Medium::kText:
    case StreamType::Medium::kSubpicture:
      FXL_CHECK(false) << "Only audio and video are supported.";
      break;
  }
}

void FidlDecoder::Init(fuchsia::media::StreamProcessorPtr decoder,
                       fit::function<void(bool)> callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(decoder);
  FXL_DCHECK(callback);

  outboard_decoder_ = std::move(decoder);
  init_callback_ = std::move(callback);

  outboard_decoder_.set_error_handler(fit::bind_member(this, &FidlDecoder::OnConnectionFailed));

  outboard_decoder_.events().OnStreamFailed2 = fit::bind_member(this, &FidlDecoder::OnStreamFailed);
  outboard_decoder_.events().OnInputConstraints =
      fit::bind_member(this, &FidlDecoder::OnInputConstraints);
  outboard_decoder_.events().OnOutputConstraints =
      fit::bind_member(this, &FidlDecoder::OnOutputConstraints);
  outboard_decoder_.events().OnOutputFormat = fit::bind_member(this, &FidlDecoder::OnOutputFormat);
  outboard_decoder_.events().OnOutputPacket = fit::bind_member(this, &FidlDecoder::OnOutputPacket);
  outboard_decoder_.events().OnOutputEndOfStream =
      fit::bind_member(this, &FidlDecoder::OnOutputEndOfStream);
  outboard_decoder_.events().OnFreeInputPacket =
      fit::bind_member(this, &FidlDecoder::OnFreeInputPacket);

  outboard_decoder_->EnableOnStreamFailed();
}

FidlDecoder::~FidlDecoder() { FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_); }

const char* FidlDecoder::label() const { return "fidl decoder"; }

void FidlDecoder::Dump(std::ostream& os) const {
  Node::Dump(os);
  // TODO(dalesat): More.
}

void FidlDecoder::ConfigureConnectors() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  MaybeConfigureInput(nullptr);
  MaybeConfigureOutput(nullptr);
}

void FidlDecoder::OnInputConnectionReady(size_t input_index) {
  FXL_DCHECK(input_index == 0);

  if (add_input_buffers_pending_) {
    add_input_buffers_pending_ = false;
    AddInputBuffers();
  }
}

void FidlDecoder::FlushInput(bool hold_frame, size_t input_index, fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(callback);
  FXL_DCHECK(input_format_details_.has_mime_type());

  // This decoder will always receive a FlushOutput shortly after a FlushInput.
  // We call CloseCurrentStream now to let the outboard decoder know we're
  // abandoning this stream. Incrementing stream_lifetime_ordinal_ will cause
  // any stale output packets to be discarded. When FlushOutput is called, we'll
  // sync with the outboard decoder to make sure we're all caught up.
  outboard_decoder_->CloseCurrentStream(stream_lifetime_ordinal_, false, false);
  stream_lifetime_ordinal_ += 2;
  end_of_input_stream_ = false;
  flushing_ = true;

  callback();
}

void FidlDecoder::PutInputPacket(PacketPtr packet, size_t input_index) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(packet);
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(input_buffers_.has_current_set());

  if (flushing_) {
    return;
  }

  if (pts_rate_ == media::TimelineRate()) {
    pts_rate_ = packet->pts_rate();
  } else {
    FXL_DCHECK(pts_rate_ == packet->pts_rate());
  }

  if (packet->size() != 0) {
    // The buffer attached to this packet will be one we created using
    // |input_buffers_|.

    BufferSet& current_set = input_buffers_.current_set();

    FXL_DCHECK(packet->payload_buffer()->id() < current_set.buffer_count())
        << "Buffer ID " << packet->payload_buffer()->id()
        << " is out of range, should be less than " << current_set.buffer_count();
    current_set.AddRefBufferForDecoder(packet->payload_buffer()->id(), packet->payload_buffer());

    fuchsia::media::Packet codec_packet;
    codec_packet.mutable_header()->set_buffer_lifetime_ordinal(current_set.lifetime_ordinal());
    codec_packet.mutable_header()->set_packet_index(packet->payload_buffer()->id());
    codec_packet.set_buffer_index(packet->payload_buffer()->id());
    codec_packet.set_stream_lifetime_ordinal(stream_lifetime_ordinal_);
    codec_packet.set_start_offset(0);
    codec_packet.set_valid_length_bytes(packet->size());
    codec_packet.set_timestamp_ish(static_cast<uint64_t>(packet->pts()));
    codec_packet.set_start_access_unit(packet->keyframe());
    codec_packet.set_known_end_access_unit(false);

    FXL_DCHECK(packet->size() <= current_set.buffer_size());

    outboard_decoder_->QueueInputPacket(std::move(codec_packet));
  }

  if (packet->end_of_stream()) {
    end_of_input_stream_ = true;
    outboard_decoder_->QueueInputEndOfStream(stream_lifetime_ordinal_);
  }
}

void FidlDecoder::OnOutputConnectionReady(size_t output_index) {
  FXL_DCHECK(output_index == 0);

  if (add_output_buffers_pending_) {
    add_output_buffers_pending_ = false;
    AddOutputBuffers();
  }
}

void FidlDecoder::FlushOutput(size_t output_index, fit::closure callback) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(output_index == 0);
  FXL_DCHECK(callback);

  // This decoder will always receive a FlushInput shortly before a FlushOutput.
  // In FlushInput, we've already closed the stream. Now we sync with the
  // output decoder just to make sure we're caught up.
  outboard_decoder_->Sync(std::move(callback));
}

void FidlDecoder::RequestOutputPacket() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  flushing_ = false;

  MaybeRequestInputPacket();
}

std::unique_ptr<StreamType> FidlDecoder::output_stream_type() const {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(output_stream_type_);
  return output_stream_type_->Clone();
}

void FidlDecoder::InitSucceeded() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (init_callback_) {
    auto callback = std::move(init_callback_);
    callback(true);
  }
}

void FidlDecoder::InitFailed() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (init_callback_) {
    auto callback = std::move(init_callback_);
    callback(false);
  }
}

void FidlDecoder::MaybeConfigureInput(fuchsia::media::StreamBufferConstraints* constraints) {
  if (constraints == nullptr) {
    // We have no constraints to apply. Defer the configuration.
    ConfigureInputDeferred();
    return;
  }

  FXL_DCHECK(input_buffers_.has_current_set());

  const bool physically_contiguous_required =
      constraints->has_is_physically_contiguous_required() &&
      constraints->is_physically_contiguous_required();

  FXL_DCHECK(!physically_contiguous_required || constraints->has_very_temp_kludge_bti_handle());

  BufferSet& current_set = input_buffers_.current_set();
  ConfigureInputToUseVmos(
      0, current_set.buffer_count(), current_set.buffer_size(),
      current_set.single_vmo() ? VmoAllocation::kSingleVmo : VmoAllocation::kVmoPerBuffer,
      physically_contiguous_required,
      physically_contiguous_required
          ? std::move(*constraints->mutable_very_temp_kludge_bti_handle())
          : zx::handle(),
      [&current_set](uint64_t size, const PayloadVmos& payload_vmos) {
        // This callback runs on an arbitrary thread.
        return current_set.AllocateBuffer(size, payload_vmos);
      });

  if (InputConnectionReady()) {
    AddInputBuffers();
  } else {
    add_input_buffers_pending_ = true;
  }
}

void FidlDecoder::AddInputBuffers() {
  FXL_DCHECK(InputConnectionReady());

  BufferSet& current_set = input_buffers_.current_set();
  for (uint32_t index = 0; index < current_set.buffer_count(); ++index) {
    auto descriptor = current_set.GetBufferDescriptor(index, false, UseInputVmos());
    outboard_decoder_->AddInputBuffer(std::move(descriptor));
  }
}

void FidlDecoder::MaybeConfigureOutput(fuchsia::media::StreamBufferConstraints* constraints) {
  FXL_DCHECK(constraints == nullptr || constraints->has_per_packet_buffer_bytes_max() &&
                                           constraints->per_packet_buffer_bytes_max() != 0);

  if (constraints == nullptr) {
    // We have no constraints to apply. Defer the configuration.
    ConfigureOutputDeferred();
    return;
  }

  FXL_DCHECK(output_buffers_.has_current_set());
  FXL_DCHECK(output_stream_type_);
  FXL_DCHECK(constraints->has_very_temp_kludge_bti_handle());

  // TODO(dalesat): Do we need to add some buffers for queueing?
  BufferSet& current_set = output_buffers_.current_set();
  output_vmos_physically_contiguous_ = (constraints->has_is_physically_contiguous_required() &&
                                        constraints->is_physically_contiguous_required());
  ConfigureOutputToUseVmos(
      0, current_set.buffer_count(), current_set.buffer_size(),
      current_set.single_vmo() ? VmoAllocation::kSingleVmo : VmoAllocation::kVmoPerBuffer,
      output_vmos_physically_contiguous_,
      std::move(*constraints->mutable_very_temp_kludge_bti_handle()));

  if (OutputConnectionReady()) {
    AddOutputBuffers();
  } else {
    add_output_buffers_pending_ = true;
  }
}

void FidlDecoder::AddOutputBuffers() {
  FXL_DCHECK(OutputConnectionReady());

  // We allocate all the buffers on behalf of the outboard decoder. We give
  // the outboard decoder ownership of these buffers as long as this set is
  // current. The decoder decides what buffers to use for output. When an
  // output packet is produced, the player shares ownership of the buffer until
  // all packets referencing the buffer are recycled. This ownership model
  // reflects the fact that the outboard decoder is free to use output buffers
  // as references and even use the same output buffer for multiple packets as
  // happens with VP9.
  BufferSet& current_set = output_buffers_.current_set();
  current_set.AllocateAllBuffersForDecoder(UseOutputVmos());

  for (uint32_t index = 0; index < current_set.buffer_count(); ++index) {
    auto descriptor = current_set.GetBufferDescriptor(index, true, UseOutputVmos());
    outboard_decoder_->AddOutputBuffer(std::move(descriptor));
  }
}

void FidlDecoder::MaybeRequestInputPacket() {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (!flushing_ && input_buffers_.has_current_set() && !end_of_input_stream_) {
    // |HasFreeBuffer| returns true if there's a free buffer. If there's no
    // free buffer, it will call the callback when there is one.
    if (!input_buffers_.current_set().HasFreeBuffer(
            [this]() { PostTask([this]() { MaybeRequestInputPacket(); }); })) {
      return;
    }

    RequestInputPacket();
  }
}

void FidlDecoder::OnConnectionFailed(zx_status_t error) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  InitFailed();
  // TODO(dalesat): Report failure.
}

void FidlDecoder::OnStreamFailed(uint64_t stream_lifetime_ordinal,
                                 fuchsia::media::StreamError error) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_LOG(ERROR) << "OnStreamFailed: stream_lifetime_ordinal: " << stream_lifetime_ordinal
                 << " error: " << std::hex << static_cast<uint32_t>(error);
  // TODO(dalesat): Report failure.
}

void FidlDecoder::OnInputConstraints(fuchsia::media::StreamBufferConstraints constraints) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);
  FXL_DCHECK(!input_buffers_.has_current_set()) << "OnInputConstraints received more than once.";

  input_buffers_.ApplyConstraints(constraints, true);
  FXL_DCHECK(input_buffers_.has_current_set());
  BufferSet& current_set = input_buffers_.current_set();

  MaybeConfigureInput(&constraints);

  outboard_decoder_->SetInputBufferSettings(fidl::Clone(current_set.settings()));

  InitSucceeded();
}

void FidlDecoder::OnOutputConstraints(fuchsia::media::StreamOutputConstraints constraints) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (constraints.has_buffer_constraints_action_required() &&
      constraints.buffer_constraints_action_required() && !constraints.has_buffer_constraints()) {
    FXL_LOG(ERROR) << "OnOutputConstraints: constraints action required but "
                      "constraints missing";
    InitFailed();
    return;
  }

  if (!constraints.has_buffer_constraints_action_required() ||
      !constraints.buffer_constraints_action_required()) {
    if (init_callback_) {
      FXL_LOG(ERROR) << "OnOutputConstraints: constraints action not required on "
                        "initial constraints.";
      InitFailed();
      return;
    }
  }

  if (output_buffers_.has_current_set()) {
    // All the old output buffers were owned by the outboard decoder. We release
    // that ownership. The buffers will continue to exist until all packets
    // referencing them are destroyed.
    output_buffers_.current_set().ReleaseAllDecoderOwnedBuffers();
  }

  // Use a single VMO for audio, VMO per buffer for video.
  const bool success =
      output_buffers_.ApplyConstraints(constraints.buffer_constraints(),
                                       output_stream_type_->medium() == StreamType::Medium::kAudio);
  if (!success) {
    FXL_LOG(ERROR) << "OnOutputConstraints: Failed to apply constraints.";
    InitFailed();
    return;
  }

  FXL_DCHECK(output_buffers_.has_current_set());
  BufferSet& current_set = output_buffers_.current_set();

  outboard_decoder_->SetOutputBufferSettings(fidl::Clone(current_set.settings()));

  if (constraints.has_buffer_constraints() &&
      (!constraints.buffer_constraints().has_per_packet_buffer_bytes_max() ||
       constraints.buffer_constraints().per_packet_buffer_bytes_max() == 0)) {
    FXL_LOG(ERROR) << "Buffer constraints are missing non-zero per packet "
                      "buffer bytes max";
    InitFailed();
    return;
  }

  // Create the VMOs when we're ready, and add them to the outboard decoder.
  // Mutable so we can move the vmo handle out.
  MaybeConfigureOutput(constraints.mutable_buffer_constraints());
};  // namespace media_player

void FidlDecoder::OnOutputFormat(fuchsia::media::StreamOutputFormat format) {
  if (!format.has_format_details()) {
    FXL_LOG(ERROR) << "Config has no format details.";
    InitFailed();
    return;
  }

  auto stream_type = fidl::To<std::unique_ptr<StreamType>>(format.format_details());
  if (!stream_type) {
    FXL_LOG(ERROR) << "Can't comprehend format details.";
    InitFailed();
    return;
  }

  if (!format.format_details().has_format_details_version_ordinal()) {
    FXL_LOG(ERROR) << "Format details do not have version ordinal.";
    InitFailed();
    return;
  }

  if (output_stream_type_) {
    if (output_format_details_version_ordinal_ !=
        format.format_details().format_details_version_ordinal()) {
      HandlePossibleOutputStreamTypeChange(*output_stream_type_, *stream_type);
    }
  }

  output_format_details_version_ordinal_ = format.format_details().format_details_version_ordinal();

  output_stream_type_ = std::move(stream_type);
  have_real_output_stream_type_ = true;
}

void FidlDecoder::OnOutputPacket(fuchsia::media::Packet packet, bool error_detected_before,
                                 bool error_detected_during) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (!packet.has_header() || !packet.header().has_buffer_lifetime_ordinal() ||
      !packet.header().has_packet_index() || !packet.has_buffer_index() ||
      !packet.has_valid_length_bytes() || !packet.has_stream_lifetime_ordinal()) {
    FXL_LOG(ERROR) << "Packet not fully initialized.";
    return;
  }

  uint64_t buffer_lifetime_ordinal = packet.header().buffer_lifetime_ordinal();
  uint32_t packet_index = packet.header().packet_index();
  uint32_t buffer_index = packet.buffer_index();
  FXL_DCHECK(buffer_index != 0x80000000);

  if (error_detected_before) {
    FXL_LOG(WARNING) << "OnOutputPacket: error_detected_before";
  }

  if (error_detected_during) {
    FXL_LOG(WARNING) << "OnOutputPacket: error_detected_during";
  }

  if (!output_buffers_.has_current_set()) {
    FXL_LOG(FATAL) << "OnOutputPacket event without prior OnOutputConstraints event";
    // TODO(dalesat): Report error rather than crashing.
  }

  if (!have_real_output_stream_type_) {
    FXL_LOG(FATAL) << "OnOutputPacket event without prior OnOutputFormat event";
    // TODO(dalesat): Report error rather than crashing.
  }

  BufferSet& current_set = output_buffers_.current_set();

  if (packet.header().buffer_lifetime_ordinal() != current_set.lifetime_ordinal()) {
    // Refers to an obsolete buffer. We've already assumed the outboard
    // decoder gave up this buffer, so there's no need to free it. Also, this
    // shouldn't happen, and there's no evidence that it does.
    FXL_LOG(FATAL) << "OnOutputPacket delivered tacket with obsolete "
                      "buffer_lifetime_ordinal.";
    return;
  }

  if (packet.stream_lifetime_ordinal() != stream_lifetime_ordinal_) {
    // Refers to an obsolete stream. We'll just recycle the packet back to the
    // output decoder.
    outboard_decoder_->RecycleOutputPacket(std::move(*packet.mutable_header()));
    return;
  }

  // All the output buffers in the current set are always owned by the outboard
  // decoder. Get another reference to the |PayloadBuffer| for the specified
  // buffer.
  FXL_DCHECK(buffer_lifetime_ordinal == current_set.lifetime_ordinal());
  fbl::RefPtr<PayloadBuffer> payload_buffer = current_set.GetDecoderOwnedBuffer(buffer_index);

  // TODO(dalesat): Tolerate !has_timestamp_ish somehow.
  if (!packet.has_timestamp_ish()) {
    FXL_LOG(ERROR) << "We demand has_timestamp_ish for now (TODO)";
    return;
  }

  next_pts_ = static_cast<int64_t>(packet.timestamp_ish());

  auto output_packet = Packet::Create(next_pts_, pts_rate_, true, false,
                                      packet.valid_length_bytes(), std::move(payload_buffer));

  if (revised_output_stream_type_) {
    output_packet->SetRevisedStreamType(std::move(revised_output_stream_type_));
  }

  output_packet->AfterRecycling(
      [this, shared_this = shared_from_this(), packet_index](Packet* packet) {
        PostTask([this, shared_this, packet_index,
                  buffer_lifetime_ordinal = packet->payload_buffer()->buffer_config()]() {
          FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

          // |outboard_decoder_| is always set after |Init| is called, so we
          // can rely on it here.
          FXL_DCHECK(outboard_decoder_);
          fuchsia::media::PacketHeader header;
          header.set_buffer_lifetime_ordinal(buffer_lifetime_ordinal);
          header.set_packet_index(packet_index);
          outboard_decoder_->RecycleOutputPacket(std::move(header));
        });
      });

  PutOutputPacket(std::move(output_packet));
}

void FidlDecoder::OnOutputEndOfStream(uint64_t stream_lifetime_ordinal,
                                      bool error_detected_before) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (error_detected_before) {
    FXL_LOG(WARNING) << "OnOutputEndOfStream: error_detected_before";
  }

  PutOutputPacket(Packet::CreateEndOfStream(next_pts_, pts_rate_));
}

void FidlDecoder::OnFreeInputPacket(fuchsia::media::PacketHeader packet_header) {
  FXL_DCHECK_CREATION_THREAD_IS_CURRENT(thread_checker_);

  if (!packet_header.has_buffer_lifetime_ordinal() || !packet_header.has_packet_index()) {
    FXL_LOG(ERROR) << "Freed packet missing ordinal or index.";
    return;
  }

  input_buffers_.ReleaseBufferForDecoder(packet_header.buffer_lifetime_ordinal(),
                                         packet_header.packet_index());
}

void FidlDecoder::HandlePossibleOutputStreamTypeChange(const StreamType& old_type,
                                                       const StreamType& new_type) {
  // TODO(dalesat): Actually compare the types.
  revised_output_stream_type_ = new_type.Clone();
}

}  // namespace media_player
