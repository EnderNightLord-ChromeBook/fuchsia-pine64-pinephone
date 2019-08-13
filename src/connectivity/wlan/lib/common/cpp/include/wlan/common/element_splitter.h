// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_ELEMENT_SPLITTER_H_
#define SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_ELEMENT_SPLITTER_H_

#include <tuple>

#include <fbl/span.h>
#include <wlan/common/element_id.h>

namespace wlan {
namespace common {

class ElementIterator;

bool operator==(const ElementIterator& a, const ElementIterator& b);
bool operator!=(const ElementIterator& a, const ElementIterator& b);

class ElementIterator {
 public:
  friend bool wlan::common::operator==(const ElementIterator& a, const ElementIterator& b);
  friend bool wlan::common::operator!=(const ElementIterator& a, const ElementIterator& b);

  explicit ElementIterator(fbl::Span<const uint8_t> buffer);

  std::tuple<element_id::ElementId, fbl::Span<const uint8_t>> operator*() const;

  ElementIterator& operator++();

 private:
  fbl::Span<const uint8_t> remaining_;
};

class ElementSplitter {
 public:
  explicit ElementSplitter(fbl::Span<const uint8_t> buffer) : buffer_(buffer) {}

  ElementIterator begin() const { return ElementIterator(buffer_); }

  ElementIterator end() const { return ElementIterator(buffer_.subspan(buffer_.size())); }

 private:
  fbl::Span<const uint8_t> buffer_;
};

}  // namespace common
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_ELEMENT_SPLITTER_H_
