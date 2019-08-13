// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/lazy_symbol.h"

#include "src/developer/debug/zxdb/symbols/symbol.h"
#include "src/developer/debug/zxdb/symbols/symbol_factory.h"

namespace zxdb {

namespace {

// Singleton null symbol to return when a LazySymbol is invalid.
fxl::RefPtr<Symbol> null_symbol;

}  // namespace

LazySymbol::LazySymbol() = default;
LazySymbol::LazySymbol(const LazySymbol& other) = default;
LazySymbol::LazySymbol(LazySymbol&& other) = default;
LazySymbol::LazySymbol(fxl::RefPtr<SymbolFactory> factory, void* factory_data_ptr,
                       uint32_t factory_data_offset)
    : factory_(std::move(factory)),
      factory_data_ptr_(factory_data_ptr),
      factory_data_offset_(factory_data_offset) {}
LazySymbol::LazySymbol(const Symbol* symbol) : symbol_(RefPtrTo(symbol)) {}
LazySymbol::~LazySymbol() = default;

LazySymbol& LazySymbol::operator=(const LazySymbol& other) = default;
LazySymbol& LazySymbol::operator=(LazySymbol&& other) = default;

const Symbol* LazySymbol::Get() const {
  if (!symbol_.get()) {
    if (is_valid()) {
      symbol_ = factory_->CreateSymbol(factory_data_ptr_, factory_data_offset_);
    } else {
      // Return the null symbol. Don't populate symbol_ for this case because it will mean
      // is_valid() will always return true.
      if (!null_symbol)
        null_symbol = fxl::MakeRefCounted<Symbol>();
      return null_symbol.get();
    }
  }
  return symbol_.get();
}

}  // namespace zxdb
