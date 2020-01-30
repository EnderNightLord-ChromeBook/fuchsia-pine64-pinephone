// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::util::*;

fn start_provider_with_ephemeral_fonts() -> Result<(App, fonts::ProviderProxy), Error> {
    start_provider_with_manifest("ephemeral.font_manifest.json", false)
}

#[fasync::run_singlethreaded(test)]
async fn test_ephemeral_get_font_family_info() -> Result<(), Error> {
    let (_app, font_provider) = start_provider_with_ephemeral_fonts()?;

    let mut family = fonts::FamilyName { name: "Ephemeral".to_string() };

    let response = font_provider.get_font_family_info(&mut family).await?;

    assert_eq!(response.name, Some(family));
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn test_ephemeral_get_typeface() -> Result<(), Error> {
    let (_app, font_provider) = start_provider_with_ephemeral_fonts()?;

    let family = Some(fonts::FamilyName { name: "Ephemeral".to_string() });
    let query = Some(fonts::TypefaceQuery {
        family,
        style: None,
        code_points: None,
        languages: None,
        fallback_family: None,
    });
    let request = fonts::TypefaceRequest { query, flags: None, cache_miss_policy: None };

    let response = font_provider.get_typeface(request).await?;

    assert!(response.buffer.is_some(), "{:?}", response);
    assert_eq!(response.buffer_id.unwrap(), 0, "{:?}", response);
    assert_eq!(response.font_index.unwrap(), 0, "{:?}", response);
    Ok(())
}
