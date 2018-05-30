// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_LIB_LEDGER_CLIENT_TYPES_H_
#define PERIDOT_LIB_LEDGER_CLIENT_TYPES_H_

#include <algorithm>
#include <lib/fidl/cpp/array.h>
#include <lib/fidl/cpp/vector.h>

namespace fuchsia {
namespace modular {

using LedgerPageId = ledger::PageId;
using LedgerPageKey = fidl::VectorPtr<uint8_t>;
using LedgerToken = std::unique_ptr<ledger::Token>;

inline bool PageIdsEqual(const LedgerPageId& a, const LedgerPageId& b) {
  return a.id == b.id;
}

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_LIB_LEDGER_CLIENT_TYPES_H_
