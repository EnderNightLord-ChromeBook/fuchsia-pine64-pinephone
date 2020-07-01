// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::fidl_hanging_get_responder,
    crate::fidl_hanging_get_result_responder,
    crate::fidl_process,
    crate::fidl_processor::RequestContext,
    crate::switchboard::base::{
        SettingRequest, SettingResponse, SettingType, SystemLoginOverrideMode,
    },
    crate::switchboard::hanging_get_handler::Sender,
    fidl_fuchsia_settings::{
        SystemMarker, SystemRequest, SystemSetResponder, SystemSettings, SystemWatch2Responder,
        SystemWatchResponder,
    },
    fuchsia_async as fasync,
    futures::future::LocalBoxFuture,
    futures::prelude::*,
};

fidl_hanging_get_responder!(SystemSettings, SystemWatch2Responder, SystemMarker::DEBUG_NAME);

// TODO(fxb/52593): Remove when clients are ported to watch2.
fidl_hanging_get_result_responder!(SystemSettings, SystemWatchResponder, SystemMarker::DEBUG_NAME);

impl From<SettingResponse> for SystemSettings {
    fn from(response: SettingResponse) -> Self {
        if let SettingResponse::System(info) = response {
            let mut system_settings = fidl_fuchsia_settings::SystemSettings::empty();
            system_settings.mode =
                Some(fidl_fuchsia_settings::LoginOverride::from(info.login_override_mode));
            system_settings
        } else {
            panic!("incorrect value sent to system");
        }
    }
}

fidl_process!(
    System,
    SettingType::System,
    process_request,
    SystemWatch2Responder,
    process_request_2
);

// TODO(fxb/52593): Replace with logic from process_request_2
// and remove process_request_2 when clients ported to Watch2 and back.
async fn process_request(
    context: RequestContext<SystemSettings, SystemWatchResponder>,
    req: SystemRequest,
) -> Result<Option<SystemRequest>, anyhow::Error> {
    #[allow(unreachable_patterns)]
    match req {
        SystemRequest::Watch { responder } => {
            context.watch(responder, false).await;
        }
        _ => {
            return Ok(Some(req));
        }
    }

    return Ok(None);
}

async fn process_request_2(
    context: RequestContext<SystemSettings, SystemWatch2Responder>,
    req: SystemRequest,
) -> Result<Option<SystemRequest>, anyhow::Error> {
    #[allow(unreachable_patterns)]
    match req {
        SystemRequest::Set { settings, responder } => {
            if let Some(mode) = settings.mode {
                change_login_override(
                    context.clone(),
                    SystemLoginOverrideMode::from(mode),
                    responder,
                );
            }
        }
        SystemRequest::Watch2 { responder } => {
            context.watch(responder, true).await;
        }
        _ => {
            return Ok(Some(req));
        }
    }

    return Ok(None);
}

/// Sets the login mode and schedules accounts to be cleared. Upon success, the
/// device is scheduled to reboot so the change will take effect.
fn change_login_override(
    context: RequestContext<SystemSettings, SystemWatch2Responder>,
    mode: SystemLoginOverrideMode,
    responder: SystemSetResponder,
) {
    fasync::spawn(async move {
        if context
            .request(SettingType::System, SettingRequest::SetLoginOverrideMode(mode))
            .await
            .is_err()
        {
            responder.send(&mut Err(fidl_fuchsia_settings::Error::Failed)).ok();
            return;
        }

        if context
            .request(SettingType::Account, SettingRequest::ScheduleClearAccounts)
            .await
            .is_err()
        {
            responder.send(&mut Err(fidl_fuchsia_settings::Error::Failed)).ok();
            return;
        }

        if context.request(SettingType::Power, SettingRequest::Reboot).await.is_err() {
            responder.send(&mut Err(fidl_fuchsia_settings::Error::Failed)).ok();
            return;
        }

        responder.send(&mut Ok(())).ok();
    });
}

impl From<fidl_fuchsia_settings::LoginOverride> for SystemLoginOverrideMode {
    fn from(item: fidl_fuchsia_settings::LoginOverride) -> Self {
        match item {
            fidl_fuchsia_settings::LoginOverride::AutologinGuest => {
                SystemLoginOverrideMode::AutologinGuest
            }
            fidl_fuchsia_settings::LoginOverride::AuthProvider => {
                SystemLoginOverrideMode::AuthProvider
            }
            fidl_fuchsia_settings::LoginOverride::None => SystemLoginOverrideMode::None,
        }
    }
}

impl From<SystemLoginOverrideMode> for fidl_fuchsia_settings::LoginOverride {
    fn from(item: SystemLoginOverrideMode) -> Self {
        match item {
            SystemLoginOverrideMode::AutologinGuest => {
                fidl_fuchsia_settings::LoginOverride::AutologinGuest
            }
            SystemLoginOverrideMode::AuthProvider => {
                fidl_fuchsia_settings::LoginOverride::AuthProvider
            }
            SystemLoginOverrideMode::None => fidl_fuchsia_settings::LoginOverride::None,
        }
    }
}
