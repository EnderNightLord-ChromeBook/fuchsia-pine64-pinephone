// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/decode/software_decoder.h"

#include <lib/async/default.h>

#include "src/lib/fxl/logging.h"
#include "src/media/playback/mediaplayer/graph/formatting.h"

namespace media_player {

SoftwareDecoder::SoftwareDecoder()
    : main_thread_dispatcher_(async_get_default_dispatcher()),
      worker_loop_(&kAsyncLoopConfigNoAttachToThread) {
  output_state_ = OutputState::kIdle;
  worker_loop_.StartThread();
}

SoftwareDecoder::~SoftwareDecoder() { FXL_DCHECK(is_main_thread()); }

void SoftwareDecoder::FlushInput(bool hold_frame, size_t input_index, fit::closure callback) {
  FXL_DCHECK(is_main_thread());
  FXL_DCHECK(input_index == 0);
  FXL_DCHECK(callback);

  flushing_ = true;
  input_packet_.reset();
  end_of_input_stream_ = false;

  // If we were waiting for an input packet, we aren't anymore.
  if (output_state_ == OutputState::kWaitingForInput) {
    output_state_ = OutputState::kIdle;
  }

  callback();
}

void SoftwareDecoder::FlushOutput(size_t output_index, fit::closure callback) {
  FXL_DCHECK(is_main_thread());
  FXL_DCHECK(output_index == 0);
  FXL_DCHECK(callback);

  flushing_ = true;
  end_of_output_stream_ = false;

  if (output_state_ == OutputState::kWaitingForWorker ||
      output_state_ == OutputState::kWorkerNotDone) {
    // The worker is busy processing an input packet. Wait until it's done
    // before calling the callback.
    flush_callback_ = std::move(callback);
    return;
  }

  PostTaskToWorkerThread([this, callback = std::move(callback)]() mutable {
    Flush();
    PostTaskToMainThread(std::move(callback));
  });
}

void SoftwareDecoder::PutInputPacket(PacketPtr packet, size_t input_index) {
  FXL_DCHECK(is_main_thread());
  FXL_DCHECK(packet);
  FXL_DCHECK(input_index == 0);

  FXL_DCHECK(!input_packet_);
  FXL_DCHECK(!end_of_input_stream_);

  if (flushing_) {
    // We're flushing. Discard the packet.
    return;
  }

  if (packet->end_of_stream()) {
    end_of_input_stream_ = true;
  }

  if (output_state_ != OutputState::kWaitingForInput) {
    // We weren't waiting for this packet, so save it for later.
    input_packet_ = std::move(packet);
    return;
  }

  output_state_ = OutputState::kWaitingForWorker;

  PostTaskToWorkerThread([this, packet] { HandleInputPacketOnWorker(packet); });

  if (!end_of_input_stream_) {
    // Request another packet to keep |input_packet_| full.
    RequestInputPacket();
  }
}

void SoftwareDecoder::RequestOutputPacket() {
  FXL_DCHECK(is_main_thread());
  FXL_DCHECK(!end_of_output_stream_);

  if (flushing_) {
    FXL_DCHECK(!end_of_input_stream_);
    FXL_DCHECK(!input_packet_);
    flushing_ = false;
    RequestInputPacket();
  }

  if (output_state_ == OutputState::kWaitingForWorker) {
    return;
  }

  if (output_state_ == OutputState::kWorkerNotDone) {
    // The worker is processing an input packet and has satisfied a previous
    // request for an output packet. Indicate that we have a new unsatisfied
    // request.
    output_state_ = OutputState::kWaitingForWorker;
    return;
  }

  if (!input_packet_) {
    FXL_DCHECK(!end_of_input_stream_);

    // We're expecting an input packet. Wait for it.
    output_state_ = OutputState::kWaitingForInput;
    return;
  }

  output_state_ = OutputState::kWaitingForWorker;

  PostTaskToWorkerThread(
      [this, packet = std::move(input_packet_)] { HandleInputPacketOnWorker(std::move(packet)); });

  if (!end_of_input_stream_) {
    // Request the next packet, so it will be ready when we need it.
    RequestInputPacket();
  }
}

void SoftwareDecoder::HandleInputPacketOnWorker(PacketPtr input) {
  FXL_DCHECK(is_worker_thread());
  FXL_DCHECK(input);

  bool done = false;
  bool new_input = true;

  int64_t start_time = zx::clock::get_monotonic().get();

  // We depend on |TransformPacket| behaving properly here. Specifically, it
  // should return true in just a few iterations. It will normally produce an
  // output packet and/or return true. The only exception is when the output
  // allocator is exhausted.
  while (!done) {
    PacketPtr output;
    done = TransformPacket(input, new_input, &output);

    new_input = false;

    if (output) {
      PostTaskToMainThread([this, output]() { HandleOutputPacket(output); });
    }
  }

  {
    std::lock_guard<std::mutex> locker(decode_duration_mutex_);
    decode_duration_.AddSample(zx::clock::get_monotonic().get() - start_time);
  }

  PostTaskToMainThread([this]() { WorkerDoneWithInputPacket(); });
}

void SoftwareDecoder::HandleOutputPacket(PacketPtr packet) {
  FXL_DCHECK(is_main_thread());
  FXL_DCHECK(!end_of_output_stream_);

  if (flushing_) {
    // We're flushing. Discard the packet.
    return;
  }

  switch (output_state_) {
    case OutputState::kIdle:
      FXL_DCHECK(false) << "HandleOutputPacket called when idle.";
      break;
    case OutputState::kWaitingForInput:
      FXL_DCHECK(false) << "HandleOutputPacket called waiting for input.";
      break;
    case OutputState::kWaitingForWorker:
      // We got the requested packet. Indicate we've satisfied the request for
      // an output packet, but the worker hasn't finished with the input packet.
      output_state_ = OutputState::kWorkerNotDone;
      break;
    case OutputState::kWorkerNotDone:
      // We got an additional output packet.
      break;
  }

  end_of_output_stream_ = packet->end_of_stream();
  PutOutputPacket(std::move(packet));
}

void SoftwareDecoder::WorkerDoneWithInputPacket() {
  FXL_DCHECK(is_main_thread());

  switch (output_state_) {
    case OutputState::kIdle:
      FXL_DCHECK(false) << "WorkerDoneWithInputPacket called in idle state.";
      break;

    case OutputState::kWaitingForInput:
      FXL_DCHECK(false) << "WorkerDoneWithInputPacket called waiting for input.";
      break;

    case OutputState::kWaitingForWorker:
      // We didn't get the requested output packet. Behave as though we just
      // got a new request.
      output_state_ = OutputState::kIdle;
      if (!flushing_) {
        RequestOutputPacket();
      }

      break;

    case OutputState::kWorkerNotDone:
      // We got the requested output packet. Done for now.
      output_state_ = OutputState::kIdle;
      break;
  }

  if (flush_callback_) {
    PostTaskToWorkerThread([this, callback = std::move(flush_callback_)]() mutable {
      Flush();
      PostTaskToMainThread(std::move(callback));
    });
  }
}

void SoftwareDecoder::Dump(std::ostream& os) const {
  FXL_DCHECK(is_main_thread());

  os << label() << fostr::Indent;
  Node::Dump(os);
  os << fostr::NewLine << "output stream type:" << output_stream_type();
  os << fostr::NewLine << "state:             ";

  switch (output_state_) {
    case OutputState::kIdle:
      os << "idle";
      break;
    case OutputState::kWaitingForInput:
      os << "waiting for input";
      break;
    case OutputState::kWaitingForWorker:
      os << "waiting for worker";
      break;
    case OutputState::kWorkerNotDone:
      os << "worker not done";
      break;
  }

  os << fostr::NewLine << "flushing:          " << flushing_;
  os << fostr::NewLine << "end of input:      " << end_of_input_stream_;
  os << fostr::NewLine << "end of output:     " << end_of_output_stream_;

  if (input_packet_) {
    os << fostr::NewLine << "input packet:      " << input_packet_;
  }

  {
    std::lock_guard<std::mutex> locker(decode_duration_mutex_);
    if (decode_duration_.count() != 0) {
      os << fostr::NewLine << "decodes:           " << decode_duration_.count();
      os << fostr::NewLine << "decode durations:";
      os << fostr::Indent;
      os << fostr::NewLine << "minimum        " << AsNs(decode_duration_.min());
      os << fostr::NewLine << "average        " << AsNs(decode_duration_.average());
      os << fostr::NewLine << "maximum        " << AsNs(decode_duration_.max());
      os << fostr::Outdent;
    }
  }

  os << fostr::Outdent;
}

}  // namespace media_player
