// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {crate::ast::BanjoAst, failure::Error, std::io};

pub use self::{
    ast::AstBackend, c::CBackend, cpp::CppBackend, cpp::CppInternalBackend, rust::RustBackend,
};

mod ast;
mod c;
mod cpp;
mod rust;

pub trait Backend<'a, W: io::Write> {
    fn codegen(&mut self, ast: BanjoAst) -> Result<(), Error>;
}
