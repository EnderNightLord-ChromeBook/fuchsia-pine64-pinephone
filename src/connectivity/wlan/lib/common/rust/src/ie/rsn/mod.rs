// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod akm;
pub mod cipher;
mod pmkid;
pub mod rsne;
pub mod suite_filter;
pub mod suite_selector;

pub use suite_selector::OUI;

use failure::Fail;

#[derive(Debug, Fail)]
pub enum Error {
    #[fail(display = "unexpected IO error: {}", _0)]
    UnexpectedIoError(#[cause] std::io::Error),
    #[fail(display = "invalid OUI length; expected 3 bytes but received {}", _0)]
    InvalidOuiLength(usize),
    #[fail(display = "invalid PMKID length; expected 16 bytes but received {}", _0)]
    InvalidPmkidLength(usize),
}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::UnexpectedIoError(e)
    }
}
