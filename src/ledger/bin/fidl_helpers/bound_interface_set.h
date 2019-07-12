// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LEDGER_BIN_FIDL_HELPERS_BOUND_INTERFACE_SET_H_
#define SRC_LEDGER_BIN_FIDL_HELPERS_BOUND_INTERFACE_SET_H_

#include <lib/fidl/cpp/binding_set.h>
#include <lib/fit/function.h>

#include "src/lib/fxl/macros.h"

namespace ledger {
namespace fidl_helpers {
template <class Interface, class Impl>
class BoundInterfaceSet {
 public:
  template <class... Args>
  explicit BoundInterfaceSet(Args&&... args) : impl_(std::forward<Args>(args)...) {}

  void AddBinding(fidl::InterfaceRequest<Interface> request) {
    binding_.AddBinding(&impl_, std::move(request));
  }

  void CloseAll() { binding_.CloseAll(); }

  void set_on_empty(fit::closure on_empty) { binding_.set_empty_set_handler(std::move(on_empty)); }

  bool is_bound() { return binding_.is_bound(); }

  size_t size() const { return binding_.size(); }

 private:
  Impl impl_;
  fidl::BindingSet<Interface> binding_;

  FXL_DISALLOW_COPY_AND_ASSIGN(BoundInterfaceSet);
};
}  // namespace fidl_helpers
}  // namespace ledger

#endif  // SRC_LEDGER_BIN_FIDL_HELPERS_BOUND_INTERFACE_SET_H_
