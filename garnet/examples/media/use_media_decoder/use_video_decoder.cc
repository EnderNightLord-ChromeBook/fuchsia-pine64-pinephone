// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "use_video_decoder.h"

#include "in_stream_peeker.h"

#include <garnet/lib/media/raw_video_writer/raw_video_writer.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/fit/defer.h>
#include <lib/media/codec_impl/fourcc.h>
#include <lib/media/test/codec_client.h>
#include <lib/media/test/frame_sink.h>
#include <lib/media/test/one_shot_event.h>
#include <src/lib/fxl/arraysize.h>
#include <src/lib/fxl/logging.h>
#include <stdint.h>
#include <string.h>

#include <thread>

#include "lib/zx/time.h"
#include "util.h"

namespace {

constexpr zx::duration kReadDeadlineDuration = zx::sec(30);

// This example only has one stream_lifetime_ordinal which is 1.
//
// TODO(dustingreen): actually re-use the Codec instance for at least one more
// stream, even if it's just to decode the same data again.
constexpr uint64_t kStreamLifetimeOrdinal = 1;

// Scenic ImagePipe doesn't allow image_id 0, so offset by this much.
constexpr uint32_t kFirstValidImageId = 1;

constexpr uint8_t kLongStartCodeArray[] = {0x00, 0x00, 0x00, 0x01};
constexpr uint8_t kShortStartCodeArray[] = {0x00, 0x00, 0x01};

// If readable_bytes is 0, that's considered a "start code", to allow the caller
// to terminate a NAL the same way regardless of whether another start code is
// found or the end of the buffer is found.
//
// ptr has readable_bytes of data - the function only evaluates whether there is
// a start code at the beginning of the data at ptr.
//
// readable_bytes - the caller indicates how many bytes are readable starting at
// ptr.
//
// *start_code_size_bytes will have length of the start code in bytes when the
// function returns true - unchanged otherwise.  Normally this would be 3 or 4,
// but a 0 is possible if readable_bytes is 0.
bool is_start_code(uint8_t* ptr, size_t readable_bytes, size_t* start_code_size_bytes_out) {
  if (readable_bytes == 0) {
    *start_code_size_bytes_out = 0;
    return true;
  }
  if (readable_bytes >= 4) {
    if (!memcmp(ptr, kLongStartCodeArray, sizeof(kLongStartCodeArray))) {
      *start_code_size_bytes_out = 4;
      return true;
    }
  }
  if (readable_bytes >= 3) {
    if (!memcmp(ptr, kShortStartCodeArray, sizeof(kShortStartCodeArray))) {
      *start_code_size_bytes_out = 3;
      return true;
    }
  }
  return false;
}

// Test-only.  Not for production use.  Caller must ensure there are at least 5
// bytes at nal_unit.
uint8_t GetNalUnitType(const uint8_t* nal_unit) {
  // Also works with 4-byte startcodes.
  static const uint8_t start_code[3] = {0, 0, 1};
  uint8_t* next_start = static_cast<uint8_t*>(memmem(nal_unit, 5, start_code, sizeof(start_code))) +
                        sizeof(start_code);
  return *next_start & 0xf;
}

struct __attribute__((__packed__)) IvfHeader {
  uint32_t signature;
  uint16_t version;
  uint16_t header_length;
  uint32_t fourcc;
  uint16_t width;
  uint16_t height;
  uint32_t frame_rate;
  uint32_t time_scale;
  uint32_t frame_count;
  uint32_t unused;
};

struct __attribute__((__packed__)) IvfFrameHeader {
  uint32_t size_bytes;
  uint64_t presentation_timestamp;
};

enum class Format {
  kH264,
  kVp9,
};

}  // namespace

// Payload data for bear.h264 is 00 00 00 01 start code before each NAL, with
// SPS / PPS NALs and also frame NALs.  We deliver to Codec NAL-by-NAL without
// the start code.
//
// Since the .h264 file has SPS + PPS NALs in addition to frame NALs, we don't
// use oob_bytes for this stream.
//
// TODO(dustingreen): Determine for .mp4 or similar which don't have SPS / PPS
// in band whether .mp4 provides ongoing OOB data, or just at the start, and
// document in codec.fidl how that's to be handled.
void QueueH264Frames(CodecClient* codec_client, InStreamPeeker* in_stream) {
  // We assign fake PTS values starting at 0 partly to verify that 0 is
  // treated as a valid PTS.
  uint64_t input_frame_pts_counter = 0;
  // Raw .h264 has start code 00 00 01 or 00 00 00 01 before each NAL, and
  // the start codes don't alias in the middle of NALs, so we just scan
  // for NALs and send them in to the decoder.
  auto queue_access_unit = [&codec_client, &input_frame_pts_counter](uint8_t* bytes,
                                                                     size_t byte_count) {
    size_t bytes_so_far = 0;
    // printf("queuing offset: %ld byte_count: %zu\n", bytes -
    // input_bytes.get(), byte_count);
    while (bytes_so_far != byte_count) {
      std::unique_ptr<fuchsia::media::Packet> packet = codec_client->BlockingGetFreeInputPacket();

      if (!packet->has_header()) {
        Exit("broken server sent packet without header");
      }

      if (!packet->header().has_packet_index()) {
        Exit("broken server sent packet without packet index");
      }

      // For input we do buffer_index == packet_index.
      const CodecBuffer& buffer =
          codec_client->GetInputBufferByIndex(packet->header().packet_index());
      size_t bytes_to_copy = std::min(byte_count - bytes_so_far, buffer.size_bytes());
      packet->set_stream_lifetime_ordinal(kStreamLifetimeOrdinal);
      packet->set_start_offset(0);
      packet->set_valid_length_bytes(bytes_to_copy);

      if (bytes_so_far == 0) {
        uint8_t nal_unit_type = GetNalUnitType(bytes);
        if (nal_unit_type == 1 || nal_unit_type == 5) {
          packet->set_timestamp_ish(input_frame_pts_counter++);
        }
      }

      packet->set_start_access_unit(bytes_so_far == 0);
      packet->set_known_end_access_unit(bytes_so_far + bytes_to_copy == byte_count);
      memcpy(buffer.base(), bytes + bytes_so_far, bytes_to_copy);
      codec_client->QueueInputPacket(std::move(packet));
      bytes_so_far += bytes_to_copy;
    }
  };

  // Let caller-provided in_stream drive how far ahead we peek.  If it's not far
  // enough to find a start code or the EOS, then we'll error out.
  uint32_t max_peek_bytes = in_stream->max_peek_bytes();
  while (true) {
    // Until clang-tidy correctly interprets Exit(), this "= 0" satisfies it.
    size_t start_code_size_bytes = 0;
    uint32_t actual_peek_bytes;
    uint8_t* peek;
    zx_status_t status = in_stream->PeekBytes(
        max_peek_bytes, &actual_peek_bytes, &peek,
        zx::deadline_after(kReadDeadlineDuration));
    ZX_ASSERT(status == ZX_OK);
    if (actual_peek_bytes == 0) {
      // Out of input.  Not an error.  No more input AUs.
      ZX_DEBUG_ASSERT(in_stream->eos_position_known() && in_stream->cursor_position() == in_stream->eos_position());
      break;
    }
    if (!is_start_code(&peek[0], actual_peek_bytes, &start_code_size_bytes)) {
      if (in_stream->cursor_position() == 0) {
        Exit(
            "Didn't find a start code at the start of the file, and this "
            "example doesn't scan forward (for now).");
      } else {
        Exit(
            "Fell out of sync somehow - previous NAL offset + previous "
            "NAL length not a start code.");
      }
    }
    if (in_stream->eos_position_known() &&
        in_stream->cursor_position() + start_code_size_bytes == in_stream->eos_position()) {
      Exit("Start code at end of file unexpected");
    }
    size_t nal_start_offset = start_code_size_bytes;
    // Scan for end of NAL.  The end of NAL can be because we're out of peeked
    // data, or because we hit another start code.
    size_t find_end_iter = nal_start_offset;
    size_t ignore_start_code_size_bytes;
    while (find_end_iter <= actual_peek_bytes &&
           !is_start_code(&peek[find_end_iter],
                          actual_peek_bytes - find_end_iter,
                          &ignore_start_code_size_bytes)) {
      find_end_iter++;
    }
    ZX_DEBUG_ASSERT(find_end_iter <= actual_peek_bytes);
    if (find_end_iter == nal_start_offset) {
      Exit("Two adjacent start codes unexpected.");
    }
    ZX_DEBUG_ASSERT(find_end_iter > nal_start_offset);
    size_t nal_length = find_end_iter - nal_start_offset;
    queue_access_unit(&peek[0], start_code_size_bytes + nal_length);

    // start code + NAL payload
    in_stream->TossPeekedBytes(start_code_size_bytes + nal_length);
  }

  // Send through QueueInputEndOfStream().
  codec_client->QueueInputEndOfStream(kStreamLifetimeOrdinal);
  // We flush and close to run the handling code server-side.  However, we don't
  // yet verify that this successfully achieves what it says.
  codec_client->FlushEndOfStreamAndCloseStream(kStreamLifetimeOrdinal);
  // input thread done
}

void QueueVp9Frames(CodecClient* codec_client, InStreamPeeker* in_stream) {
  uint64_t input_frame_pts_counter = 0;
  auto queue_access_unit = [&codec_client, in_stream, &input_frame_pts_counter](
      size_t byte_count) {
    std::unique_ptr<fuchsia::media::Packet> packet =
        codec_client->BlockingGetFreeInputPacket();
    ZX_ASSERT(packet->has_header());
    ZX_ASSERT(packet->header().has_packet_index());
    const CodecBuffer& buffer =
        codec_client->GetInputBufferByIndex(packet->buffer_index());
    // VP9 decoder doesn't yet support splitting access units into multiple
    // packets.
    ZX_ASSERT(byte_count <= buffer.size_bytes());
    packet->set_stream_lifetime_ordinal(kStreamLifetimeOrdinal);
    packet->set_start_offset(0);
    packet->set_valid_length_bytes(byte_count);

    // We don't use frame_header->presentation_timestamp, because we want to
    // send through frame index in timestamp_ish field instead, for consistency
    // with .h264 files which don't have timestamps in them, and so tests can
    // assume frame index as timestamp_ish on output.
    packet->set_timestamp_ish(input_frame_pts_counter++);

    packet->set_start_access_unit(true);
    packet->set_known_end_access_unit(true);
    uint32_t actual_bytes_read;
    zx_status_t status = in_stream->ReadBytesComplete(
        byte_count,
        &actual_bytes_read,
        buffer.base(),
        zx::deadline_after(kReadDeadlineDuration));
    ZX_ASSERT(status == ZX_OK);
    if (actual_bytes_read < byte_count) {
      Exit("Frame truncated.");
    }
    ZX_DEBUG_ASSERT(actual_bytes_read == byte_count);
    codec_client->QueueInputPacket(std::move(packet));
  };
  IvfHeader header;
  uint32_t actual_bytes_read;
  zx_status_t status = in_stream->ReadBytesComplete(
      sizeof(header),
      &actual_bytes_read,
      reinterpret_cast<uint8_t*>(&header),
      zx::deadline_after(kReadDeadlineDuration));
  // This could fail if a remote-source stream breaks.
  ZX_ASSERT(status == ZX_OK);
  // This could fail if the input is too short.
  ZX_ASSERT(actual_bytes_read == sizeof(header));
  size_t remaining_header_length =
      header.header_length - sizeof(header);
  // We're not interested in any remaining portion of the header, but we should
  // skip the rest of the header, if any.
  if (remaining_header_length) {
    uint8_t toss_buffer[1024];
    while (remaining_header_length != 0) {
      uint32_t bytes_to_read = std::min(sizeof(toss_buffer), remaining_header_length);
      uint32_t actual_bytes_read;
      status = in_stream->ReadBytesComplete(
        bytes_to_read,
        &actual_bytes_read,
        &toss_buffer[0],
        zx::deadline_after(kReadDeadlineDuration));
      ZX_ASSERT(status == ZX_OK);
      ZX_ASSERT(actual_bytes_read == bytes_to_read);
      remaining_header_length -= actual_bytes_read;
    }
  }
  ZX_DEBUG_ASSERT(!remaining_header_length);
  while (true) {
    IvfFrameHeader frame_header;
    status = in_stream->ReadBytesComplete(
        sizeof(frame_header),
        &actual_bytes_read,
        reinterpret_cast<uint8_t*>(&frame_header),
        zx::deadline_after(kReadDeadlineDuration));
    ZX_ASSERT(status == ZX_OK);
    if (actual_bytes_read == 0) {
      // No more frames.  That's fine.
      break;
    }
    if (actual_bytes_read < sizeof(frame_header)) {
      Exit("Frame header truncated.");
    }
    ZX_DEBUG_ASSERT(actual_bytes_read == sizeof(frame_header));
    queue_access_unit(frame_header.size_bytes);
  }

  // Send through QueueInputEndOfStream().
  codec_client->QueueInputEndOfStream(kStreamLifetimeOrdinal);
  // We flush and close to run the handling code server-side.  However, we don't
  // yet verify that this successfully achieves what it says.
  codec_client->FlushEndOfStreamAndCloseStream(kStreamLifetimeOrdinal);
  // input thread done
}

static void use_video_decoder(
    async::Loop* fidl_loop,
    thrd_t fidl_thread,
    fuchsia::mediacodec::CodecFactoryPtr codec_factory,
    fidl::InterfaceHandle<fuchsia::sysmem::Allocator> sysmem,
    InStreamPeeker* in_stream, Format format,
    uint64_t min_output_buffer_size,
    FrameSink* frame_sink,
    EmitFrame emit_frame) {
  VLOGF("use_video_decoder()\n");

  VLOGF("before CodecClient::CodecClient()...\n");
  CodecClient codec_client(fidl_loop, fidl_thread, std::move(sysmem));
  codec_client.SetMinOutputBufferSize(min_output_buffer_size);

  const char* mime_type;
  switch (format) {
    case Format::kH264:
      mime_type = "video/h264";
      break;

    case Format::kVp9:
      mime_type = "video/vp9";
      break;
  }

  async::PostTask(
      fidl_loop->dispatcher(),
      [&codec_factory, codec_client_request = codec_client.GetTheRequestOnce(),
       mime_type]() mutable {
        VLOGF("before codec_factory->CreateDecoder() (async)\n");
        fuchsia::media::FormatDetails input_details;
        input_details.set_format_details_version_ordinal(0);
        input_details.set_mime_type(mime_type);
        fuchsia::mediacodec::CreateDecoder_Params params;
        params.set_input_details(std::move(input_details));
        // This is required for timestamp_ish values to transit the
        // Codec.
        params.set_promise_separate_access_units_on_input(true);
        codec_factory->CreateDecoder(std::move(params),
                                     std::move(codec_client_request));
      });

  VLOGF("before codec_client.Start()...\n");
  // This does a Sync(), so after this we can drop the CodecFactory without it
  // potentially cancelling our Codec create.
  codec_client.Start();

  // We don't need the CodecFactory any more, and at this point any Codec
  // creation errors have had a chance to arrive via the
  // codec_factory.set_error_handler() lambda.
  //
  // Unbind() is only safe to call on the interfaces's dispatcher thread.  We
  // also want to block the current thread until this is done, to avoid
  // codec_factory potentially disappearing before this posted work finishes.
  OneShotEvent unbind_done_event;
  async::PostTask(
      fidl_loop->dispatcher(),
      [&codec_factory, &unbind_done_event] {
        codec_factory.Unbind();
        unbind_done_event.Signal();
        // codec_factory and unbind_done_event are potentially gone by this
        // point.
      });
  unbind_done_event.Wait();

  VLOGF("before starting in_thread...\n");
  std::unique_ptr<std::thread> in_thread = std::make_unique<std::thread>(
      [&codec_client, in_stream, format]() {
        switch (format) {
          case Format::kH264:
            QueueH264Frames(&codec_client, in_stream);
            break;

          case Format::kVp9:
            QueueVp9Frames(&codec_client, in_stream);
            break;
        }
      });

  // Separate thread to process the output.
  //
  // codec_client outlives the thread (and for separate reasons below, all the
  // frame_sink activity started by out_thread).
  std::unique_ptr<std::thread> out_thread = std::make_unique<
      std::thread>([fidl_loop, &codec_client, frame_sink, &emit_frame]() {
    // We allow the server to send multiple output constraint updates if it
    // wants; see implementation of BlockingGetEmittedOutput() which will hide
    // multiple constraint updates before the first packet from this code.  In
    // contrast we assert if the server sends multiple format updates with no
    // packets in between since that's not compliant with the protocol rules.
    std::shared_ptr<const fuchsia::media::StreamOutputFormat>
        prev_stream_format;
    const fuchsia::media::VideoUncompressedFormat* raw = nullptr;
    while (true) {
      std::unique_ptr<CodecOutput> output = codec_client.BlockingGetEmittedOutput();
      if (output->stream_lifetime_ordinal() != kStreamLifetimeOrdinal) {
        Exit(
            "server emitted a stream_lifetime_ordinal that client didn't set "
            "on any input");
      }
      if (output->end_of_stream()) {
        VLOGF("output end_of_stream() - done with output\n");
        // Just "break;" would be more fragile under code modification.
        goto end_of_output;
      }

      const fuchsia::media::Packet& packet = output->packet();

      if (!packet.has_header()) {
        // The server should not generate any empty packets.
        Exit("broken server sent packet without header");
      }

      // cleanup can run on any thread, and codec_client.RecycleOutputPacket()
      // is ok with that.  In addition, cleanup can run after codec_client is
      // gone, since we don't block return from use_video_decoder() on Scenic
      // actually freeing up all previously-queued frames.
      auto cleanup =
          fit::defer([&codec_client, packet_header = fidl::Clone(packet.header())]() mutable {
            // Using an auto call for this helps avoid losing track of the
            // output_buffer.
            codec_client.RecycleOutputPacket(std::move(packet_header));
          });
      std::shared_ptr<const fuchsia::media::StreamOutputFormat> format = output->format();

      if (!packet.has_buffer_index()) {
        // The server should not generate any empty packets.
        Exit("broken server sent packet without buffer index");
      }

      // This will remain live long enough because this thread is the only
      // thread that re-allocates output buffers.
      const CodecBuffer& buffer =
          codec_client.GetOutputBufferByIndex(packet.buffer_index());

      ZX_ASSERT(
          !prev_stream_format ||
          (prev_stream_format->has_format_details() &&
           prev_stream_format->format_details().format_details_version_ordinal()));
      if (!format->has_format_details()) {
        Exit("!format->has_format_details()");
      }
      if (!format->format_details().has_format_details_version_ordinal()) {
        Exit("!format->format_details().has_format_details_version_ordinal()");
      }

      if (!packet.has_valid_length_bytes() || packet.valid_length_bytes() == 0) {
        // The server should not generate any empty packets.
        Exit("broken server sent empty packet");
      }

      if (!packet.has_start_offset()) {
        // The server should not generate any empty packets.
        Exit("broken server sent packet without start offset");
      }

      // We have a non-empty packet of the stream.

      if (!prev_stream_format || prev_stream_format.get() != format.get()) {
        // Every output has a format.  This happens exactly once.
        prev_stream_format = format;

        ZX_ASSERT(format->format_details().has_domain());

        if (!format->has_format_details()) {
          Exit("!format_details");
        }

        const fuchsia::media::FormatDetails& format_details = format->format_details();
        if (!format_details.has_domain()) {
          Exit("!format.domain");
        }

        if (!format_details.domain().is_video()) {
          Exit("!format.domain.is_video()");
        }
        const fuchsia::media::VideoFormat& video_format = format_details.domain().video();
        if (!video_format.is_uncompressed()) {
          Exit("!video.is_uncompressed()");
        }

        raw = &video_format.uncompressed();
        switch (raw->fourcc) {
          case make_fourcc('N', 'V', '1', '2'): {
            size_t y_size = raw->primary_height_pixels * raw->primary_line_stride_bytes;
            if (raw->secondary_start_offset < y_size) {
              Exit("raw.secondary_start_offset < y_size");
            }
            // NV12 requires UV be same line stride as Y.
            size_t total_size = raw->secondary_start_offset +
                                raw->primary_height_pixels / 2 * raw->primary_line_stride_bytes;
            if (packet.valid_length_bytes() < total_size) {
              Exit("packet.valid_length_bytes < total_size");
            }
            break;
          }
          case make_fourcc('Y', 'V', '1', '2'): {
            size_t y_size = raw->primary_height_pixels * raw->primary_line_stride_bytes;
            size_t v_size = raw->secondary_height_pixels * raw->secondary_line_stride_bytes;
            size_t u_size = v_size;
            size_t total_size = y_size + u_size + v_size;

            if (packet.valid_length_bytes() < total_size) {
              Exit("packet.valid_length_bytes < total_size");
            }

            if (raw->secondary_start_offset < y_size) {
              Exit("raw.secondary_start_offset < y_size");
            }

            if (raw->tertiary_start_offset < y_size + v_size) {
              Exit("raw.tertiary_start_offset < y_size + v_size");
            }
            break;
          }
          default:
            Exit("fourcc != NV12 && fourcc != YV12");
        }
      }

      if (emit_frame) {
        // i420_bytes is in I420 format - Y plane first, then U plane, then V
        // plane.  The U and V planes are half size in both directions.  Each
        // plane is 8 bits per sample.
        uint32_t i420_stride = fbl::round_up(raw->primary_display_width_pixels, 2u);
        // When width is odd, we want a chroma sample for the right-most luma. 
        uint32_t uv_width = (raw->primary_display_width_pixels + 1) / 2;
        // When height is odd, we want a chroma sample for the bottom-most luma.
        uint32_t uv_height = (raw->primary_display_height_pixels + 1) / 2;
        uint32_t uv_stride = i420_stride / 2;
        std::unique_ptr<uint8_t[]> i420_bytes = std::make_unique<uint8_t[]>(
            i420_stride * raw->primary_display_height_pixels + uv_stride * uv_height * 2);
        switch (raw->fourcc) {
          case make_fourcc('N', 'V', '1', '2'): {
            // Y
            uint8_t* y_src = buffer.base() + packet.start_offset() +
                             raw->primary_start_offset;
            uint8_t* y_dst = i420_bytes.get();
            for (uint32_t y_iter = 0; y_iter < raw->primary_display_height_pixels;
                 y_iter++) {
              memcpy(y_dst, y_src, raw->primary_display_width_pixels);
              y_src += raw->primary_line_stride_bytes;
              y_dst += i420_stride;
            }
            // UV
            uint8_t* uv_src = buffer.base() + packet.start_offset() +
                              raw->secondary_start_offset;
            uint8_t* u_dst_line = y_dst;
            uint8_t* v_dst_line = u_dst_line + uv_stride * uv_height;
            for (uint32_t uv_iter = 0; uv_iter < uv_height; uv_iter++) {
              uint8_t* u_dst = u_dst_line;
              uint8_t* v_dst = v_dst_line;
              for (uint32_t uv_line_iter = 0; uv_line_iter < uv_width; ++uv_line_iter) {
                *u_dst++ = uv_src[uv_line_iter * 2];
                *v_dst++ = uv_src[uv_line_iter * 2 + 1];
              }
              uv_src += raw->primary_line_stride_bytes;
              u_dst_line += uv_stride;
              v_dst_line += uv_stride;
            }
            break;
          }
          case make_fourcc('Y', 'V', '1', '2'): {
            // Y
            uint8_t* y_src = buffer.base() + packet.start_offset() + raw->primary_start_offset;
            uint8_t* y_dst = i420_bytes.get();
            for (uint32_t y_iter = 0; y_iter < raw->primary_display_height_pixels; y_iter++) {
              memcpy(y_dst, y_src, raw->primary_display_width_pixels);
              y_src += raw->primary_line_stride_bytes;
              y_dst += i420_stride;
            }
            // UV
            uint8_t* v_src = buffer.base() + packet.start_offset() + raw->primary_start_offset +
                raw->primary_line_stride_bytes * raw->primary_height_pixels;
            uint8_t* u_src = v_src + (raw->primary_line_stride_bytes / 2) * (raw->primary_height_pixels / 2);
            uint8_t* u_dst = y_dst;
            uint8_t* v_dst = u_dst + uv_stride * uv_height;
            for (uint32_t uv_iter = 0; uv_iter < uv_height; uv_iter++) {
              memcpy(u_dst, u_src, uv_width);
              memcpy(v_dst, v_src, uv_width);
              u_dst += uv_stride;
              v_dst += uv_stride;
              u_src += raw->primary_line_stride_bytes / 2;
              v_src += raw->primary_line_stride_bytes / 2;
            }
            break;
          }
          default:
            Exit("Feeding EmitFrame not yet implemented for fourcc: %s", fourcc_to_string(raw->fourcc).c_str());
        }
        emit_frame(
          i420_bytes.get(),
          raw->primary_display_width_pixels,
          raw->primary_display_height_pixels,
          i420_stride,
          packet.has_timestamp_ish(),
          packet.has_timestamp_ish() ? packet.timestamp_ish() : 0);
      }

      if (frame_sink) {
        async::PostTask(
            fidl_loop->dispatcher(),
            [frame_sink,
             image_id = packet.header().packet_index() + kFirstValidImageId,
             &vmo = buffer.vmo(),
             vmo_offset = buffer.vmo_offset() + packet.start_offset() + raw->primary_start_offset,
             format, cleanup = std::move(cleanup)]() mutable {
              frame_sink->PutFrame(image_id, vmo, vmo_offset, format,
                                   [cleanup = std::move(cleanup)] {
                                     // The ~cleanup can run on any thread (the
                                     // current thread is main_loop's thread),
                                     // and codec_client is ok with that
                                     // (because it switches over to |loop|'s
                                     // thread before sending a Codec message).
                                     //
                                     // ~cleanup
                                   });
            });
      }
      // If we didn't std::move(cleanup) before here, then ~cleanup runs here.
    }
  end_of_output:;
    VLOGF("output thread done\n");
    // output thread done
    // ~raw_video_writer
  });

  // decode for a bit...  in_thread, loop, out_thread, and the codec itself are
  // taking care of it.

  // First wait for the input thread to be done feeding input data.  Before the
  // in_thread terminates, it'll have sent in a last empty EOS input buffer.
  VLOGF("before in_thread->join()...\n");
  in_thread->join();
  VLOGF("after in_thread->join()\n");

  // The EOS queued as an input buffer should cause the codec to output an EOS
  // output buffer, at which point out_thread should terminate, after it has
  // finalized the output file.
  VLOGF("before out_thread->join()...\n");
  out_thread->join();
  VLOGF("after out_thread->join()\n");

  // We wait for frame_sink to return all the frames for these reasons:
  //   * As of this writing, some noisy-in-the-log things can happen in Scenic
  //     if we don't.
  //   * We don't want to cancel display of any frames, because we want to see
  //     the frames on the screen.
  //   * We don't want the |cleanup| to run after codec_client is gone since the
  //     |cleanup| calls codec_client.
  //   * It's easier to grok if activity started by use_h264_decoder() is done
  //     by the time use_h264_decoder() returns, given use_h264_decoder()'s role
  //     as an overall sequencer.
  if (frame_sink) {
    OneShotEvent frames_done_event;
    fit::closure on_frames_returned = [&frames_done_event] {
      frames_done_event.Signal();
    };
    async::PostTask(fidl_loop->dispatcher(),
                    [frame_sink, on_frames_returned =
                                     std::move(on_frames_returned)]() mutable {
                      frame_sink->PutEndOfStreamThenWaitForFramesReturnedAsync(
                          std::move(on_frames_returned));
                    });
    // The just-posted wait will set frames_done using the main_loop_'s thread,
    // which is not this thread.
    FXL_LOG(INFO) << "waiting for all frames to be returned from Scenic...";
    frames_done_event.Wait(zx::deadline_after(zx::sec(30)));
    FXL_LOG(INFO) << "all frames have been returned from Scenic";
    // Now we know that there are zero frames in frame_sink, including zero
    // frame cleanup(s) in-flight (in the sense of a pending/running cleanup
    // that's touching codec_client to post any new work.  Work already posted
    // via codec_client can still be in flight.  See below.)
  }

  // Close the channels explicitly (just so we can more easily print messages
  // before and after vs. ~codec_client).
  VLOGF("before codec_client stop...\n");
  codec_client.Stop();
  VLOGF("after codec_client stop.\n");

  // success
  // ~codec_client
  return;
}

void use_h264_decoder(async::Loop* fidl_loop,
                      thrd_t fidl_thread,
                      fuchsia::mediacodec::CodecFactoryPtr codec_factory,
                      fidl::InterfaceHandle<fuchsia::sysmem::Allocator> sysmem,
                      InStreamPeeker* in_stream,
                      uint64_t min_output_buffer_size,
                      FrameSink* frame_sink,
                      EmitFrame emit_frame) {
  use_video_decoder(fidl_loop, fidl_thread, std::move(codec_factory), std::move(sysmem),
                    in_stream, Format::kH264, min_output_buffer_size,
                    frame_sink, std::move(emit_frame));
}

void use_vp9_decoder(async::Loop* fidl_loop,
                     thrd_t fidl_thread,
                     fuchsia::mediacodec::CodecFactoryPtr codec_factory,
                     fidl::InterfaceHandle<fuchsia::sysmem::Allocator> sysmem,
                     InStreamPeeker* in_stream,
                     uint64_t min_output_buffer_size,
                     FrameSink* frame_sink,
                     EmitFrame emit_frame) {
  use_video_decoder(fidl_loop, fidl_thread, std::move(codec_factory), std::move(sysmem),
                    in_stream, Format::kVp9, min_output_buffer_size,
                    frame_sink, std::move(emit_frame));
}
