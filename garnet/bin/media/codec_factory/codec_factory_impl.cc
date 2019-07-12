// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "codec_factory_impl.h"

#include <algorithm>

#include "lib/svc/cpp/services.h"

namespace {

// Other types of SW isolates can exist, but at the moment we only have one,
// which uses ffmpeg for SW decode (or potentially encode).
//
// For HW-based codecs, we discover their "LocalCodecFactory" by watching for
// their device and sending the server end of a (local) CodecFactory to the
// driver.
const std::string kIsolateUrlFfmpeg =
    "fuchsia-pkg://fuchsia.com/codec_runner_sw_ffmpeg#meta/"
    "codec_runner_sw_ffmpeg.cmx";
const std::string kIsolateUrlSbc =
    "fuchsia-pkg://fuchsia.com/codec_runner_sw_sbc#meta/"
    "codec_runner_sw_sbc.cmx";
const std::string kIsolateUrlAac =
    "fuchsia-pkg://fuchsia.com/codec_runner_sw_aac#meta/"
    "codec_runner_sw_aac.cmx";

struct EncoderSupportSpec {
  std::string isolate_url;
  std::vector<std::string> mime_types;
  std::function<bool(const fuchsia::media::EncoderSettings&)> supports_settings;
  bool supports_mime_type(const std::string& mime_type) const {
    return std::find(mime_types.begin(), mime_types.end(), mime_type) !=
           mime_types.end();
  }
  bool supports(const std::string& mime_type,
                const fuchsia::media::EncoderSettings& settings) const {
    return supports_mime_type(mime_type) && supports_settings(settings);
  }
};

const EncoderSupportSpec kSbcEncoderSupportSpec = {
    .isolate_url = kIsolateUrlSbc,
    .mime_types = {"audio/pcm"},
    .supports_settings =
        [](const fuchsia::media::EncoderSettings& settings) {
          return settings.is_sbc();
        },
};

const EncoderSupportSpec kAacEncoderSupportSpec = {
    .isolate_url = kIsolateUrlAac,
    .mime_types = {"audio/pcm"},
    .supports_settings =
        [](const fuchsia::media::EncoderSettings& settings) {
          return settings.is_aac();
        },
};

const std::vector<EncoderSupportSpec> supported_encoders = {
    kSbcEncoderSupportSpec, kAacEncoderSupportSpec};

const std::vector<std::string> kFfmpegDecoderMimeTypes = {"video/h264"};

bool FfmpegDecoderSupportsFormat(std::string mime_type) {
  for (auto& supported : kFfmpegDecoderMimeTypes) {
    if (supported == mime_type) {
      return true;
    }
  }

  return false;
}

std::optional<std::string> FindEncoder(
    const std::string& mime_type,
    const fuchsia::media::EncoderSettings& settings) {
  auto encoder =
      std::find_if(supported_encoders.begin(), supported_encoders.end(),
                   [&mime_type, &settings](const EncoderSupportSpec& encoder) {
                     return encoder.supports(mime_type, settings);
                   });

  if (encoder == supported_encoders.end()) {
    return std::nullopt;
  }

  return {encoder->isolate_url};
}

void ForwardToIsolate(
    std::string component_url, component::StartupContext* startup_context,
    std::function<void(fuchsia::mediacodec::CodecFactoryPtr)> connect_func) {
  fuchsia::sys::ComponentControllerPtr component_controller;
  component::Services services;
  fuchsia::sys::LaunchInfo launch_info{};
  launch_info.url = component_url;
  launch_info.directory_request = services.NewRequest();
  startup_context->launcher()->CreateComponent(
      std::move(launch_info), component_controller.NewRequest());
  component_controller.set_error_handler([component_url](zx_status_t status) {
    FXL_LOG(ERROR) << "app_controller_ error connecting to CodecFactoryImpl of "
                   << component_url;
  });
  fuchsia::mediacodec::CodecFactoryPtr factory_delegate;
  services.ConnectToService(
      factory_delegate.NewRequest().TakeChannel(),
      // TODO(dustingreen): Might be helpful (for debugging maybe) to change
      // this name to distinguish these delegate CodecFactory(s) from the main
      // CodecFactory service.
      fuchsia::mediacodec::CodecFactory::Name_);

  // Forward the request to the factory_delegate_ as-is.  This avoids conversion
  // to command-line parameters and back, and avoids creating a separate
  // interface definition for the delegated call.  The downside is potential
  // confusion re. why we have several implementations of CodecFactory, but we
  // can comment why.  The presently-running implementation is the main
  // implementation that clients use directly.

  // Dropping factory_delegate in here is ok; messages will be received in order
  // by the peer before they see the PEER_CLOSED event.
  connect_func(std::move(factory_delegate));

  // We don't want to be forced to keep component_controller around.  When using
  // an isolate, we trust that the ComponentController will kill the app if we
  // crash before this point, as this process crashing will kill the server side
  // of the component_controller.  If we crash after this point, we trust that
  // the isolate will receive the CreateDecoder() message sent just above, and
  // will either exit on failure to create the Codec server-side, or will exit
  // later when the client side of the Codec channel closes, or will exit later
  // when the Codec fails asynchronously in whatever way. Essentially the Codec
  // channel owns the isolate at this point, and we trust the isolate to exit
  // when the Codec channel closes.
  component_controller->Detach();
}

}  // namespace

namespace codec_factory {

// TODO(dustingreen): Currently we assume, potentially incorrectly, that clients
// of CodecFactory won't spam CodecFactory channel creation.  Rather than trying
// to mitigate that problem locally in this class, it seems better to intergrate
// with a more general-purpose request spam mitigation mechanism.
void CodecFactoryImpl::CreateSelfOwned(
    CodecFactoryApp* app, component::StartupContext* startup_context,
    zx::channel request) {
  // I considered just doing "new CodecFactoryImpl(...)" here and declaring that
  // it always inherently owns itself (and implementing it that way), but that
  // seems less flexible for testing purposes and also not necessarily as safe
  // if we were to add any error cases before the Binding has taken over
  // ownership.
  //
  // As usual, can't use std::make_unique<> here since making it a friend would
  // break the point of making the constructor private.
  std::unique_ptr<CodecFactoryImpl> self(
      new CodecFactoryImpl(app, startup_context, std::move(request)));
  auto* self_ptr = self.get();
  self_ptr->OwnSelf(std::move(self));
  assert(!self);
}

CodecFactoryImpl::CodecFactoryImpl(CodecFactoryApp* app,
                                   component::StartupContext* startup_context,
                                   zx::channel channel)
    : app_(app),
      startup_context_(startup_context),
      channel_temp_(std::move(channel)) {}

// TODO(dustingreen): Seems simpler to avoid channel_temp_ and OwnSelf() and
// just have CreateSelfOwned() directly create the binding.
void CodecFactoryImpl::OwnSelf(std::unique_ptr<CodecFactoryImpl> self) {
  binding_ = std::make_unique<BindingType>(
      std::move(self), std::move(channel_temp_), app_->loop()->dispatcher());
  binding_->set_error_handler([this](zx_status_t status) {
    FXL_LOG(INFO) << "CodecFactoryImpl channel failed (INFO) - status: "
                  << status;
    // this will also ~this
    binding_.reset();
  });
}

void CodecFactoryImpl::CreateDecoder(
    fuchsia::mediacodec::CreateDecoder_Params params,
    fidl::InterfaceRequest<fuchsia::media::StreamProcessor> decoder) {
  if (!params.has_input_details()) {
    FXL_LOG(WARNING) << "missing input_details";
    return;
  }

  if (!params.input_details().has_mime_type()) {
    FXL_LOG(WARNING) << "input details missing mime type";
    // Without mime_type we cannot search for a decoder.
    return;
  }

  // We don't have any need to bind the codec_request locally to this process.
  // Instead, we find where to delegate the request to.

  // First, try to find a hw-accelerated codec to satisfy the request.
  const fuchsia::mediacodec::CodecFactoryPtr* factory =
      app_->FindHwDecoder([&params](const fuchsia::mediacodec::CodecDescription&
                                        hw_codec_description) -> bool {
        // TODO(dustingreen): pay attention to the bool constraints of the
        // params vs. the hw_codec_description bools.  For the moment we just
        // match the codec_type, mime_type.
        constexpr fuchsia::mediacodec::CodecType codec_type =
            fuchsia::mediacodec::CodecType::DECODER;
        return (codec_type == hw_codec_description.codec_type) &&
               (params.input_details().mime_type() ==
                hw_codec_description.mime_type);
      });
  if (factory) {
    // prefer HW-accelerated
    (*factory)->CreateDecoder(std::move(params), std::move(decoder));
    return;
  }

  if (params.has_require_hw() && params.require_hw()) {
    FXL_LOG(WARNING) << "require_hw, but no matching HW decoder factory found ("
                     << params.input_details().mime_type() << "); closing";
    // TODO(dustingreen): Send epitaph when possible.
    // ~decoder
    return;
  }

  std::string url;
  if (FfmpegDecoderSupportsFormat(params.input_details().mime_type())) {
    url = kIsolateUrlFfmpeg;
  } else {
    // ~decoder
    return;
  }

  ForwardToIsolate(
      url, startup_context_,
      [&params, &decoder](
          fuchsia::mediacodec::CodecFactoryPtr factory_delegate) mutable {
        // Forward the request to the factory_delegate_ as-is. This
        // avoids conversion to command-line parameters and back,
        // and avoids creating a separate interface definition for
        // the delegated call.  The downside is potential confusion
        // re. why we have several implementations of CodecFactory,
        // but we can comment why.  The presently-running
        // implementation is the main implementation that clients
        // use directly.
        factory_delegate->CreateDecoder(std::move(params), std::move(decoder));
      });
}

void CodecFactoryImpl::CreateEncoder(
    fuchsia::mediacodec::CreateEncoder_Params encoder_params,
    ::fidl::InterfaceRequest<fuchsia::media::StreamProcessor> encoder_request) {
  if (!encoder_params.has_input_details()) {
    FXL_LOG(WARNING) << "missing input_details";
    return;
  }

  if (!encoder_params.input_details().has_mime_type()) {
    FXL_LOG(WARNING) << "missing mime_type";
    return;
  }

  if (!encoder_params.input_details().has_encoder_settings()) {
    FXL_LOG(WARNING) << "missing encoder_settings";
    return;
  }

  if (encoder_params.has_require_hw() && encoder_params.require_hw()) {
    FXL_LOG(WARNING) << "There are no hardware encoders yet.";
    return;
  }

  auto maybe_encoder_isolate_url =
      FindEncoder(encoder_params.input_details().mime_type(),
                  encoder_params.input_details().encoder_settings());
  if (!maybe_encoder_isolate_url) {
    FXL_LOG(WARNING) << "No encoder supports "
                     << encoder_params.input_details().mime_type()
                     << " input with these settings.";
    return;
  }

  ForwardToIsolate(
      *maybe_encoder_isolate_url, startup_context_,
      [&encoder_params, &encoder_request](
          fuchsia::mediacodec::CodecFactoryPtr factory_delegate) mutable {
        factory_delegate->CreateEncoder(std::move(encoder_params),
                                        std::move(encoder_request));
      });
}

}  // namespace codec_factory
