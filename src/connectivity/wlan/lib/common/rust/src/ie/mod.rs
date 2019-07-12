// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod rsn;
pub mod wpa;

mod constants;
mod fields;
mod id;
mod parse;
mod reader;
mod write;

use zerocopy::{AsBytes, FromBytes, Unaligned};

pub use {constants::*, fields::*, id::*, parse::*, reader::Reader, write::*};

#[repr(C, packed)]
#[derive(AsBytes, FromBytes, Unaligned)]
pub struct Header {
    pub id: Id,
    pub body_len: u8,
}
