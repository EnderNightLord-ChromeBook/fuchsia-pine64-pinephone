// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_VIDEO_AMLOGIC_DECODER_AMLOGIC_VIDEO_H_
#define GARNET_DRIVERS_VIDEO_AMLOGIC_DECODER_AMLOGIC_VIDEO_H_

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/amlogiccanvas.h>
#include <ddk/protocol/platform/device.h>
#include <ddk/protocol/sysmem.h>
#include <lib/zx/handle.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>

#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include "decoder_core.h"
#include "decoder_instance.h"
#include "device_ctx.h"
#include "firmware_blob.h"
#include "registers.h"
#include "stream_buffer.h"
#include "video_decoder.h"

class AmlogicVideo final : public VideoDecoder::Owner,
                           public DecoderCore::Owner,
                           public CanvasEntry::Owner {
 public:
  AmlogicVideo();

  ~AmlogicVideo();

  __WARN_UNUSED_RESULT zx_status_t InitRegisters(zx_device_t* parent);
  __WARN_UNUSED_RESULT zx_status_t InitDecoder();

  // VideoDecoder::Owner implementation.
  __WARN_UNUSED_RESULT DosRegisterIo* dosbus() override { return dosbus_.get(); }
  __WARN_UNUSED_RESULT zx_handle_t bti() override { return bti_.get(); }
  __WARN_UNUSED_RESULT DeviceType device_type() override { return device_type_; }
  __WARN_UNUSED_RESULT FirmwareBlob* firmware_blob() override { return firmware_.get(); }
  __WARN_UNUSED_RESULT std::unique_ptr<CanvasEntry> ConfigureCanvas(io_buffer_t* io_buffer,
                                                                    uint32_t offset, uint32_t width,
                                                                    uint32_t height, uint32_t wrap,
                                                                    uint32_t blockmode) override;

  __WARN_UNUSED_RESULT DecoderCore* core() override { return core_; }
  __WARN_UNUSED_RESULT zx_status_t AllocateIoBuffer(io_buffer_t* buffer, size_t size,
                                                    uint32_t alignment_log2,
                                                    uint32_t flags) override;
  __WARN_UNUSED_RESULT bool IsDecoderCurrent(VideoDecoder* decoder) override
      __TA_NO_THREAD_SAFETY_ANALYSIS {
    assert(decoder);
    return decoder == video_decoder_;
  }
  __WARN_UNUSED_RESULT zx_status_t SetProtected(ProtectableHardwareUnit unit,
                                                bool protect) override;

  // DecoderCore::Owner implementation.
  __WARN_UNUSED_RESULT
  MmioRegisters* mmio() override { return registers_.get(); }
  void UngateClocks() override;
  void GateClocks() override;

  // CanvasEntry::Owner implementation.
  void FreeCanvas(CanvasEntry* canvas) override;

  // The pts manager has its own locking, so don't worry about the video decoder
  // lock.
  __WARN_UNUSED_RESULT PtsManager* pts_manager() __TA_NO_THREAD_SAFETY_ANALYSIS {
    ZX_DEBUG_ASSERT(video_decoder_);
    return video_decoder_->pts_manager();
  }

  // Reset the current instance - only for use with single-stream decoders.
  void ClearDecoderInstance();

  // Erase a specific decoder. May switch to a different decoder in multi-stream
  // mode. This will stop and power off the core if the decoder is currently
  // running.
  void RemoveDecoder(VideoDecoder* decoder);

  __WARN_UNUSED_RESULT
  zx_status_t InitializeStreamBuffer(bool use_parser, uint32_t size);
  __WARN_UNUSED_RESULT
  zx_status_t InitializeEsParser();
  __WARN_UNUSED_RESULT
  zx_status_t ParseVideo(void* data, uint32_t len);
  // If parser_running_, try to cause WaitForParsingCompleted() to return
  // ZX_ERR_CANCELED ASAP.  If !parser_running_, do nothing.  The caller is
  // responsible for ensuring that only its own decoder's work is ever canceled.
  void TryStartCancelParsing();
  // Any error: The caller should call CancelParsing() to clean up.
  // ZX_ERR_CANCELED: TryStartCancelParsing() was called and the caller should
  //   call CancelParsing() to cancel the parsing, just as the caller
  //   does for any error from WaitForParsingCompleted().  This error code in
  //   this context can be thought of as ZX_ERR_YOU_SHOULD_CANCEL_PARSING_NOW.
  //   It's not an indication that parsing is already canceled, only that the
  //   caller should call CancelParsing().
  // ZX_OK: The parsing is done.  If the caller called TryStartCancelParsing()
  //   at some point, no harm done.  The caller should not call CancelParsing().
  __WARN_UNUSED_RESULT
  zx_status_t WaitForParsingCompleted(zx_duration_t deadline);
  void CancelParsing();
  __WARN_UNUSED_RESULT
  zx_status_t ProcessVideoNoParser(const void* data, uint32_t len, uint32_t* written_out = nullptr);

  __WARN_UNUSED_RESULT DecoderCore* hevc_core() const { return hevc_core_.get(); }
  __WARN_UNUSED_RESULT DecoderCore* vdec1_core() const { return vdec1_core_.get(); }

  // Add the instance as a swapped-out decoder.
  void AddNewDecoderInstance(std::unique_ptr<DecoderInstance> instance)
      __TA_REQUIRES(video_decoder_lock_);

  // For single-instance decoders, set the default instance.
  void SetDefaultInstance(std::unique_ptr<VideoDecoder> decoder, bool hevc)
      __TA_REQUIRES(video_decoder_lock_);
  __WARN_UNUSED_RESULT
  std::mutex* video_decoder_lock() __TA_RETURN_CAPABILITY(video_decoder_lock_) {
    return &video_decoder_lock_;
  }
  __WARN_UNUSED_RESULT
  VideoDecoder* video_decoder() __TA_REQUIRES(video_decoder_lock_) { return video_decoder_; }

  // This tries to schedule the next runnable decoder. It may leave the current
  // decoder scheduled if no other decoder is runnable.
  void TryToReschedule() __TA_REQUIRES(video_decoder_lock_);
  void TryToRescheduleAssumeVideoDecoderLocked() __TA_NO_THREAD_SAFETY_ANALYSIS {
    TryToReschedule();
  }

  __WARN_UNUSED_RESULT zx_status_t AllocateStreamBuffer(StreamBuffer* buffer, uint32_t size);

  // This gets started connecting to sysmem, but returns an InterfaceHandle
  // instead of InterfacePtr so that the caller can bind to the dispatcher.
  fidl::InterfaceHandle<fuchsia::sysmem::Allocator> ConnectToSysmem();

 private:
  friend class TestH264;
  friend class TestMpeg2;
  friend class TestVP9;
  friend class TestFrameProvider;

  void InitializeStreamInput(bool use_parser);

  __WARN_UNUSED_RESULT
  zx_status_t ProcessVideoNoParserAtOffset(const void* data, uint32_t len, uint32_t current_offset,
                                           uint32_t* written_out = nullptr);
  void InitializeInterrupts();
  void SwapOutCurrentInstance() __TA_REQUIRES(video_decoder_lock_);
  void SwapInCurrentInstance() __TA_REQUIRES(video_decoder_lock_);

  zx_device_t* parent_ = nullptr;
  pdev_protocol_t pdev_{};
  sysmem_protocol_t sysmem_{};
  amlogic_canvas_protocol_t canvas_{};
  DeviceType device_type_ = DeviceType::kUnknown;
  zx::handle secure_monitor_;
  std::unique_ptr<CbusRegisterIo> cbus_;
  std::unique_ptr<DosRegisterIo> dosbus_;
  std::unique_ptr<HiuRegisterIo> hiubus_;
  std::unique_ptr<AoRegisterIo> aobus_;
  std::unique_ptr<DmcRegisterIo> dmc_;
  std::unique_ptr<ResetRegisterIo> reset_;
  std::unique_ptr<DemuxRegisterIo> demux_;
  std::unique_ptr<ParserRegisterIo> parser_;

  std::unique_ptr<MmioRegisters> registers_;

  std::unique_ptr<FirmwareBlob> firmware_;

  std::unique_ptr<io_buffer_t> parser_input_;

  // This buffer holds an ES start code that's used to get an interrupt when the
  // parser is finished.
  io_buffer_t search_pattern_ = {};

  zx::handle bti_;

  // ZX_USER_SIGNAL_0 is for parser done.
  // ZX_USER_SIGNAL_1 is for client wants ParseVideo() to return
  //   ZX_ERR_CANCELED ASAP.
  //
  // Both must be un-signaled while parser_running_ is false (transients while
  // under parser_running_lock_ are fine).
  //
  // While parser_running_ is true, either can become signaled as appropriate.
  zx::event parser_finished_event_;

  std::mutex parser_running_lock_;
  bool parser_running_ = false;

  zx::handle parser_interrupt_handle_;
  zx::handle vdec0_interrupt_handle_;
  zx::handle vdec1_interrupt_handle_;

  std::thread parser_interrupt_thread_;
  std::thread vdec0_interrupt_thread_;
  std::thread vdec1_interrupt_thread_;

  std::unique_ptr<DecoderCore> hevc_core_;
  std::unique_ptr<DecoderCore> vdec1_core_;

  std::mutex video_decoder_lock_;
  // This is the video decoder that's currently attached to the hardware.
  __TA_GUARDED(video_decoder_lock_)
  VideoDecoder* video_decoder_ = nullptr;

  // This is the stream buffer that's currently attached to the hardware.
  StreamBuffer* stream_buffer_ = nullptr;

  // The decoder core for the currently-running decoder. It must be powered on.
  DecoderCore* core_ = nullptr;

  __TA_GUARDED(video_decoder_lock_)
  std::unique_ptr<DecoderInstance> current_instance_;
  __TA_GUARDED(video_decoder_lock_)
  std::list<std::unique_ptr<DecoderInstance>> swapped_out_instances_;
};

#endif  // GARNET_DRIVERS_VIDEO_AMLOGIC_DECODER_AMLOGIC_VIDEO_H_
