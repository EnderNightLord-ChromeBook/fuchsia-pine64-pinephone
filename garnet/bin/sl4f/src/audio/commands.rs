// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::audio::{facade::AudioFacade, types::AudioMethod};
use failure::Error;
use serde_json::Value;
use std::sync::Arc;

// Takes SL4F method command and executes corresponding Audio Client methods.
pub async fn audio_method_to_fidl(
    method_name: String,
    args: Value,
    facade: Arc<AudioFacade>,
) -> Result<Value, Error> {
    match method_name.parse()? {
        AudioMethod::PutInputAudio => facade.put_input_audio(args).await,
        AudioMethod::StartInputInjection => facade.start_input_injection(args).await,
        AudioMethod::StopInputInjection => facade.stop_input_injection().await,

        AudioMethod::StartOutputSave => facade.start_output_save().await,
        AudioMethod::StopOutputSave => facade.stop_output_save().await,
        AudioMethod::GetOutputAudio => facade.get_output_audio().await,
    }
}
